# TODO

The project's goals, from our proposal and the instructor's reply. The question:
how much ray tracing does a rasterizer need to light a 3D scene convincingly in
real time? We compare ways to compute indirect light, in rising order of ray cost,
against the path-traced reference, measure FLIP error, GPU time per pass and
temporal stability along fixed camera paths, and switch between the methods live
in the demo.

- **Required** (instructor): the hybrid rendering pipeline (direct illumination)
  and some form of denoising. One part at a time; the rest as time allows.
- **Demo**: week 15, 10-15 minutes, flying through the scene.
- **Code + report**: finals week.

`[x]` is in the code, `[ ]` is to do.

## Scope

Both required parts are in the code and the demo is close, so the project adds no
new rendering method. It measures the methods that are there against the
path-traced reference, and shows them live in the demo. The rungs, with the first
numbers (the progress figures: one frame of the cockpit turning toward the Anvil,
1280x720, RTX 4060 Laptop GPU, LDR-FLIP against 8192 spp):

| Rung | `starcore-render` | GPU ms | FLIP |
|---|---|---:|---:|
| Raster pipeline: shadow maps, constant ambient, no rays | `--raster` | 2.27 | 0.436 |
| Path tracing, one path per pixel, no denoising | `--gpu` | 3.72 | 0.342 |
| Hybrid, Half trace resolution (the game's default) | `--hybrid --half` | 9.31 | 0.097 |
| Hybrid, Half, shadow maps for the direct light | `--hybrid --half --shadow-maps` | 10.17 | 0.094 |
| Hybrid, Half, irradiance probes | `--hybrid --half --probes` * | 11.50 | 0.100 |
| Hybrid, Full trace resolution | `--hybrid` | 14.79 | 0.074 |
| Hybrid, Full, radiance cache | `--hybrid --cache` | 16.84 | 0.076 |
| Hybrid denoised by DLSS-RR | `--hybrid --upscaler DLSS-RR` | | |
| Reference: the GPU path tracer, 8192 spp | `--gpu -f 2048 -s 4` | | |

\* plus `--probe-volume Ship 0 0.155 -0.15 1.65 1.09 3.05 6 4 10` for the cockpit.

The work is three branches, one per team member, below. What the old list had
beyond them is under [Out of scope](#out-of-scope), for after the deadlines.

## Order

1. The three branches, in parallel. `feature/demo-clean-build` cannot slip: the
   demo is week 15. `feature/eval-denoiser` uses the scripts of
   `feature/eval-direct-light`, so those land early.
2. The final numbers: both evaluations run again on one machine, in one go.
3. The demo (week 15), then the report (finals week).

## Branches

| Branch | Owner | What |
|---|---|---|
| `feature/demo-clean-build` | (to assign) | a build from a clean clone; the demo's keys, overlay |
| `feature/eval-direct-light` | (to assign) | the measuring scripts; direct light (required part 1) |
| `feature/eval-denoiser` | (to assign) | our denoiser against none and DLSS-RR (required part 2) |

For all three:

- One pull request into `main` per branch (`AGENTS.md`), its numbers in the body;
  the items below ticked in it.
- GPU times are compared only from one machine, nothing else using the GPU, runs
  alternated, medians (`AGENTS.md`, Measuring). Record the GPU, the driver and the
  resolution with every table.
- The hybrid pipeline and DLSS-RR need an NVIDIA RTX GPU (ray queries, DLSS);
  without one the hybrid configurations do not run.

### `feature/demo-clean-build`

First the build from a clean clone (it also sets up the owner's machine), then what
the demo needs. The Render panel switches every rung live today and shows the GPU
time; what is missing is switching them fast and showing which one is on.

- [x] Clean build: clone into a new folder on a machine that has not built
  StarTraveler; follow only `README.md` and `docs/building.md`; `msvc` preset;
  `ctest --preset msvc-release` passes. What the docs miss goes into
  `docs/building.md`. MinGW is not checked: say so there. Checked from a fresh
  tree at `C:\Users\Lrf81\src\StarTraveler` (VS 2026 Build Tools, Kitware CMake
  4.4.5, a new vcpkg clone): 161 tests passed. The workspace path is not ASCII,
  and MSBuild's shader step fails there; WinLibs' CMake cannot verify the
  upscaler downloads. Both are in `docs/building.md`. MinGW was not checked.
- [x] Demo presets: keys 1-7 set the Render panel's options in one press, while
  flying as well: 1 raster, no rays; 2 hybrid, Half, shadow maps; 3 hybrid, Half
  (the default); 4 hybrid, Full; 5 hybrid, Half, irradiance probes; 6 DLSS-RR
  (traces every pixel; the upscaler from before key 6 comes back with the others);
  7 path tracing (the reference; it restarts whenever anything moves). The number
  keys are free (`game_layer.cpp` takes WASD, Space, Ctrl, Shift, C, T, Esc and
  Ctrl+N/O/S/D). Quality and effects are left as they are. A mix the keys do not
  name shows as "Custom".
- [x] An overlay in a corner of the view (bottom-left): the preset's name, the GPU
  time per frame and the trace pass's (`gpuTimeMs()`, `traceTimeMs()`, shown in
  the Render panel today); key 0 hides it.
- [x] `docs/playing.md`: the keys and the overlay. The route is in `docs/demo.md`.
- [x] On the demo machine: each preset's frame rate at the demo's resolution, and a
  route through `blue_star.yaml` for the 10-15 minutes (the cockpit, the mirror,
  the glass, the glowing rocks). The route is in `docs/demo.md`. The view at the
  default 1600x900 layout is 930x844 (RTX 4060 Laptop, driver 581.29, vsync off,
  DLSS Quality, 8 ms budget, cockpit at rest, five runs, medians): raster 2.17 ms
  (461 FPS), hybrid half shadow maps 7.63 ms (131), hybrid half 7.10 ms (141),
  hybrid full 7.08 ms (141), probes 7.19 ms (139), DLSS-RR 7.66 ms (131), path
  tracing 5.88 ms (170). The budget spends the ray-traced rungs on samples, so
  their frame times sit together.

Done when: a clean clone builds and passes the tests, and the seven presets switch
live with their name and time on screen.

### `feature/eval-direct-light`

The measuring scripts, which `feature/eval-denoiser` uses as well, then the first
required part. Agree with the owner of `feature/eval-denoiser` on day 1 on the
configuration file and the CSV's columns.

- [ ] The scripts that made the progress figures, into `StarTraveler/tools/eval/`
  (they are not in the repository yet: the branch starts by adding them), made
  into a tool:
  - shots and configurations in one file, so that `feature/eval-denoiser` adds its
    own without touching the code. A shot: the scene, the camera and look-at, the
    motion (`--turn-entity Ship 0.5`, `--move`, `--still`), the frames. A
    configuration: a name and its `starcore-render` flags;
  - nothing hard-coded: the exe, the scenes and the output folder are arguments;
  - one command runs it all: the scenes (the game's ship put into `blue_star.yaml`
    at a pose), the renders (3 seeds each, configurations alternated), the
    references, FLIP, one CSV (shot, config, seed, gpu_ms, trace_ms, flip), plots
    of error against GPU time, image crops;
  - FLIP: NVIDIA's `flip-evaluator` (pip), LDR-FLIP on the PNGs.
- [ ] Three shots, with the moves `starcore-render` has (no keyframed camera
  paths): the cockpit turning toward the Anvil (the progress shot), a slow pass by
  the mirror and the glass, a fast turn past the star (the light changes).
- [ ] A reference per shot: the GPU path tracer at the shot's last pose, 8192 spp
  (`--gpu -f 2048 -s 4`, about 22 s at 1280x720). Score a second seed of it against
  it for its own noise (the progress shot: FLIP 0.034); differences below that mean
  nothing.
- [ ] Direct light on the three shots, FLIP and GPU time per frame: the raster
  pipeline (shadow maps, no rays), the hybrid pipeline with traced shadows and with
  shadow maps (both at Half and Full). Once on the whole image, once on direct light
  alone: `-b 2` is direct light only in the path tracer (the tests use it so);
  check that the hybrid pipeline honours it on a case whose answer is known before
  trusting the numbers.
- [ ] The table, the plot, crops and a paragraph for the report: what traced shadows
  buy over shadow maps, and for how many milliseconds.

Done when: one command reproduces the direct light's table and plot into an empty
folder.

### `feature/eval-denoiser`

The second required part: our denoiser (A-SVGF) against no denoising and against
DLSS-RR, with the scripts of `feature/eval-direct-light` and its three shots. Until
they land, run `starcore-render` by hand to choose the configurations.

- [ ] The configurations: no denoising (`--gpu`, one path per pixel), temporal reuse
  only (`--hybrid --no-spatial`), the spatial filter only (`--hybrid
  --no-temporal`), A-SVGF (`--hybrid`), DLSS-RR (`--hybrid --upscaler DLSS-RR`).
  All at the same render resolution and upscaling ratio (ours with DLSS at the
  `--quality` DLSS-RR has). DLSS-RR traces every pixel, so ours at Full as well as
  Half.
- [ ] Error and time: FLIP and GPU time per frame on the three shots.
- [ ] Lag: a shot that moves, then stands still (`--still N`, every frame written
  with `--dump-every 1`); FLIP of each still frame against the reference at the end
  pose. How many frames each denoiser takes to come within 10% of the error it
  settles at.
- [ ] Crops for the report: one region without denoising, with ours, with DLSS-RR,
  and in the reference.
- [ ] The table, the plot and a paragraph for the report: how much error each
  denoiser removes, for how many milliseconds, and how long it lags.

Done when: one command (the shared scripts) reproduces the denoiser's table, plot
and lag.

## After the branches (everyone)

- [ ] The final numbers: both evaluations on one machine, in one go.
- [ ] The report: the question, each method and what it took to learn it
  (irradiance fields, the denoiser's filters, FLIP), the results, what each rung
  buys for its rays, the limits.
- [ ] Citations: the proposal's sources and, as the course requires, the
  AI-generated code.
- [ ] The demo: a script for the 10-15 minutes, slides, rehearsals, and a recorded
  video in case the demo machine fails.

## In the code

What the comparison uses.

- [x] A visibility buffer rasterized with a jittered projection (triangle ids,
  spheres as ray-cast impostors); paths start from its hits with ray queries.
- [x] Direct light of the sphere lights in closed form every frame, shadowed by
  rays (accumulated) or by shadow maps (`DirectShadows`). Emissive meshes are
  lights too (next event estimation with MIS).
- [x] The raster pipeline with shadow maps (cube maps near a light, cascades far
  away, PCSS): direct light without any rays, a constant ambient term and a
  headlight.
- [x] A-SVGF: samples reprojected with motion vectors, history dropped where the
  temporal gradients see the light change, an edge-avoiding a-trous filter; layers
  of their own for direct light, primary surfaces, reflections and window panes.
- [x] Temporal upscalers: the engine's TAAU, FSR 3.1, DLSS, and DLSS Ray
  Reconstruction (which also denoises).
- [x] DDGI irradiance probes: a probe grid around the camera plus `ProbeVolume`s on
  entities (the ship's cockpit), octahedral irradiance and distance maps, Chebyshev
  visibility, hysteresis, probes inside geometry turned off. In the hybrid pipeline
  only: a diffuse path vertex takes the bounced light from the probes. Off by
  default (`probes`, "Irradiance probes" in the game).
- [x] Resampled light candidates (`lightCandidates`, off by default; 4 candidates
  gave 3-6% less noise in the cockpit for 1.7 ms).
- [x] A path per sample and pixel, reprojected and filtered; an optional radiance
  cache.
- [x] The GPU path tracer, matching the CPU one almost bit for bit.
- [x] `blue_star.yaml` (made by `StarTraveler/tools/build_blue_star.py`): a
  mirror, liquid metal, glass, cracked rocks glowing from within, a lattice of
  bone, a ring.

## Out of scope

From the first list; for after the deadlines, if at all.

- ReSTIR DI (Bitterli et al. 2020): light reservoirs reused across frames and
  neighbors; the blue star's glowing rocks are many mesh lights, where reuse should
  pay more.
- SSR + GTAO (Stachowiak and Uludag 2015; Jimenez et al. 2016) in the raster
  pipeline, the window panes' reflections included.
- DDGI in the raster pipeline (probes in place of the constant ambient); a probe
  ray budget (today every probe every frame, 128 rays each: about 3.3 ms in the
  cockpit); a debug view of the probes; relocation and classification (Majercik et
  al. 2021).
- ReSTIR GI (Ouyang et al. 2021). The trace pass is at the limit of 32 push
  descriptors: new buffers go by device address in `FrameUniforms`, as the probes'
  do.
- One option picking the indirect method, every method through the same denoiser
  and upscaler.
- Keyframed camera paths, played by `starcore-render` and in the game; paths with
  moving lights or objects.
- GPU time per pass (timestamp queries around every pass; today only the whole
  frame and the trace pass), as CSV and in the game.
- Flicker between frames; equal-time comparisons; HDR-FLIP (needs EXR output).
- A closed set piece (a hollow rock, a cave) if bounced light does not show.
- A split screen, two methods side by side.
- The MinGW build.
