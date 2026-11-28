# Direct-light evaluation

From the repository root, build the Release game and examples as described in
[Building](building.md). The build produces both `starcore-render` and
`startraveler-prepare-shot`. If building for this evaluation alone, configure with
`-DSTARCORE_FSR=OFF -DSTARCORE_DLSS=OFF`. Install the Python dependencies in a virtual environment.

```powershell
python -m pip install -r StarTraveler/tools/eval/requirements-direct.txt
```

On an idle ray-query GPU, run the complete sweep into an empty directory.

```powershell
python StarTraveler/tools/eval/direct_light.py --exe build/msvc/bin/Release/starcore-render.exe --assets StarTraveler/assets --output renders/direct-light
```

The runner finds the preparer beside the renderer. Use `--prepare-exe` for a separate build,
`--config` for another experiment, and `STARCORE_VULKAN_DEVICE` to select the renderer's GPU.
The report and CSV record the selected Vulkan device, driver and output resolution.
Keep other GPU workloads off that device throughout collection. Use `--timing-note` to
record collection conditions or flag an exploratory run in the report.

## Shots and configurations

`StarTraveler/tools/eval/direct-light.json` holds the shots, camera and ship poses, motion rates,
seeds, resolution, crop rectangles (left, top, right, bottom as fractions of the image), and
named flag lists. The three shots and the game's ship preparer are shared with the denoiser
branch. The original progress figure's exact pose was unavailable. The 81-frame Anvil turn is
its reproducible replacement. The 81-frame mirror/glass pass uses a free camera ahead of the
ship, and the 61-frame cockpit turn sweeps 120 degrees past the blue star.

Each shot can override `source_scene` with its own `scene` (relative to `--assets`), and
specify a trailing `still` frame count. An optional `camera` gives `position`, `fov` and
either a world-space `look_at` point or `yaw` and `pitch` in degrees. The first frame is
stationary. Both camera and ship translate by `move` and yaw by `turn` on each moving update.
The preparer saves the initial and final scenes with the ship, cabin light, pilot eye or free
camera, exact camera angles and look-at point. For each shot the runner checks that the last
moving raw path-traced frame matches a separate render from the saved pose. Each reference
accumulates 2048 stationary frames at 4 spp from that final pose. Seeds 1009 and 2027 give the
reference and its independent noise estimate.

Five configurations run in each of two modes. Whole-image runs use the normal five-segment
estimator. Direct runs use `-b 2 --direct-only`. The segment cap alone can still sample a
light from the second surface, producing indirect light. The extra switch keeps emission,
sky and contributions with at most one scattering event under the same segment cap.
Straight transmission through a thin pane consumes a segment but does not count as scattering.
Raster keeps its sky and sphere-light approximation and disables the artificial ambient and
camera headlight in direct mode. The default rendering settings are preserved for whole images.

A known-answer test runs first on the GPU path tracer and both shadow methods at Full and Half.
It views a directly lit receiver, then its reflection in a mirror. The receiver must be bright
and its reflection dark in direct mode. A five-segment render must recover the bright reflection.
Failure stops the sweep and retains the diagnostic PNGs and renderer logs.

To add a configuration, append a name and renderer flags to `configurations`. Optional `pair`
and `shadows` fields identify two configurations for the report's traced-versus-maps paragraph.
Shot timing, output, seeds, camera, spp and motion come from the runner. Flags common to
references and candidates belong in `common_flags` or the corresponding `modes` entry.

## Output and interpretation

- `results.csv` begins with `shot,config,seed,gpu_ms,trace_ms,flip`. The remaining columns
  are `mode,gpu,driver,width,height`. Each shot and mode has 15 candidate rows, one reference
  self-check and one reference-noise row, for 102 rows in the default sweep.
- `gpu_ms` and `trace_ms` use Vulkan timestamp queries associated with each submitted
  frame. Each run uses the median after the 16-frame warmup and before any still suffix.
  Configurations alternate and their starting order rotates across seeds 17, 29 and 43.
  The report shows medians across seeds. Raster reports zero trace time.
- FLIP is NVIDIA LDR-FLIP on same-sized sRGB PNGs at 67 pixels/degree. The mean is over
  the full endpoint image. Each mode has its own reference and noise estimate. The report
  flags paired differences at or below that estimate.
- `report.md` contains tables, paired comparisons per shot and across the three shots, and
  links to the six error/time plots and six crop sheets. Crops show the same pixels and first
  seed across configurations. Each shot directory retains both scenes, poses, all PNGs,
  renderer commands, logs and raw per-frame timestamps.

## Verification on RTX 4090

The complete 1280×720 sweep on an RTX 4090 with NVIDIA driver 610.60 produced 102 CSV rows,
six plots and six crop sheets. All 15 direct-light controls and all three endpoint comparisons
passed. Independent reference pairs had FLIP from 0.00146 to 0.01957. Of the 12 paired
shadow-method comparisons, eight fell within their reference-pair FLIP, two favored
traced shadows and two favored shadow maps beyond that threshold. The four group medians
for traced shadows were 0.16–0.86 ms faster in this run. The Windows desktop was running
normally during the sweep. [PR #2](https://github.com/jamesnulliu/StarTraveler/pull/2)
records the results and collection conditions.

The Visual Studio 2022 Release build passed 156 CTest cases and the four evaluation script
tests. Three upscaling cases were skipped with the optional FSR and DLSS SDKs disabled.
