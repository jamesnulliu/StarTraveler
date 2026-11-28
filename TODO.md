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
  (the default); 4 hybrid, Full; 5 hybrid, Half, irradiance probes; 6 DLSS-RR;
  7 path tracing (the reference; it restarts whenever anything moves). The number
  keys are free (`game_layer.cpp` takes WASD, Space, Ctrl, Shift, C, T, Esc and
  Ctrl+N/O/S/D).
- [x] An overlay in a corner of the view: the preset's name, the GPU time per frame
  and the trace pass's (`gpuTimeMs()`, `traceTimeMs()`, shown in the Render panel
  today); a key hides it.
- [x] `docs/playing.md`: the keys and the overlay. F2 toggles live status;
  actual dimensions/upscaler and fallback are shown. All seven CLI profiles and
  screenshots pass, with 17 game cases / 286 assertions; physical keys remain
  part of the person's final rehearsal. See [profiles](docs/demo-presets.md).
- [ ] On the demo machine: each preset's frame rate at the demo's resolution, and a
  route through `blue_star.yaml` for the 10-15 minutes (the cockpit, the mirror,
  the glass, the glowing rocks).

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
- [x] Three fixed shots with the moves `starcore-render` has (no keyframed camera
  paths), in `StarTraveler/tools/eval/shots.json`: an 81-frame cockpit turn toward
  the Anvil (a documented replacement: the old progress pose is unavailable), an
  81-frame free-camera pass by mirror and glass, and a 61-frame fast cockpit turn
  across the star. All motion updates are retained in the low-resolution smoke
  runs; each explicit end pose matches its moving raw sample exactly.
- [x] Two independent references per selected shot at its explicit end pose:
  1280x720, `--gpu -f 2048 -s 4`, exactly 8192 spp, seeds 1009 and 2027.
  Reference-pair LDR-FLIP: Anvil 0.01957, mirror/glass 0.00188, star turn 0.01926
  (RTX 3070 Laptop, driver 610.91, 67 PPD). State/timestamp CSVs verify all 2048
  frames; same-image scores are zero. These are empirical noise estimates, not
  exact truth; avoid claims about differences below them. See measurement validation.
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

Preflight findings and environment gates: [denoiser preflight](docs/eval-denoiser-preflight.md).

- [x] Resolve the preflight's build gates and verify an actual DLSS-RR render
  (RTX 3070 Laptop, driver 610.91, 853x480 -> 1280x720, Quality, 16 frames).
- [ ] Reject fallback upscalers and NGX evaluation errors in the shared evaluation tool.
- [x] Prepare three shots and a local handoff runner (`run_shots.ps1`), using the
  game's ship spawn, explicit end scenes, existing configuration groups, and the
  shared CSV's required first six columns. Nine selected motion runs plus three
  endpoint checks passed at 384x216; the runner rejects fallback/error/size and
  frame-state mismatches. See [shot handoff](docs/evaluation-shots.md).
- [ ] Integrate with the actual upstream evaluator when published: the checked
  remote branch and main still point to `dde6e1b` after the full GPU regression;
  no shared evaluation scripts are present. The smoke runner retains empty
  `unmeasured_smoke` fields; the separate local formal runner has complete data.
- [ ] Clear the GPU regression before the PR. All 50 GPU cases were executed:
  49 passed, one ordinary-DLSS reconstruction mean-difference assertion failed
  (0.509729445 versus <0.5), zero skipped; two isolated reruns reproduce it.
  The prior glossy-pane timeout is resolved (passed in 155 s); 109 CPU/game cases
  also pass. Diagnose the DLSS discrepancy without weakening the threshold.
  See [full regression](docs/gpu-regression.md).
  [Diagnosis](docs/dlss-diagnosis.md): linear/exposure probes localize the local
  Performance bias to M; longer history and DLL 310.9.16 do not resolve it.
  Explicit K passes original bounds and is selectable; defaults and the 81-run
  Quality dataset remain unchanged. Fresh CPU: 109 pass; focused GPU: 6 pass /
  1 unchanged M failure / 0 skipped. M remains open.
  On 2026-11-26 the user accepted deferring further M investigation to continue
  evaluation/presentation work; this is not a passing test or a changed threshold.
- [x] Expose ordinary DLSS model selection (Default/K/M) in both CLIs and the
  Render panel; validate K color preservation and safe history-resetting switches.
