# Production-resolution settling validation

Completed v3 results and independent audit are in
[production settling results](production-lag-results.md): all 18 captures /
36 reference outcomes pass at the initial shared 512-frame horizon per shot.

Protocol v3 retains v2's fixed temporal step and adds explicit K exposure 1.
Native tone mapping stays at manual exposure 1; SR's internal auto exposure is
disabled with a constant texture 1. RR keeps its original unity-input behavior.
The earlier auto-exposure runs remain diagnostic. The first wall-clock batch
under `renders/production-lag` is diagnostic, not final: collection stopped on
a motion-endpoint mismatch. Two repeated historical timing runs and a short
dumped run reproduce the historical image, so that mismatch's exact cause is
not yet established. Source audit independently found that DLSS/RR receive
wall-clock frame intervals (including export waits), which must be controlled
for reproducible offline frame-based evaluation. No settling/error threshold
was loosened. The original v1 protocol and source are archived with that batch.

This supplements the one-seed 384x216 Anvil result. The predeclared collection
uses the three fixed shots, seeds 17/29/43, and two methods: A-SVGF Full + DLSS
Quality and DLSS-RR Quality. Output is 1280x720, internal size 853x480, one sample
per frame, with temporal history retained. It does not change the original
81-run dataset or the known Performance-M test failure.

## Fixed protocol

- Use the fixed-step renderer, fingerprinted in the new manifest, and
  `--frame-ms 16.6666667` for every control and still capture. Actual supplied
  steps are checked in every `frame_time_ms` CSV row. The SDK remains 310.9.1;
  ordinary DLSS explicitly selects K and `--dlss-exposure 1`, verified in every
  `dlss_exposure` CSV row (RR rows 0: this SR-only setting is inactive).
  The 16.667 ms value is simulation input,
  not measured GPU time; timestamp queries still measure real GPU work.
- The reference bank retains renderer SHA-256
  `b5a3164785840db1907d8a45989fc7a0240ddfa67175f6bbdc77563599adb9df`.
  Three 32-spp endpoint controls match between the old and fixed-step renderers;
  source audit confirms the added override changes temporal/post time, not path
  transport. Manual exposure/effects-off references are compatible. Evidence:
  `renders/fixed-exposure-validation/reference-compatibility.json`.
- Reuse the original prepared scenes and exact motion increments; the last
  moving PNG must match a new same-seed, fixed-step motion-only control byte
  for byte. Those controls use timestamp capture without PNG dumping; the
  still capture dumps each frame. Thus scheduling/export differences may not
  alter the shared motion prefix. Differences from historical wall-clock
  endpoints are recorded separately, not silently merged into old results.
  Validate every frame's dimensions, seed, sample count, active upscaler,
  accumulated frame count and history-reset state. No fallback is accepted.
- Score every still frame with NVIDIA LDR-FLIP 1.7, 67 PPD, equal-sized sRGB
  PNGs without resizing or an additional gamma transform. Use both independently
  seeded 8192-spp endpoint references (1009 and 2027). The second reference is a
  sensitivity check; neither is exact ground truth.
- Preserve the original settling rule: final 64-frame median as the terminal
  error; relative drift between its two halves <=3%; threshold 110% of terminal
  error; entry must remain valid through the captured suffix with at least
  16 observations. An unstable tail receives no settling-frame estimate.
- Start with 512 still frames. For each shot, use a shared horizon for both
  methods, all three seeds and both references. If any of the twelve outcomes
  is not settled, rerender all six physical runs at 1024, then at most 2048.
  A longer capture must reproduce every saved shorter-prefix PNG exactly.
  At the maximum horizon, retain unresolved statuses rather than relaxing the
  rule. Different shots may select different shared horizons.
- Alternate method order between seed runs. Report each seed and reference
  separately, plus median/min/max only when all three seeds settle for that
  method/reference. Never omit an unsettled seed to obtain a favorable median.

The lowest horizon passing the above finite-capture rule is selected; this is
not a claim of asymptotic convergence, a confidence interval, a common-error-target
latency or a statistical significance test. Each method approaches its own
terminal-error band. Three seeds measure limited variability on one GPU.

This is frame-based quality validation, **not a new timing comparison**. AC power
and GPU identity are checked during rendering; frame dumping and CPU FLIP work
are excluded from performance claims. Per-process GPU-idle/time-comparison gates
are not used because no GPU-time estimates are published from these captures.

## Running and resuming

The clean fixed-exposure implementation passes 109 CPU/game cases and three
focused GPU regressions with synchronization validation: exposure/color/history
changes, initial object motion, and fixed time with queued/per-frame readback.
The real mirror seed 29 preflight passes all 80 moving PNG comparisons, a timed
endpoint check, 593 frame-state records and three native reference controls.
Evidence: `renders/fixed-exposure-validation`. Its renderer and runtime files are
fingerprinted by the formal manifest. Earlier automatic-exposure diagnostics do
not establish a unique SDK-internal cause; this is a verified controlled option.

```powershell
& ../.build-tools/eval-venv/Scripts/python.exe StarTraveler/tools/eval/run_production_lag.py `
  --exe build/msvc-vs2022/bin/Release/starcore-render.exe `
  --references renders/measurement --formal renders/formal-ac `
  --output renders/production-lag-fixed-exposure --workers 4
```

The runner reuses the validated renderer subprocess and power checks, preserves
all frame PNGs, fingerprints completed captures, and checkpoints two-reference
FLIP rows every 16 frames. Four CPU worker processes call the same `ldr_flip`
helper; this does not change the rendering or metric definition. Interrupted
renders are archived before retrying; completed captures and scores are reused.
Renderer, configuration, reference, helper or runner changes reject an old
manifest rather than mixing outputs.

Use `--exe build/msvc-vs2022/bin/Release/starcore-render.exe` for v3; the frozen
v1 executable has no fixed-step input. The compatibility/preflight manifest must
pass before starting v3. Do not reuse diagnostic output directories.

`renders/production-lag-fixed-exposure/pause.request` stops active rendering or terminates the
CPU scoring workers. The supervising agent checks five-hour and weekly quotas;
the standalone runner does not query account quota. After a requested pause,
preserve its reason before clearing the sentinel to resume.

Outputs are a manifest, all capture states/images/hardware samples, per-run
two-reference curves, `runs.csv`, `summary.csv` and `convergence.png`. Final
results and verification are appended after collection; this document records
the protocol before outcome inspection.

## Shared evaluator

The fresh remote check still reports both main and `feature/eval-direct-light`
at `dde6e1bb85e6afa33fa2d1241d55b947ad46b8c3`. Its already audited identical tree
contains only the two scene/model builders under `StarTraveler/tools`; no shared
evaluator is published. Evidence is in
`renders/production-lag/upstream-check.json`. The local bridge remains separate
from the pending upstream integration.
