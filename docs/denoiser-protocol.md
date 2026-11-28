# Denoiser comparison protocol

This protocol separates two questions: which filters reduce noise in the hybrid
pipeline, and how the complete A-SVGF plus DLSS solution compares with DLSS-RR.
The configuration data is in
`StarTraveler/tools/eval/denoiser-protocol.json`. Its validation camera is a small
known scene for checking behavior, not one of the three final evaluation shots.

## Native filter ablation

Use the same output and internal size for every row, with upscaling Off. The
smoke check uses 384x216; the final shots can use 1280x720. Full means every
internal pixel is traced. Half keeps the same image dimensions but rotates over
one traced pixel per 2x2 block each frame.

| Name | Pipeline-specific flags | Role |
|---|---|---|
| raw-path | `--gpu --no-accumulation --upscaler Off` | Required unfiltered one-path-per-pixel baseline |
| raw-hybrid | `--hybrid --no-temporal --no-spatial --upscaler Off` | Additional control using the same hybrid estimator |
| temporal-full | `--hybrid --no-spatial --upscaler Off` | Temporal reuse; no a-trous iterations |
| spatial-full | `--hybrid --no-temporal --upscaler Off` | Spatial filter; no history from earlier frames |
| asvgf-full | `--hybrid --upscaler Off` | Full A-SVGF |
| asvgf-half | `--hybrid --half --upscaler Off` | A-SVGF with one quarter of pixel paths per frame |

Raw path tracing and hybrid use different primary-hit/sample organization and
direct-light treatment. The raw-hybrid row helps isolate filtering within the
hybrid pipeline. Treat the path-tracer comparison as a system comparison, not
an exact subtraction of filter cost. Half is a different ray budget, not an
isolated filter toggle. Do not use Half for the temporal-only or spatial-only
ablation: its missing pixels borrow neighbors even without a-trous filtering.

## Equal-resolution end-to-end comparison

Use the same output size and DLSS Quality mode for these three rows:

| Name | Flags | Actual internal/output sizes in the smoke check |
|---|---|---|
| asvgf-dlss-full | `--hybrid --upscaler DLSS --quality Quality` | 256x144 -> 384x216 |
| asvgf-dlss-half | `--hybrid --half --upscaler DLSS --quality Quality` | 256x144 -> 384x216 |
| rr-quality | `--hybrid --upscaler DLSS-RR --quality Quality` | 256x144 -> 384x216 |

At 1280x720 output, the expected internal size is 853x480 for every row; verify
the actual CSV and log rather than assuming ideal fractional dimensions.
DLSS-RR always traces every internal pixel. Its window-pane reflections still
use the engine's filtering. Reject RR runs with a fallback, failed NGX feature
creation, evaluation errors, or mismatched sizes.

Ordinary DLSS is itself temporal reconstruction. This table measures complete
solutions, including that reconstruction. Keep its error/time plot separate
from the native ablation. The raw path tracer cannot use the same temporal
upscaling path: it ignores `--upscaler`. A raw image rendered at 853x480 and
compared with a 1280x720 image after arbitrary resizing is not an equal-ratio
baseline. Do not mix it into this table or imply that all rows across the two
groups have equal internal resolution.

## Shared controls

Pin 1 spp, 5 path segments, exposure 1, ACES, and firefly clamp 0, with automatic
exposure and optional effects off. Use the same camera, scene contents, frame
count, and per-frame motion in each comparison group. Disable probes, radiance
cache, candidate resampling, and shadow maps unless they are an explicit factor.
Use seeds 17, 29, and 43 for the final three runs (17 alone in the smoke check),
alternate configurations, and take timing medians on one idle GPU. Record the
GPU, driver, output size, actual internal size, and effective upscaler.

For the raw path tracer, `--no-accumulation` selects independent frames and uses
`seed + zero-based frame index`, modulo 2^32, so a four-frame run starting at
17 uses 17, 18, 19, 20. At 1 spp every output frame contains one path sample per
pixel. The mode is restricted to `--gpu` without Hybrid/Raster, and it does not
change the default accumulation behavior. Merely disabling accumulation without
advancing the seed would repeat the same pixel RNG stream after every reset.
For comparisons across seeds, pair runs by their base seed and disclose this
stream policy; hybrid retains its existing global frame RNG stream.

Generate path-traced references at explicit end poses with normal accumulation
(`--gpu -f 2048 -s 4`). References must match each group's output size, exposure,
tone mapper, clamp, and bounce limit. Use a second independent reference seed to
measure its noise floor. Do not pass `--no-accumulation` to reference renders.