- [x] Replay all three fixed paths at 1280x720 with Quality: Full+K, Half+K and
  RR, seed 17. All nine runs pass; 669 frame states/timestamps and 66 artifact
  hashes verify, and all nine endpoint PNGs equal the original formal outputs.
  This is a functional rehearsal, not a new timing comparison. See
  [presentation outline](docs/denoiser-demo.md).
- [x] Run the real game window through a seven-phase automatic flight rehearsal
  and record a silent backup: 139.9 s, 1280x720/30 fps, 4197 decoded frames.
  Cockpit Full/Half/RR, mirror/glass K/RR and star-turn K/RR all appear; sampled
  actual-upscaler logs reject fallback. Complete decoding, scene-change checks
  and visual review of all seven phases pass. Actual viewport 1280x698,
  internal 853x465; this illustrative recording is not formal timing data.
  See [recording and chapters](docs/denoiser-demo.md#window-recording).
- [ ] Verify manual keyboard/mouse flight and projection on the final presentation
  setup. Native input control is unavailable in this agent session; automatic
  windowed flight does not certify manual operation or a full spoken rehearsal.
- [x] Establish an independent one-frame-sample baseline (`--gpu --no-accumulation`)
  and comparison groups with matching internal/output sizes. The minimal protocol
  check validates 11 configurations and confirms that `--no-temporal` also resets
  DLSS history every frame; see [comparison protocol](docs/denoiser-protocol.md).
- [x] Validate moving-frame counts and explicit end scenes: 80/80/60 updates for
  the three shots; each final independent raw frame matches a standalone end
  scene's one-sample render at the matching seed, byte for byte. Camera angles
  can be restored without a look-at round trip (`--camera-angles`).
- [x] Correct frame-associated GPU timing: per-slot submission IDs, chronological
  latest getter, opt-in `--gpu-timings` CSV, setup marker, and no PNG dumping in
  timing runs. GPU slot-drain and workload controls passed; capture does not
  change image bytes. Appending 128 still frames preserves the 80-update endpoint
  (verified with an independent raw sample). Frame 1 remains absent from PNG
  dumps, but is in state/timing CSV; the complete still range starts at frame 82.
- [x] Connect NVIDIA LDR-FLIP 1.7 at explicit 67 PPD on equal-sized sRGB PNGs;
  known-image, PNG/array, and dimension checks passed. No resizing or extra gamma
  conversion. Reference images, hashes, noise table, and 256 still-frame scores
  are in `renders/measurement/`; [method and results](docs/measurement-validation.md).
- [x] Define and validate the finite-capture lag rule: final 64-frame median,
  half-tail drift <=3%, threshold 110% of terminal error, with at least 16
  observations sustained through the captured suffix. Unsteady tails get no
  settling-frame number. Synthetic curves and a real 128-still-frame case passed.
- [x] Extend the lag capture with the unchanged convergence rule: both methods
  pass a shared 512-still-frame Anvil capture at 384x216, seed 17. A-SVGF+DLSS
  enters its terminal band at frame 101 (0.947% tail drift), RR at frame 47
  (0.495%). The longer capture supersedes the earlier 128-frame estimates.

- [x] The configurations, in `StarTraveler/tools/eval/denoiser-protocol.json`:
  native-resolution ablation with independent raw paths, an unfiltered hybrid
  control, temporal-only Full, spatial-only Full, and A-SVGF Full/Half, all with
  upscaling Off. A separate end-to-end group compares A-SVGF Full/Half plus DLSS
  with DLSS-RR, all at Quality and matching internal/output dimensions. RR traces
  every internal pixel. Keep the two groups' tables separate: the raw path tracer
  ignores upscaling, and no-temporal also resets DLSS, so mixing those rows into
  the Quality table would confound the comparison. All 11 minimal static
  configurations (including diagnostic controls) passed on the preflight GPU.
- [x] Error and time: 81 controlled AC runs on the three shots, three seeds,
  nine configurations; 27-row median/range summary. GPU isolation, actual
  upscaler/history/dimensions, 6021 timestamps, artifact hashes and AC state
  passed validation. Earlier mixed-power runs remain diagnostic only. Three
  identical AC controls had 0.278% relative timing range. Independent-reference
  re-scoring preserved within-group order. See [results](docs/denoiser-results.md).
- [x] Lag: a shot that moves, then stands still (`--still N`, every frame written
  with `--dump-every 1`); FLIP of each still frame against the reference at the end
  pose. How many frames each denoiser takes to come within 10% of the error it
  settles at. The local result is the one-seed, 384x216 Anvil capture above;
  it is not a claim for every shot or production resolution.
- [x] Validate production-resolution lag and seed variability before generalizing
  the small one-shot lag result to 1280x720 or all camera paths.
  The first production batch is diagnostic: endpoint mismatch at mirror/glass,
  seed 29 (FLIP 0.0000583), plus a discovered wall-clock temporal input. Nine
  completed captures/curves are retained; none are final fixed-step results.
  [Protocol v3](docs/production-lag.md) fixes temporal input and DLSS K exposure.
  All 18 production captures / 36 reference outcomes pass unchanged criteria
  at a shared 512-frame horizon per shot, including exact motion endpoints.
  Independent verification checks 10536 PNG hashes, 10554 state rows and
  36 direct FLIP samples (maximum deviation 0). See [results](docs/production-lag-results.md).
- [x] Expose fixed GPU temporal input (`--frame-ms`, default 0 = wall clock),
  record actual `frame_time_ms`, and test queued versus per-frame readback
  equivalence. 109 CPU/game cases and two focused GPU cases pass.
- [x] Initialize previous object transforms on full scene uploads and verify
  stationary first-frame motion plus real timed/dumped motion-prefix equality.
  Frozen fixed-step renderer shows 372273 nonzero Motion-view pixels in the
  stationary mirror first frame; the fix reduces this to zero. The new GPU
  regression passes with synchronization validation, and 109 CPU/game cases pass.
  Auto-exposure controls still differed, so that repair alone was insufficient.
  With fixed K exposure1, the clean81/593-frame check now passes: all80 moving
  PNGs, timed endpoint, reference controls and state/time/exposure checks agree.
- [x] Integrate explicit fixed DLSS K exposure and complete the clean production
  prefix/settling validation. `--dlss-exposure 1` is implemented in both CLIs,
  Render panel and frame CSV. 109 CPU/game cases and three GPU regressions pass
  with synchronization validation. Clean 81/593-frame preflight and complete
  formal collection/audit pass under `renders/production-lag-fixed-exposure`.
- [x] Revalidate the 27 historical Quality-group runs in a separate fixed-time,
  fixed-exposure batch. All 27 runs, three calibration sequences, three exact
  repeats and 54 independently recomputed reference scores pass unchanged limits.
  See [new results](docs/quality-fixed-exposure-results.md).
  Preserve the original AC results; native upscaler-Off/manual-exposure rows are
  unaffected by this particular temporal-upscaler input.
- [x] Crops for the report: fixed regions in all three shots, raw paths, native
  A-SVGF Full, Full+DLSS, RR, and the reference; median-FLIP seeds are labelled.
- [x] The local table, plots and report paragraph: [results](docs/denoiser-results.md)
  include error/time tradeoffs, negative findings, reference sensitivity, lag
  and scope limits. Shared upstream integration remains a separate open item.

Done when: one command (the shared scripts) reproduces the denoiser's table, plot
and lag.

## After the branches (everyone)

- [x] Local denoiser primary bibliography and AI assistance draft are linked
  from the final report. Reconciliation with the original proposal, other
  members' contributions and instructor-specific submission rules remains open.

- [x] Prepare verified source/full team handoff archives, with Chinese receiving
  instructions, runtime dependencies and an explicit open-issue list. See
  [handoff](HANDOFF.md) and [open issues](OPEN_ISSUES.md).

- [x] Step 5, local denoiser materials: integrate fixed-exposure production
  results into the [final denoiser report](docs/final-denoiser-report.md),
  dual-reference figures, editable 12-main-slide presentation with two Quality appendices and timed English script
  with Chinese rehearsal cues. Preserve historical timings and all thresholds.
  This completes the denoiser material integration, not the team's unified report.
- [x] Step 6, automatic real-window rehearsal and backup: final fixed-exposure
  configuration recorded and accepted. The new 139.87-second video, full decode,
  141 configuration samples, per-phase motion/holds and visual review pass.
  See [current recording](docs/final-window-rehearsal.md). Preserve the old take.
- [ ] Step 6, person on the presentation setup: verify manual controls,
  projection, spoken/audio timing and video playback. Automatic flight does not
  certify these checks.

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
