# ADR 0002: Colour page — wheels, curves, scopes and gallery thumbnails

Status: accepted (2026-10-21)
Extends: [ADR 0001](0001-color-grading-yuv-table.md)
Code: `windows/core/src/color_grade.{h,cpp}`, `windows/core/src/scopes.cpp`,
public contract in `owc/client.h` (`ColorWheel`, `CurvePoint`, `ColorGrade`),
`owc/color_grade.h`, `owc/scopes.h`.

## Context
The colour page grows into a DaVinci-Resolve-style one: lift / gamma / gain /
offset wheels with a contrast pivot, custom curves (Y, R, G, B), hue vs hue,
hue vs sat, luma vs sat, video scopes and a LUT gallery with live thumbnails.
ADR 0001's constraint holds: per-frame cost must not change, neutral grades
must stay exact identities.

## Decision
1. **Same architecture.** New controls only extend the RGB pipeline that is
   baked into the YUV table; the per-frame kernel is untouched.
   Order: WB -> exposure -> offset -> lift -> gamma -> gain -> contrast (pivot)
   -> shadows/highlights -> curves (Y then R/G/B) -> hue vs hue -> hue vs sat
   -> luma vs sat -> saturation/vibrance -> LUT.
2. **Per-channel stages folded.** WB, exposure, offset, lift, gamma, gain and
   contrast are all per channel: one 4096-entry table per channel (was two
   lookups). Formulas exactly as documented in `client.h` (per channel c, m =
   master): offset `x + 0.25(m+c)`, lift `x + 0.25(m+c)(1-x)`, gamma
   `x^(2^-(m+c))` (x <= 0 passes through), gain `x * 2^(m+c)`. Values may
   leave 0..1 between stages; contrast only reshapes 0..1 (outside passes
   through) and pivots via `x^a`, `a = log 0.5 / log pivot` (pivot 0.5 = the
   previous curve exactly). Custom curves: a second per-channel table
   (channel curve of the Y curve), only on channels with a non-neutral curve.
3. **Curves** (`PreparedCurve`, the same code behind `evalCurve` /
   `evalHueCurve` / `evalLumaSatCurve`, so the UI draws exactly what is
   applied): points clamped, sorted, merged within 1e-4 in x; monotone cubic
   Hermite with Fritsch-Carlson tangents in the Fritsch-Butland (PCHIP)
   form, shape-preserving one-sided ends. Custom curves get implicit (0,0) /
   (1,1) ends when missing (a single point bends the identity through it).
   Hue curves: same tangents on a periodic knot sequence. Luma vs sat: flat
   beyond its end points. A curve is neutral when every point is on the
   diagonal (custom) or at y = 0.5 (others); neutral stages are skipped, so a
   grade with every field present-but-neutral is an exact identity.
4. **Hue space.** Hue curves work on the BT.709 opponent chroma (the one the
   looks and vibrance already use): luma kept, Cb/Cr rotated / scaled. Hue x
   places R, Yl, G, Cy, B, Mg at 0, 1/6 .. 5/6 (piecewise linear in Cb/Cr
   angle between them) so the UI can draw an HSV rainbow under the curve.
   Both hue curves key on the incoming hue (like curves of one Resolve
   node). The hue-keyed effects fade out between chroma 0.03 and 0.004
   (grain around grey is not rotated or boosted by hue noise). Sat gain of a
   curve y: `2^(4(y-0.5))`, minus `0.25(1-2y)^2` below 0.5 so y = 0 is
   exactly grey (C1 at 0.5, monotonic).
5. **Hue tables over angle.** The hue curves are tabulated per bake over the
   Cb/Cr angle (1024 steps): rotation as cos/sin, saturation gain as a
   factor. Per sample: one branchless polynomial atan2 (1e-5 rad) and three
   lerps. Hue mapping, sin and cos per sample (with their data-dependent
   branches) cost 2x more on varied content.
6. **Wheels.** `wheelToRgb`: the puck direction is the vectorscope direction
   (BT.709 Cb right, Cr up) of the push, i.e. d = zero-sum RGB whose chroma
   points along the puck, scaled so `max |d| = |p|`. R ~103 deg, Yl ~175,
   G ~230, Cy ~283, B ~355, Mg ~50: the wheels line up with
   `vectorscopeTargets()`. `rgbToWheel` is its exact inverse on zero-sum input
   (others: mean removed first).
7. **Thumbnails** (`renderGradedRgba`): direct per-pixel pipeline (no table),
   1024-entry 1D tables (prepare 4x cheaper), the LUT-less part of the last
   grade cached (`GradePipeline::setLut` swaps only the LUT), `.cube` files
   through a process-wide cache keyed by path + modification stamp (also used
   by the bake worker and `exportCube`), bounded at 192 MB, LRU.
   `downscaleNV12`: exact box filter.
8. **Scopes** (`computeScopes`): one pass over the sampled pixels into
   uint32 density counters, then log tone mapping per scope normalised by the
   expected count per bin. Waveform / parade counters column-major (image row
   order walks the columns: cache friendly); vectorscope and histogram
   counters doubled by sample parity (flat areas would serialise on one
   counter); per-thread scratch reused (fresh multi-MB buffers page-fault on
   every call); count -> RGBA tables. Graticules are the UI's job.

## Measured (same laptop as ADR 0001, Release; CPU reported at 1.4 GHz, noisy)
- Bake with everything on (4 wheels, pivot, 4 curves, 3 hue curves,
  WB/exposure/tone/vibrance, 33^3 `.cube` at 0.8): 25-27 ms on 4 threads,
  80-90 ms single thread (target < 40 ms). Basic grade: 17-20 ms (unchanged).
- Apply 1080p, all on: 1.8 ms best / 2.1-2.4 avg (same as before: table-based).
- Thumbnail 192x108, all on, cached `.cube`: 3.8 ms best / 3.9-4.1 avg, one thread.
  Downscale 1080p -> 192x108: 1.5 ms. Thumbnail vs table path on random
  in-gamut colours: mean 0.64 codes, 0.7% > 4 codes, max 11 (saturated
  colours at the gamut edge, where the table's bilinear chroma is coarse).
- Scopes 1080p, step 2: 4.6-5.1 ms best / 5.1-5.6 avg, one thread.

## Trade-offs
- Folding the per-channel stages into one table changes rounding of the
  previous two-lookup path by < 1e-4; existing identity tests are exact
  because neutral stages are skipped, not evaluated.
- Default pivot 0.435 (Resolve) moves the contrast centre slightly darker
  than the previous 0.5: existing saved grades with contrast look marginally
  different. Pivot 0.5 restores the old curve bit for bit.
- Custom curves clamp to 0..1 (Resolve's are display-referred too); gain or
  offset headroom above 1 is lost on channels that have a curve.
- Hue x is HSV-like (equal spacing of primaries/secondaries) rather than
  uniform in Cb/Cr angle: matches what users expect on the curve's x axis at
  the cost of small slope kinks at the six anchors (C0, invisible in practice).
- Hue fade near grey means hue curves never affect chroma < 0.004 and only
  partly up to 0.03 (very desaturated colours react less than in Resolve).
- Thumbnails use 1024-entry tables: up to ~1 code differences near black vs
  the live grade, invisible at 192x108.
- `computeScopes` keeps ~1.5 MB of thread-local scratch per calling thread.
