# Evaluation shots and shared-tool handoff

The entry file is `StarTraveler/tools/eval/shots.json`. It imports the denoiser
configuration groups from `denoiser-protocol.json`; renderer flags are not copied
into the shot runner. The default smoke run selects raw paths, native Full
A-SVGF, and DLSS-RR Quality. Those are functional checks from separate comparison
groups, not three rows of a resolution-matched quality/time benchmark.

## Upstream status

On 2026-11-25, the remote `feature/eval-direct-light` branch was checked at
`dde6e1bb85e6afa33fa2d1241d55b947ad46b8c3` ("TODO: three branches, one per team
member; the rest out of scope"). Its tree contains no `StarTraveler/tools/eval/`
scripts. A sparse read-only copy is in `../.build-tools/shared-direct-light`.
There is no upstream evaluator to import yet. The files here provide a concrete
local runner and proposed handoff contract; they are not an agreed upstream API
or a completed FLIP/timing evaluator. Recheck that branch before integrating its
eventual scripts and resolve schema differences explicitly.

## Three fixed shots

All three use `blue_star.yaml` with the game's ship spawned from `cockpit.glb`.
The preparer calls `ShipSystem::spawn` and `attachCamera`, retaining the model
hierarchy, hull, cabin light, pilot eye offset, and 70-degree vertical FOV. The
mirror/glass shot overrides the pilot eye with a free camera ahead of the ship
to avoid cockpit pillars hiding the glass. Its ship still translates with the
camera. The ship's transient marker is removed so it can be saved. There is no flight-physics
simulation, collision response, body lean, particle simulation, or camera shake.
Motion uses the renderer's existing per-frame translation/yaw operations.

| Shot | Initial ship position | Initial camera yaw/pitch | Update per frame | Frames / updates | Final yaw |
|---|---|---|---|---|---|
| cockpit-anvil-turn | (0, 0, 0) | -90 / 0 degrees | Ship and pilot yaw -0.5 degrees | 81 / 80 | -130 degrees |
| mirror-glass-pass | (-3, 5, -15) | -89 / 0 degrees | Camera and Ship move (0.05, 0, -0.15) world units | 81 / 80 | -89 degrees |
| fast-star-turn | (0, 0, 0) | -175 / 15 degrees | Ship and pilot yaw +2 degrees | 61 / 60 | -55 degrees |

The first frame does not move. Rates are per rendered frame, independent of
wall-clock performance. If interpreted at 60 Hz, the yaw speeds are -30 and
+120 degrees/s, and the pass speed is about 9.49 world units/s. This conversion
is descriptive; the CLI applies a fixed displacement, not a real-time integrator.

The first shot turns from forward toward the Anvil and its violet cluster.
The old progress image's pose and generating script are unavailable, so this is
a documented reproducible replacement, not a claim of matching its exact pixels.
The second's free camera starts at (-3, 8, -25), with the ship facing -75 degrees
behind it. It moves 4 units right and 12 units forward while viewing Mirror Blade
and Prism. The third turns 120 degrees across the blue star's bearing
(about -116.6 degrees), changing the cockpit's orientation to its light.

The final evaluation resolution is 1280x720. Smoke runs keep every motion update
and use 384x216 to limit image work. A Quality configuration renders internally
at 256x144 in that smoke run. Settings and final seed repetitions are inherited
from the [denoiser protocol](denoiser-protocol.md).

## Preparation and exact endpoints

The Release build adds `startraveler-prepare-shot.exe`, built with the game and
`STARCORE_BUILD_EXAMPLES=ON`. It writes an initial YAML, an end YAML, and
`poses.json` per shot. Scene model references are rebased through StarCore's own
serializer, so the output directory can be outside the assets tree.

The preparer repeats the same floating-point operations and order as
`starcore-render`: camera translation, entity translation, entity yaw around its
origin, orbiting the camera around that origin, then camera yaw. Its pose JSON
records the camera position, yaw, pitch, FOV, and informational look-at point.
`--camera-angles yaw pitch` restores the exact float angles and excludes
`--look-at`; this avoids an extra direction-to-angle round trip at the endpoint.
It is a fixed camera pose option, not a keyframed camera-path system.

When raw paths are selected, the runner also renders the explicit end scene for
one frame at seed `base_seed + frames - 1`. Its PNG hash must equal the moving
raw run's final PNG. This checks the ship, camera, transforms, motion count, and
seed accounting together. These one-sample endpoint images are not the final
8192 spp references. Future references can use the same end YAML and camera
with `--gpu -f 2048 -s 4`, without appending motion or still frames.

## Run it

From the repository root after building Release:

```powershell
StarTraveler/tools/eval/run_shots.ps1 -Exe build/msvc-vs2022/bin/Release/starcore-render.exe -PrepareExe build/msvc-vs2022/bin/Release/startraveler-prepare-shot.exe -Assets StarTraveler/assets -Output renders/shots-smoke
```

All executable paths, the assets root, output directory, configuration file,
shot/configuration selections, seed list, and image size are arguments. The
default is a one-seed functional smoke check. Add `-PlanOnly` to prepare scenes
and emit the command manifest without launching renders. In PowerShell, for
example, use `-Seeds 17,29,43` to select all three seeds, or
`-Configurations asvgf-dlss-full,asvgf-dlss-half,rr-quality` to select the matching
Quality comparison group. Configuration order rotates between seeds per shot.
The runner still does not measure FLIP or valid aggregate GPU time.

Output includes:

- `scenes/<shot>/initial.yaml`, `end.yaml`, and `poses.json`;
- final PNG, intermediate frame PNGs, full log, and submission-state CSV per run;
- `manifest.json`, with exact argument arrays, hashes, per-run status, actual
  camera poses, and a `complete` flag set only after every selected render/check;
- `runs.csv`, whose first columns are the required
  `shot,config,seed,gpu_ms,trace_ms,flip`, followed by group, role, status, and size.

The three metric columns remain empty and `measurement_status` is
`unmeasured_smoke`. Intermediate PNG dumping introduces readbacks and disk work,
and the existing final timestamp getter is not a frame-associated average. Do
not fill timing columns from wall time or label these logs as benchmark results.
Use a separate no-dump run with the frame-associated timestamp export now
described in [measurement validation](measurement-validation.md).

The runner rejects renderer errors, NGX evaluation failures, unexpected fallback
upscalers, wrong actual dimensions, incorrect frame/seed/sample accounting, and
blank final images. Manifest/CSV remain incomplete if a check fails. Frame 1 is
recorded in state CSV; the existing intermediate PNG names begin at frame 2.

## Handoff requirements for the final evaluator

Reuse the shot names, source assets, float poses/end scenes, motion update
counts, configuration names/groups, and CSV's first six columns. The final
evaluator still needs 8192 spp references plus independent reference seeds,
LDR-FLIP, frame-associated GPU timings, plots, crops, and still-frame lag curves.
Its schema can import these files or adapt them explicitly once available.
Keep the full motion range when running a smoke subset. Final lag captures add
the same still-frame count to total frames so the number of motion updates does
not change, and must use a defined tail/convergence criterion.

## Verified smoke results

On 2026-11-25, RTX 3070 Laptop 8 GiB, driver 610.91, the selected three shots each
passed raw-path, native Full A-SVGF, and actual DLSS-RR Quality renders: nine
motion runs in total, plus three independently rendered one-sample end scenes.
The nine runs retained their full 81/81/61 frames. Actual sizes were 384x216
native and 256x144 -> 384x216 for RR. No fallback or NGX evaluation errors were
observed. Accumulation/reset state and frame/seed/sample accounting were checked
for every submitted frame. Each raw endpoint matched its standalone end image
byte for byte. All 109 non-GPU CPU/game tests passed again (15.39 seconds).

The mirror/glass shot was reframed after inspecting an initial cockpit-view
draft: a cockpit pillar hid part of the glass. The final free-camera version
shows the left glass and right mirror without that obstruction. Initial/end
images and the star turn's middle frame were inspected, along with native and
RR results. This is a framing/functionality check, not scored denoising quality.

The selected results are consolidated in `renders/shots-validation.json` and
`renders/shots-selected.csv` (12 rows including the three endpoint checks).
The first and third shots come from `renders/shots-smoke/`; the final mirror shot
comes from `renders/shots-mirror-final/`. The earlier mirror draft in the first
folder is excluded. The summary retains each source manifest and executable
hash so the two launches are not mistaken for one formal benchmark run.
Intermediate PNGs, source scenes, and full command arrays remain in those
folders. Repeating the documented command now uses the final free-camera setup.

This completes shot selection and a runnable local handoff. The subsequent
[measurement validation](measurement-validation.md) completed 8192 spp reference
pairs/noise estimates, LDR-FLIP plumbing, frame-associated timing, and a small
lag check. The later [controlled AC results](denoiser-results.md) contain all 81
three-seed measurements and a shared 512-frame small lag capture. Upstream
integration remains pending. The [full GPU regression](gpu-regression.md) later
ran all 50 cases; one ordinary-DLSS assertion remains unresolved.
