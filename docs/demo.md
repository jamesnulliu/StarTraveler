# Demo

How the live demo switches the rungs the project compares, and a route through
`blue_star.yaml` for the 10-15 minutes. The keys and the overlay are also in
[Playing](playing.md#demo-presets). What each rung costs in rays is the question in
[TODO.md](../TODO.md); the other two branches measure it.

## What this is

The Render panel could already set every rung, and it already printed the GPU time. What
the demo needed was to change rung in one keypress while flying, and to read which rung
is on without looking at the panel.

Keys 1-7 write the rung into the same `RenderOptions` the panel edits. The bottom-left
overlay reads that state back and prints the GPU time the renderer reported for the last
frame. Nothing in the shading changes: a key is the panel, pressed once.

## Input

The keys work while flying (mouse captured, or the view hovered). They do nothing while a
text field has focus, while paused, or while Ctrl or Alt is down (Ctrl is thrust-down in
the cockpit).

| Key | Input that lands in `RenderOptions` |
|---|---|
| 1 | `rayTracing = false`. The GPU raster pipeline: shadow maps, constant ambient, no rays. |
| 2 | Hybrid, `TraceResolution::Half`, `DirectShadows::ShadowMaps`. |
| 3 | Hybrid, half, `DirectShadows::Traced`. The game's default. |
| 4 | Hybrid, `TraceResolution::Full`, traced shadows. |
| 5 | Preset 3, plus `ProbeSettings::enabled`. The ship's `ProbeVolume` (cockpit bounds, 6x4x10) is already on the ship. |
| 6 | Hybrid, full resolution, upscaler `DLSS-RR`. Where DLSS-RR cannot run, the renderer falls back to DLSS, then FSR, then TAAU. |
| 7 | `GpuPipeline::PathTracing`. Restarts whenever anything moves. |
| 0 | Toggles the overlay. Does not change the rung. |

Every preset also clears the mixes that are not part of a rung: radiance cache off, one
light candidate, denoiser temporal and spatial on, the hybrid debug view back to Final,
probes off except key 5. These are left alone, so the rungs stay comparable:

- upscaler quality and FSR sharpness;
- bloom, motion blur, vignette, lens, light shafts, grain;
- denoiser history, filter passes, color tolerance;
- shadow-map distance and resolution;
- probe spacing, rays and hysteresis (key 5 turns the probes on at those settings);
- the GPU time budget (8 ms).

Keys 1-5 and 7 put back the upscaler that was selected before key 6. Started from the
default, that is DLSS. Started with `--upscaler FSR`, it is FSR.

## Output

Bottom-left of the view, two lines:

```
3  Hybrid, half
GPU 9.31 ms    trace 6.40 ms
```

The first line is the preset number and `demoPresetName`. When the panel has left the
seven rungs, the line is `Custom` (radiance cache, a debug view, probes at full
resolution, shadow maps at full resolution, and so on). Ray tracing switched off in the
panel is preset 1. The path-tracing pipeline is preset 7.

The second line is `GpuRenderer::gpuTimeMs()` and `traceTimeMs()`, the same figures the
Render panel prints. The first frames of a run can read 0: the timestamp query has not
come back yet. With no GPU renderer the line is `GPU unavailable`.

The status bar also gets `Preset N: <name>` on each key.

## Contribution

This branch is the live half of the comparison. It adds no lighting method.

- `feature/eval-direct-light` and `feature/eval-denoiser` render the same rungs offline
  with `starcore-render` and score them against the path tracer. The keys are those rungs:
  raster, hybrid with shadow maps, hybrid at half and full, probes, DLSS-RR, path tracing.
- The overlay is the demo's copy of the GPU time those branches write to CSV. It is the
  whole frame and the trace pass, which is all the renderer times today.
- A clean `msvc` build is what the demo machine and the report's "builds from a clone"
  line both need. MinGW is not part of that check (`docs/building.md`).

The code that maps a key onto options is `StarTraveler/src/demo_presets.cpp`, covered by
`StarTraveler/tests/test_demo_presets.cpp` without a GPU. `GameLayer` only listens for the
keys and draws the overlay.

## Route

Scene `StarTraveler/assets/scenes/blue_star.yaml`, pilot view, about 12 minutes. The ship
starts at PlayerStart, looking down -Z. Stay in the cockpit for the first look at each
rung (the canopy is where indirect light and reflections show), then leave it. Press the
keys at the pauses; say the overlay's name and its milliseconds.

1. **Cockpit, still (about 2 min).** Look around the canopy without thrusting. Key 1
   (raster: the glass is a flat blend, no bounce), then 3 (the default: traced shadows,
   reflections in the canopy), then 7 for a moment and back to 3 before moving. Path
   tracing restarts as soon as the head moves, which is the point of showing it.
2. **Out, toward the glass and the liquid metal (about 3 min).** Thrust forward. Prism
   and Amethyst are the glass a short way ahead and to the left; Quicksilver is the
   liquid-metal blob. Slow down beside them. Keys 3 and 4 (half against full: less noise
   while the ship is moving, more GPU time). Key 2 here too: shadow maps, no lag, a
   harder penumbra.
3. **Mirror Blade (about 3 min).** It sits to the right, farther out. A slow pass so the
   blade holds a second sun. Keys 3 and 6 (the engine's denoiser against DLSS-RR). Key 0
   if the overlay covers the reflection, then 0 again.
4. **The glowing rocks (about 2 min).** Ember Core, just off the start, and the Violet
   Cluster on the Anvil. Park so one rock lights its neighbor. Key 5 (probes) against
   key 3: the bounced light comes from the probe grid instead of a path per pixel.
5. **A turn past the star (about 2 min).** Open the throttle, yaw so the blue star swings
   across the canopy, then cut thrust and let the image settle. The light changes; the
   history drops. Keys 1, 3 and 6 on the way out of the turn. The Coral (the bone
   lattice) and the Ring are the wide shapes farther along -Z if there is time left.

Free flight after the route: the same keys, C for the free camera.

## Clean build

Checked on a machine that had not built StarTraveler, with the `msvc` preset (Visual Studio
2026 Build Tools, not a full IDE). `ctest --preset msvc-release`: 161 tests passed, including
`StarTraveler/tests/test_demo_presets.cpp`. MinGW was not checked.

Two things the old docs did not say, now in [Building](building.md):

- CMake has to verify the FSR and DLSS downloads. Kitware's CMake does. The CMake from the
  WinLibs package fails that TLS check. A failed download used to abort the configure on
  CMake 4 (`EXPECTED_HASH` on an empty file); it now leaves the upscaler out, as the docs
  already claimed.
- The clone has to sit on an ASCII path. MSBuild runs glslang from a `cmd` batch file, and a
  path the system code page cannot encode fails every shader (`unable to open input file`,
  `VCEnd`). The check used `C:\Users\Lrf81\src\StarTraveler` for that reason.

## Frame times

The default window is 1600x900. With the default panel layout the view, which is what the
overlay times, is **930x844**. Measured there, in the cockpit of `blue_star.yaml` at rest
(the ship at PlayerStart, nothing moving), vsync off, DLSS Quality except path tracing
(it does not upscale). NVIDIA GeForce RTX 4060 Laptop GPU, driver 581.29. Five runs of
each preset, order alternated, median of the last frame of 160. The game's 8 ms budget
picks the samples per frame, so the ray-traced rungs land near that budget and differ in
how many samples they fit, not in the frame time.

| Key | Preset | GPU ms | Trace ms | Samples / frame | FPS (1000 / GPU ms) |
|---|---|---:|---:|---:|---:|
| 1 | Raster, no rays | 2.17 | 0.00 | 1 | 461 |
| 2 | Hybrid, half, shadow maps | 7.63 | 4.38 | 3 | 131 |
| 3 | Hybrid, half | 7.10 | 3.82 | 4 | 141 |
| 4 | Hybrid, full | 7.08 | 3.83 | 1 | 141 |
| 5 | Hybrid, half, probes | 7.19 | 2.28 | 3 | 139 |
| 6 | DLSS-RR | 7.66 | 4.05 | 1 | 131 |
| 7 | Path tracing | 5.88 | 5.48 | 2 | 170 |

`--preset N` starts on that rung, so the runs did not depend on the number keys. Each
column is its own median: the GPU time, the trace time and the sample count of one row
are not always the same run.

The FPS column is 1000 divided by the GPU time. That is the overlay's number turned
into a rate, for a frame the GPU limits. Vsync was off. It is not a stopwatch around
the whole window.

Raster never traces (the trace column is 0). A frame is the shadow maps, the DLSS
reconstruction and the effects: 2.17 ms, about 460 frames a second.

Presets 2 to 6 all ask for about 8 ms and stop when that budget is spent, so their
frame times sit between 7.1 and 7.7 ms. The budget is spent on samples after the
frame's other GPU work, using the previous frame's time per sample. What changes from
rung to rung is how many samples fit, not the frame rate:

- Half (preset 3) traces one pixel of each 2x2. Four samples fit in 7.10 ms, 3.82 of
  them tracing.
- Full (preset 4) traces every pixel. One sample already takes 3.83 ms of tracing, so
  the budget buys that one sample and the frame is 7.08 ms. Same rate as half, a
  quarter of the paths.
- Shadow maps (preset 2) still trace the paths. The maps are extra raster work, and
  three samples fit in 7.63 ms.
- Probes (preset 5) shorten the trace (median 2.28 ms, against 3.82 for preset 3): a
  diffuse hit takes bounced light from the grid. Updating the probes is the rest of
  the frame, which lands at 7.19 ms. Most runs fit three samples; one fit two.
- DLSS-RR (preset 6) traces every pixel and denoises while it upscales, so the budget
  buys one sample. It is the slowest hybrid rung here, 7.66 ms, and the upscaler in
  the log is DLSS-RR, not a fallback.

Path tracing does not upscale, so 5.88 ms is the full 930x844, and almost all of it
is the trace (5.48 ms). Two samples fit; a third would have passed the budget. It is
faster than the hybrid rungs in this still shot because those also denoise, upscale
and run the effects. Move the view and it throws the image away and starts again,
which this measurement does not show.

The progress table in `TODO.md` is a different measurement: 1280x720, `starcore-render`,
no 8 ms budget. Those milliseconds are not these frame rates.
