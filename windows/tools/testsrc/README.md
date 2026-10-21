# owc_testsrc and owc_shmview

Two dev tools for the OWC shared-memory frame bus (`windows/common/include/owc/shm.h`).

- `owc_testsrc` is a synthetic NV12 producer. It publishes color bars, a moving gray bar and a frame-counter strip.
- `owc_shmview` is a consumer. It reads the newest frame about every 1 ms, verifies the strip and prints stats.

## Build

From the repo root:

```
cmake -S windows -B build/tools -G "Visual Studio 16 2019" -A x64 -DOWC_BUILD_TESTS=OFF -DOWC_BUILD_CORE=OFF -DOWC_BUILD_APP=OFF -DOWC_BUILD_VIRTUALCAM=OFF -DOWC_BUILD_OBS_PLUGIN=OFF "-DOWC_TOOLS_ONLY=testsrc;shmview"
cmake --build build/tools --config Release
```

Binaries: `build/tools/out/Release/owc_testsrc.exe` and `build/tools/out/Release/owc_shmview.exe`.

## owc_testsrc

```
owc_testsrc [--width W] [--height H] [--fps F] [--seconds S]
```

| Option | Default | Notes |
|---|---|---|
| `--width` | 1280 | Even, 528..3840 (the strip needs at least 528 px) |
| `--height` | 720 | Even, 64..2160 |
| `--fps` | 30 | Integer, 1..240 |
| `--seconds` | 0 | 0 = run until Ctrl+C |

Frames are paced on an absolute schedule (frame n is due at start + n/fps), so the average rate is exact. A high-resolution waitable timer is used when available. While idle it refreshes the heartbeat at least every 50 ms. It prints one status line per second with the frame count and the actual fps. On exit it calls `setActive(false)` and closes the mapping.

### Frame content (NV12, BT.709 limited range)

- **Color bars:** 8 equal vertical bars, left to right: white, yellow, cyan, green, magenta, red, blue, black. They cover the full height above the strip area.
- **Gray bar:** 32 px wide, Y=180, U=V=128, full height above the strip area. Its left edge is at `x = (8*n) mod W`, where `n` is the producer's frame index. It moves 8 px per frame and wraps around.
- **Strip area:** the bottom 32 luma rows (`y = H-32 .. H-1`) are black (Y=16). Chroma rows covering them are U=V=128.

### Frame-counter strip (exact encoding)

`fn` is the frame number the bus assigns to the frame, `fn = header()->latest_frame_number + 1` read just before `publishNV12`. It is the value stored in `OwcShmSlot::frame_number`.

- 32 blocks, each 16x16 luma pixels.
- Block `i` (i = 0..31) covers `x = 16 + 16*i .. x + 15` and `y = H-32 .. H-17`.
- Bit `i` of `(uint32_t)fn` (LSB first) gives the block value. Bit 1 is white (Y=235). Bit 0 is black (Y=16).
- Chroma for the strip area is U=V=128.
- The strip is decoded by sampling the center pixel of each block: `(x = 16 + 16*i + 8, y = H-24)`. Y >= 128 means bit 1.
- The strip spans luma `x = 16 .. 527`. It occupies the bottom 32 rows, so the frame height must be at least 64.

## owc_shmview

```
owc_shmview [--seconds S]
```

- Retries `ShmReader::open()` every 500 ms and prints `waiting for producer` until it succeeds.
- Polls every 1 ms. Reads with `readLatest` into a buffer sized for 3840x2160 NV12, allocated once.
- Decodes the strip from the copied frame and compares it with `(uint32_t)info.frame_number`.
- Latency is `(qpcNow - info.publish_qpc) * 1000 / qpc_frequency`, measured after the copy.
- If the producer goes away (`producerAlive()` is false at a status tick), it closes its mapping and goes back to waiting.

Status line, once per second:

```
alive=1 nominal=1280x720@30.00 frames=61 fps=29.99 strip_ok=61 strip_mismatch=0 lat_avg=0.801ms lat_max=1.502ms missed=0
```

- `frames` is the cumulative count read. `strip_ok`, `strip_mismatch` and `missed` are cumulative.
- `fps`, `lat_avg` and `lat_max` cover the last second.
- `missed` counts gaps in `frame_number`.

## Test runs

```
owc_testsrc --seconds 8                              (background)
owc_shmview --seconds 5
owc_testsrc --width 1920 --height 1080 --fps 60 --seconds 6   (background)
owc_shmview --seconds 4
owc_shmview --seconds 2                              (no producer)
```
