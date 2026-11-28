# Measurement validation before the full evaluation

This stage validates measurement plumbing. It is not the three-seed, alternating
full denoiser benchmark. The entry point is
`StarTraveler/tools/eval/check_measurement.py`, using `shots.json` and the existing
denoiser configuration groups. Output is under `renders/measurement/`.

## Frame-associated GPU timing

Each renderer frame slot now stores its global submission number alongside its
timestamp queries. Completed samples contain that number, whole-frame GPU ms,
and trace-interval ms. The latest-time getters update only for a newer submission,
so draining frame slots out of chronological order cannot overwrite new data
with an old frame. The optional capture API drains completed samples independently
of the UI getters. It defaults off and does not insert a wait after every frame.

`starcore-render --gpu-timings file.csv` enables capture, drains all work at the
final image readback, sorts samples by submission, and rejects missing/duplicate
frame indices. The CSV uses one-based `frame` and columns
`frame,gpu_ms,trace_ms,setup_frame`. Accumulation resets do not reset these IDs.
Frame 1 is marked setup. The normal log's CPU duration is explicitly labelled
`Wall`, separate from the latest GPU duration. `--frame-stats` remains submission
state; it does not stand in for GPU completion data.

Timing capture rejects `--dump-every` greater than zero. No per-frame PNG writes
or readbacks are allowed in a timing run. The final readback/file write is outside
the GPU timestamp interval. Whole-frame timestamps cover the renderer command
buffer, not CPU scene preparation, driver shader compilation, or final disk I/O.
The trace interval remains the existing primary trace stages, not every ray in
gradient/probe/effect passes. Use whole-frame time for complete-system cost.

The dedicated GPU test submits five frames, drains slots, checks all IDs and
durations and the latest getter, then checks draining, another submission, and
disabling capture. This covers the old slot-order overwrite. The CLI control
captures seven frames each at 1 and 64 spp in the legacy scene and verifies
positive, nested GPU/trace intervals and complete IDs. The larger workload must
take substantially more trace time. These short controls are qualitative;
their medians are not performance claims.

A capture-on/off image comparison is byte-identical. A separate raw-sample
control appends 128 still frames to the 81-frame Anvil shot and compares its last
sample (seed 225) with an independent one-sample render of the explicit end pose.
Those images are byte-identical too, verifying motion/still accounting without
assuming that the denoiser's appearance proves the camera stopped correctly.

For final benchmarks, exclude setup and a predeclared warmup prefix (16 submitted
frames), use only the motion segment for the motion table, alternate configurations
over three seeds on one idle GPU, then report medians. Preserve the full camera
path. Keep dump/lag runs separate. The [full GPU regression](gpu-regression.md)
has now run all cases; one DLSS assertion remains unresolved.

## References and LDR-FLIP

Each shot's explicit end YAML and exact camera angles are used to render two
1280x720 references. Every render is `--gpu -f 2048 -s 4`, exactly 8192 spp, with
normal accumulation. Frame-state CSV must show accumulation ages 1 through 2048,
all at 4 spp. There is no motion, still suffix, filtering, or upscaling. Reference
seeds 1009 and 2027 are independent of the evaluation seeds 17, 29, and 43.

Reference pairs use the same 5-segment limit, exposure 1, ACES, and firefly clamp 0
as the protocol. Every reference includes its argument array, state/timing CSV,
image hash, endpoint scene hash, and renderer hash. A renderer/shot-definition
change requires a new output directory rather than silently reusing references.

The metric is NVIDIA's [`flip-evaluator`](https://github.com/NVlabs/flip/tree/main/src/python),
version 1.7, `evaluate(..., "LDR", inputsRGB=True, applyMagma=False)` with explicit
67 pixels per degree. These are tone-mapped sRGB PNGs; there is no second tone
mapping, gamma conversion, resizing, or HDR evaluation. Dimensions must match.
The display condition is standardized at 67 PPD, not a measurement of this laptop.

Known-answer checks cover identical images (exact zero), black versus white
(large error), rejection of mismatched dimensions, and agreement between PNG
loading and normalized sRGB arrays. Each real reference is also scored against
itself. The second seed is scored against the first for an empirical reference
noise estimate. This is not a statistical confidence bound or proof of zero
reference bias. Differences smaller than that estimate need more evidence;
do not subtract it from denoiser scores.

## Predeclared finite-capture convergence criterion

The small validation uses the Anvil shot at 384x216, its full 80 motion updates,
then 128 still frames. A separate 8192 spp reference pair is rendered at this
size; the 720p references are never resized. Both methods use Quality,
256x144 -> 384x216: A-SVGF Full plus DLSS and DLSS-RR. Adding 128 to the total
frame count while setting `--still 128` preserves the 80 updates. The first still
frame is submission 82 and is lag age 1. The final submission is 209.

The following rule was fixed before observing these captured curves:

1. Compute each still frame's mean LDR-FLIP against the independent end reference.
2. Estimate terminal error as the median of the final 64 observations.
3. Split that tail into two blocks of 32. Their median difference divided by
   terminal error must be at most 3% (denominator at least 1e-12). Otherwise
   report `tail_not_stationary`, with no settling-frame value.
4. Use `1.10 * terminal_error` as the entry threshold. Entry must hold for every
   remaining observed frame and include at least 16 observations. Report the
   earliest qualifying still age, or `entry_not_observed` if none qualifies.
5. Captures shorter than the tail/hold requirement are `insufficient_capture`.

The suffix requirement avoids reporting an isolated early dip as convergence.
Synthetic curves test a known entry at frame 21, a false early dip, a constant
zero curve, a declining tail, insufficient samples, and persistent oscillation.
The criterion describes the finite observation window, not a proven asymptotic
steady state. A-SVGF's still history is uncapped. If the tail is not stationary,
extend the formal capture (e.g. 256/512/1024 still frames), retaining this rule;
do not loosen it after seeing results to manufacture a settling time.

## Reproduce

Install `StarTraveler/tools/eval/requirements.txt` into an isolated Python 3.13
environment. This workspace uses the portable environment at
`../.build-tools/eval-venv/Scripts/python.exe`; uv and managed Python are also
workspace-local. Dependencies are pinned in that requirements file.

```powershell
../.build-tools/eval-venv/Scripts/python.exe -m pytest StarTraveler/tools/eval/test_metrics.py -q
build/msvc-vs2022/bin/Release/StarCoreGpuTests.exe '[timing]'
../.build-tools/eval-venv/Scripts/python.exe StarTraveler/tools/eval/check_measurement.py --exe build/msvc-vs2022/bin/Release/starcore-render.exe --prepare-exe build/msvc-vs2022/bin/Release/startraveler-prepare-shot.exe --assets StarTraveler/assets --output renders/measurement
```

`--stage timing`, `references`, `controls`, or `lag` runs one stage; `all` is the default.
Lag requires the scene/reference manifest produced by references. `--still-frames`
can extend a validation. Use separate output directories for distinct capture
policies. Preserve old manifests and curves for comparisons.

Artifacts include `measurement.json`, `reference-noise.csv`, `lag-curve.csv`,
`lag-check.png`, reference PNGs, per-run command JSON/logs, and state/timestamp CSVs.
The report stays incomplete if rendering, input validation, or metric scoring
fails. A completed preflight does not imply that the tail reached stationarity.

## Observed preflight results

Validated on 2026-11-25 with RTX 3070 Laptop (8 GiB), driver 610.91, Release.
All six 720p reference images passed frame/sample accounting. Every same-image
FLIP score is exactly zero. The fourth pair below is additional small-resolution
calibration data for the lag check, not a resized version of the Anvil pair.

| Shot / purpose | Output | Spp per reference | Independent-seed LDR-FLIP |
|---|---|---:|---:|
| Anvil turn | 1280x720 | 8192 | 0.01957120 |
| Mirror/glass pass | 1280x720 | 8192 | 0.00188173 |
| Fast star turn | 1280x720 | 8192 | 0.01926483 |
| Anvil lag calibration | 384x216 | 8192 | 0.02054464 |

The two seven-frame workload controls returned complete, positive timestamp
intervals; the 64-spp median trace interval was about 9.4 times the 1-spp interval.
This checks the measuring method's response to known extra work; it is not a
performance comparison or an expected linear scaling claim. Setup is excluded.

| Small Anvil lag check | Tail median FLIP | Half-tail drift | Reported still age |
|---|---:|---:|---|
| A-SVGF Full + DLSS Quality | 0.08690335 | 3.621% | None: tail not stationary |
| DLSS-RR Quality | 0.06264476 | 1.176% | 31, within this finite capture |

Both ran 80 motion updates followed by 128 still frames, at 384x216 output and
256x144 internal size, seed 17. Their curves and finite thresholds are in
`lag-check.png`. The A-SVGF threshold is provisional and is not a valid settling
claim. Its continued decline requires longer formal captures. The RR entry is
conditional on this scene, resolution, seed, finite tail estimate, and rule;
it is not a general latency figure or a result for all three shots.

Verification covered all 16,384 reference-frame timestamps, all eight reference
PNG hashes and sample histories, both 128-point lag curves, and the still capture
indices (82 through 209). `verification.json` records these checks. The renderer
and prepared scene hashes match the dataset. Input fingerprints include protocol,
preparer, metric helper, source scene, and GLBs; this first dataset's extra input
fingerprints were recorded after rendering, with file modification times checked
against run start. Future reuse checks require matching fingerprints.

The Release build, 109 non-GPU CPU/game tests, one dedicated GPU timing test
(29 assertions), and four metric/convergence tests passed. The plot was visually
inspected. Subsequent [controlled AC measurements](denoiser-results.md) completed
81 alternating three-seed runs and extended the small lag capture to 512 frames.
The full GPU regression subsequently ran all 50 cases (49 pass, one unresolved
DLSS assertion); integration with the separate upstream evaluator remains pending.
