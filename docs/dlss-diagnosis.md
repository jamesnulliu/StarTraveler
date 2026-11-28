# Ordinary DLSS reconstruction diagnosis

The original `difference.mean < 0.5F` threshold is unchanged. On RTX 3070 Laptop,
driver 610.91 and SDK 310.9.1, default Performance model M still fails at
0.509729445. Explicit K passes the same fixture and bounds. This is an alternative
configuration, not a repair of M. Global defaults remain unchanged.

On 2026-11-26 the user accepted deferring further M investigation and continuing
evaluation/presentation work. The failure remains recorded, not waived into a
pass. No statistical test requires blocking the separate Quality evaluation on
this fixture's deterministic image-mean bound.

## Linear data and exposure isolation

Commands, buffers, JUnit and binary fingerprints are retained under
`renders/dlss-diagnosis/`; original executables are in `baseline/`.
The fixture reconstructs 96x72 to 192x144 over 64 frames. Its mean helper is the
maximum absolute per-channel image-mean difference in 8-bit units, not FLIP or
mean absolute pixel error.

| Probe | Result |
|---|---|
| Default M | Signed RGB mean bias (-0.050817, -0.058485, -0.509729) |
| Pre-tonemap buffers | Bias already exists in linear output |
| Pure constant sky | RGB8 matches, but float output has a small bias |
| Disable auto exposure alone | Output unchanged |
| Disable auto exposure and supply unit exposure texture | M output unchanged, including linear-buffer hash |
| Explicit M | Same failure as default |
| K, auto exposure | Original bounds pass; signed blue bias -0.009549 |
| K, unit exposure | Original bounds pass; signed blue bias -0.011466 |
| M, 128 / 256 / 512 frames | Blue bias -0.587529 / -0.570023 / -0.651693; still fails |
| Official signed DLL 310.9.16, default M | Same failure at 0.509729445 |

A uniform background region has linear native RGB approximately (0.05, 0.07,
0.10); M produces (0.0496731, 0.0696371, 0.0990789). The low-resolution input
has the expected color. The initial RGB8 constant-sky control hid a small linear
bias through quantization; it did not establish exact linear color preservation.
See `exposure-preset-grid.json`, `exposure-comparison.json` and `M-history/`.
The isolated patch probe records the actually loaded DLL path and hash; the
project dependency was not upgraded.

These experiments localize the discrepancy to the M runtime/model path on this
machine. They do not identify the responsible operation inside NVIDIA's model,
nor separate its internal exposure processing from other network behavior.
Application exposure changes do not resolve it. Source review has not found a
violated integration contract: HDR linear input, low-resolution unjittered motion
vectors, reversed depth and helper exposure defaults were checked. No compensating
color scale or relaxed assertion is used.

## Explicit model selection and verification

`UpscalerSettings::dlssPreset` and `--dlss-preset Default|K|M` select ordinary
DLSS Super Resolution's model. Both renderer and game accept the flag; the
Render panel exposes DLSS model while DLSS is active. Default retains vendor
selection. The control does not select RR's model. Switching recreates the SR
feature after waiting for outstanding work and resets history once.

The installed 310.9.1 header identifies K as default for Quality/Balanced/DLAA,
and M for Performance. Model and resolution quality mode are separate settings:
selecting K does not itself change internal resolution.

- All application/test targets built successfully; fresh CPU/game suite: 109 pass.
- Focused GPU suite: 6 pass, 1 fails, 0 skipped. K color/model-switch regression
  passes the original mean/worst bounds. Timing, fallback, particles, hybrid
  upscaling and motion-trail cases pass. The unchanged M fixture still fails.
  This is a seven-case subset of the new 51-case inventory, not a full rerun.
- Six 64-frame CLI runs compare preserved/current defaults and explicit K in
  Performance and Quality. Both current defaults reproduce the preserved PNGs
  byte for byte. Quality K also matches its default PNG in this small scene;
  Performance K differs. See `cli-check/manifest.json`.
- Both executables reject invalid model names before rendering.
- Runner tests: 2 pass, including stopping an active child on a pause request
  without losing completed results.

Reports are under `validation/`. These are functional/color checks, not a K/M
performance comparison or a guarantee for all scenes. The historical full GPU
regression remains 49 pass / 1 fail / 0 skipped: [report](gpu-regression.md).

The subsequent [three-path rehearsal](denoiser-demo.md) passes all nine
production-resolution Quality runs (Full+K, Half+K, RR; seed 17). All nine
endpoint PNGs match the original formal dataset exactly. This extends the
small Quality control to the selected paths; it is not a new GPU-suite pass
or a K/M timing comparison.

Temporary readback APIs and exposure/capture hooks were removed from production
source. Snapshots remain in `diagnostic-source/`. `probe_dlss.py` captures current
fixture logs, JUnit and loaded modules; image/linear exports require an archived
diagnostic build with the opt-in hooks.

## Research and presentation

Keep the completed 81-run dataset tied to its original binary and Quality
configuration. Do not replace rows with new K results. Prefer Quality with
explicit K for a reproducible live demo, recording SDK, driver, quality and model.
The small Quality control supports continuity but cannot replace rehearsal of
the actual route. Keep M as an optional labeled diagnostic comparison. New timing
comparisons require alternating runs, three seeds and equal dimensions.

The main research question remains the quality/time/stability tradeoff between
no denoising, A-SVGF and DLSS-RR. Put the model diagnosis in limitations or an
appendix. Say K avoids the observed local bias, not that M has been fixed or the
entire GPU suite passes. K/M performance tradeoffs have not been measured here.

Primary references: [NVIDIA guide](https://raw.githubusercontent.com/NVIDIA/DLSS/main/doc/DLSS_Programming_Guide_Release.pdf),
[official releases](https://github.com/NVIDIA/DLSS/releases),
[Streamline integration](https://github.com/NVIDIA-RTX/Streamline/blob/main/docs/ProgrammingGuideDLSS.md).
Installed 310.9.1 headers govern this build's defaults; newer guides can describe
different model behavior.
