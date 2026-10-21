# ADR 0001: PC colour grading as a baked YUV-domain table

Status: accepted (2026-10-21)
Code: `windows/core/src/color_grade.{h,cpp}`, `Client::Impl::present()`.

## Context
`ClientOptions::color` (white balance, exposure, contrast, shadows/highlights,
saturation/vibrance, 3D/1D `.cube` LUT or built-in look) must reach every
consumer (shm/virtual camera/OBS, `onFrame`, latest frame) from the decode
thread, on NV12, without measurable latency, and with zero cost when off.

## Decision
1. The RGB pipeline is evaluated only at bake time and stored as one table in
   the YUV domain: 33x33 chroma nodes (U,V step 8, 0..256) x 256 Y values ->
   Y',U',V' as uint16 x64 (1.7 MB). Per 2x2 block: 4 bilinear UV weights once;
   each pixel's Y' = 4 loads at its exact Y (no interpolation along Y, so no
   banding); block U'V' from the same 4 rows at the block's mean Y.
2. Bake on one background worker (4 helper threads, below-normal priority),
   coalescing requests to the latest key; the decode thread swaps a
   `shared_ptr<const GradeTable>` under a mutex and never waits. Frames pass
   through ungraded until a table for the frame's matrix/range exists.
3. Apply on a persistent fan-out pool (min(4, hw/2) participants incl. the
   decode thread), in place when the frame is already our own copy (rotation
   buffer). Identity/disabled: no lock, no copy.
4. Out-of-gamut YUV (the RGB-cube excess) is preserved: luma excess as is,
   chroma excess through the grade's local chroma transform (complex ratio in
   the Cb/Cr plane, capped at 2x). Neutral grades stay identities on any input;
   B&W looks still produce grey.
5. Built-in looks are procedural RGB->RGB functions sampled once into cached
   33^3 LUTs, so they go through exactly the same path as `.cube` files.

## Measured (Intel laptop with Iris Xe, 8 logical cores, Release)
Apply 1080p: ~1.8 ms best / 2.2-2.6 ms avg. Bake: 46 ms (1 thread),
16 ms (4 threads). Identity `.cube`: max error 0 codes (Y and UV).

## Trade-offs
- 1.7 MB table vs. a 33^3 RGB LUT (0.4 MB): larger, but removes YUV<->RGB
  math per pixel and gives exact Y resolution; it fits in L2/L3.
- Chroma is bilinear on an 8-code grid: hue-dependent looks are smooth
  (equivalent to a 33-point LUT on chroma); not suitable for extremely
  sharp hue keys (not needed here).
- U'V' are evaluated at the block's mean Y (chroma is subsampled anyway).
- No SIMD: the scalar kernel already meets the 3 ms target. A gather-based
  AVX2 path is the next step if 4K grading is ever needed.
- Bake helpers are temporary threads (one bake per slider change, coalesced)
  instead of reusing the apply pool, so the decode thread never blocks on bake work.
- Excess chroma mapping costs nothing per frame (bake only) but adds a little
  complexity; without it, out-of-gamut colours (neon, LEDs) kept a tint in
  B&W looks.
