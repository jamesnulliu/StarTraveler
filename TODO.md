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

- [ ] Clean build: clone into a new folder on a machine that has not built
  StarTraveler; follow only `README.md` and `docs/building.md`; `msvc` preset;
  `ctest --preset msvc-release` passes. What the docs miss goes into
  `docs/building.md`. MinGW is not checked: say so there.
- [x] Demo presets: keys 1-7 set the Render panel's options in one press, while
  flying as well: 1 raster, no rays; 2 hybrid, Half, shadow maps; 3 hybrid, Half
  (traced shadows); 4 hybrid, Full; 5 hybrid, Half, irradiance probes; 6 DLSS-RR
  (Quality); 7 path tracing. `--demo-preset N` starts with one (landed with
  `feature/eval-denoiser`, PR #3).
- [x] An overlay in a corner of the view: the preset's name ("Custom" after a
  change by hand), the GPU time per frame and the trace pass's, the rendered and
  output sizes and the upscaler running (fallbacks flagged). F2 or the Render panel
  shows it; off by default.
- [x] `docs/playing.md`: the keys and the overlay.
- [ ] Decide what the presets do to the look: today they also switch the effects and
  particles off and set exposure 1 with ACES (the evaluations' settings), so the
  demo does not look like the game.
- [ ] On the demo machine: each preset's frame rate at the demo's resolution, and a
  route through `blue_star.yaml` for the 10-15 minutes (the cockpit, the mirror,
  the glass, the glowing rocks).

Done when: a clean clone builds and passes the tests, and the seven presets switch
live with their name and time on screen.

### `feature/eval-direct-light`

The measuring scripts share the shot schema and first six CSV columns with the
denoiser evaluation.

- [x] One command prepares scenes with the game's ship, alternates configurations across
  three seeds, renders references, and writes NVIDIA LDR-FLIP on the PNGs, GPU timings,
  a CSV, error/time plots and image crops. The JSON holds the shots and named render
  flags. Executable, assets and output paths are CLI arguments.
  See [reproduction instructions](docs/eval-direct-light.md).
- [x] Three fixed-motion shots. The cockpit turns toward the Anvil from a reproducible
  replacement for the progress shot's undocumented pose, the camera passes the mirror
  and glass slowly, and the cockpit turns quickly past the star.
- [x] Two independent GPU path-traced references per shot and mode at the last
  pose, 8192 spp each (`--gpu -f 2048 -s 4`). Their pairwise FLIP estimates the
  reference noise.
- [x] Direct light on the three shots, FLIP and GPU time per frame: the raster
  pipeline (shadow maps, no rays), the hybrid pipeline with traced shadows and with
  shadow maps (both at Half and Full). Once on the whole image, once on direct light
  alone. The tool uses `-b 2 --direct-only` to exclude second-scatter light sampling
  and checks direct illumination and an indirect mirror reflection before measuring.
- [x] The generated report includes the table, plots, crops and paired results
  for traced shadows versus shadow maps.
- [x] Record the RTX 4090 timing results and the normal Windows desktop
  conditions in [PR #2](https://github.com/jamesnulliu/StarTraveler/pull/2).

Done when: one command reproduces the direct light's table and plot into an empty
folder.

### `feature/eval-denoiser`

The second required part: our denoiser (A-SVGF) against no denoising and against
DLSS-RR, on the same three shots. Results and how to run it:
[denoiser evaluation](docs/eval-denoiser.md) (RTX 3070 Laptop, driver 610.91, 1280x720,
seeds 17/29/43).

- [x] The configurations (`StarTraveler/tools/eval/denoiser-protocol.json`), in two
  groups never mixed: native, no upscaler (independent path-traced frames with
  `--gpu --no-accumulation`, the hybrid pipeline unfiltered, temporal only, spatial
  only, A-SVGF Full and Half); and Quality (A-SVGF Full or Half + DLSS against
  DLSS-RR, 853x480 to 1280x720). `--no-temporal` also resets DLSS's history, hence
  the native group.
- [x] Error and time on the three shots, with `check_measurement.py` (references)
  and `run_formal.py` (the runs): A-SVGF Full has 48.6/3.6/48.6% less FLIP than the
  unfiltered hybrid pipeline for 2.6/1.6/3.0 ms more; DLSS-RR has the least error in
  the Quality group, for 1.8-2.4 ms more than A-SVGF Full + DLSS (with a fixed time
  step and DLSS model K at a fixed exposure).
- [x] Lag: 512 still frames after each shot's motion, FLIP of every one against both
  references; the frame after which a method stays within 10% of its own final
  error. DLSS-RR settles first on all three shots (47/1/131 frames against
  116/286/172), A-SVGF Full + DLSS ends with less error on two of them.
- [x] Crops (written by `run_formal.py`) and the table, plots and paragraph:
  `docs/eval-denoiser.md`, summaries in `docs/eval-denoiser/`.
- [x] Fixes the measurements needed: motion vectors of a new scene start still
  (`[initial-motion]` test); `--frame-ms` (a fixed time step for the temporal
  passes); `--dlss-preset` and `--dlss-exposure`; `--frame-stats`.
- [ ] The settling and fixed-input Quality runs (`run_production_lag.py`,
  `run_quality_revalidation.py`) only accept the renderer binary that passed their
  preflight (`validate_temporal_prefix.py`), which starts from captures not in the
  repository: make them run from a clean checkout, one command.
- [ ] One tool for both evaluations: `direct_light.py` and `run_formal.py` share
  the shots and `prepare_shot`, but not the runner or the CSV.
- [ ] DLSS at Performance with the SDK's default model (M on an RTX 3070 Laptop,
  driver 610.91) misses its color test (0.5097 against < 0.5); K passes, and the
  whole suite passes on an RTX 4060 Laptop GPU (driver 617.42).

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