## Diagnosing no-temporal with DLSS

`spatial-dlss-reset` uses `--hybrid --no-temporal --upscaler DLSS --quality Quality`
at exactly the same dimensions as the end-to-end rows. It is diagnostic only.
Turning off the engine's temporal reuse currently also resets the upscaler
every frame. The CSV records the actual `UpscaleFrame::reset` value passed to
the active upscaler, not an inference from noise or an NGX estimate of effective
history length. The renderer's reset behavior is preserved by this work.

The expected four-frame reset sequence is 1,0,0,0 for normal A-SVGF plus DLSS and
1,1,1,1 with no-temporal. Therefore the latter cannot be described as a pure
spatial-only ablation with an otherwise identical temporal DLSS stage. Use the
native spatial-full row for that claim. This diagnostic does not quantify the
visual size of DLSS's history contribution or measure denoiser lag.

## Reproduce the minimal validation

After building, run from the repository root (the executable, scene, output,
configuration, and dimensions can all be supplied):

```powershell
StarTraveler/tools/eval/validate_protocol.ps1 -Exe build/msvc-vs2022/bin/Release/starcore-render.exe -Scene StarTraveler/assets/scenes/legacy_demo.yaml -Output renders/protocol
```

The runner checks all 11 configurations at four frames each and one additional
single-frame raw reference. It asserts actual dimensions, active upscaler,
accumulated frames, reset flags, and seed/sample accounting. It also checks that
the final raw frame is byte-identical to a fresh 1 spp image at seed 20, that
successive raw dumps differ, and that the accumulated control differs from raw.
Frame 1 is present in the CSV, even though the existing PNG dump path begins
with frame 2. The runner is a behavior validation tool, not the shared final
FLIP/timing/lag evaluator.

`--frame-stats path.csv` collects submission state including frame 1, buffers
rows in memory, then writes them after rendering. The reset flag describes the
request submitted to the vendor, not a guarantee of successful NGX evaluation;
logs and process exit still need checking. CSV state is not GPU completion data
and intentionally contains no ambiguously associated GPU timings. Keep these
diagnostic/dump runs separate from performance measurements.

See `renders/protocol/results.json` and `checks.json` for local validation results,
and [preflight](eval-denoiser-preflight.md) for environment information and the
[full GPU regression](gpu-regression.md), now executed with one unresolved DLSS
assertion. Subsequent [controlled AC results](denoiser-results.md)
contain the three production shots, 81-run tables, crops and a small lag curve.

## Observed validation results

On 2026-11-25, Release on RTX 3070 Laptop (8 GiB), driver 610.91, all 11
four-frame configurations and the independent one-frame reference passed.
Every PNG was verified to be 384x216 and nonconstant; raw, native A-SVGF, and RR
images were also visually inspected. This is a functional check, not a benchmark.

| Check | Observed result |
|---|---|
| Independent raw path frames | Seeds 17,18,19,20; accumulated frames 1,1,1,1 |
| Raw final versus standalone seed 20, 1 spp | Identical PNG SHA-256 |
| Consecutive raw PNG dumps | Three distinct hashes for frames 2,3,4 |
| Native temporal/A-SVGF history | Accumulated frames 1,2,3,4 |
| Native raw-hybrid/spatial-only history | Accumulated frames 1,1,1,1 |
| A-SVGF Full/Half with DLSS Quality | Both 256x144 -> 384x216; reset flags 1,0,0,0 |
| RR Quality | Actual DLSS-RR, 256x144 -> 384x216; reset flags 1,0,0,0 |
| Spatial-only with DLSS Quality | Same sizes and active DLSS; reset flags 1,1,1,1 |
| GPU path tracer requested DLSS Quality | Actual Off, 384x216 -> 384x216 |
| Default accumulated GPU render before/after tooling | Identical SHA-256 `E0DADEBAAB73EEB0741911F67CE8543E80737DEFA4AD957ECFF76DD86A7E0C7E` |

The image checks and default-output regression check are saved in
`renders/protocol/image-checks.json`. All 109 non-GPU CPU/game tests passed
again in 13.91 seconds; invalid CPU diagnostic and Hybrid no-accumulation
combinations were rejected before rendering. The Release rebuild included the
game, tools, and tests. The subsequent GPU regression has one unresolved DLSS
assertion; see [full regression](gpu-regression.md). The three-shot measurements
are separately recorded in [controlled AC results](denoiser-results.md).
