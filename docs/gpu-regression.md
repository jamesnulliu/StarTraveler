# Full GPU regression and upstream handoff

The complete Release GPU suite was executed on 2026-11-25–26 (America/Los_Angeles).
It is **not all green**: 49 GPU cases passed, one failed, none were skipped.
The separately re-run CPU/game suite passed all 109 cases. No renderer or test
assertion threshold was changed to obtain these results.

## Environment and coverage

- NVIDIA GeForce RTX 3070 Laptop GPU, driver 610.91, Vulkan 1.4.341.
- Incremental MSVC VS2022 Release build succeeded before testing.
- GPU test executable SHA-256:
  `57f89df2c8d7fcaeed6d7b4bec08b0e354a884b082cf3c3224d8a07cd1856dc7`.
- Render executable SHA-256 remained
  `b5a3164785840db1907d8a45989fc7a0240ddfa67175f6bbdc77563599adb9df`,
  identical to the completed denoiser evaluation.
- Fifty named cases were discovered through CTest, including effects, hybrid,
  raster, timing, upscaling and GPU/CPU reference checks. The runner used the exact
  discovered commands, including Catch's escaped commas and working directories.
- Cases ran serially and retained JUnit, stdout, commands and artifact hashes.
  Total GPU-case process time was 4335.69 seconds (72.26 minutes). This is test
  execution time, not a rendering-performance comparison. CPU/game took 11.38 s.
- The previous glossy-pane case passed in 155.00 s. Other valid cases needed
  188–382 s; the old externally imposed 180 s limit is unsuitable for this suite.
- Tests requested Vulkan validation layers, but the runtime warned that they are
  not installed. This functional regression does not establish validation-layer
  cleanliness.

The complete result is [the manifest](../renders/gpu-regression/manifest.json).
Its `complete: true` means every discovered case was attempted; `all_passed: false`
records the remaining failure. GPU test case folders are `case-01` through
`case-50`. [CPU JUnit](../renders/gpu-regression/cpu.junit.xml) is retained too.

| GPU group | Passed | Failed | Skipped |
|---|---:|---:|---:|
| Effects | 4 | 0 | 0 |
| Hybrid | 23 | 0 | 0 |
| Raster | 5 | 0 | 0 |
| Timing | 1 | 0 | 0 |
| Upscaling | 4 | 1 | 0 |
| Path tracing / CPU reference | 12 | 0 | 0 |

## The remaining failure

`GPU upscaling: fewer pixels rendered, the output's reconstructed` failed only
its ordinary-DLSS image-mean difference assertion. The test reconstructs a
192x144 raster image from 96x72 internal pixels at Performance quality, after
64 frames. TAAU and FSR are also exercised by the same generated test.

| Check | Observed | Required |
|---|---:|---:|
| DLSS `difference.mean` | 0.509729445 | <0.5 |
| DLSS `difference.worst` | 1.5 | <6.0 |

The mean is the test helper's block-image difference, not LDR-FLIP. Catch exited
42 with one failure element. Two isolated reruns with the original Catch random
seed 726919427 reproduced exactly 0.509729445. Their records are under
`renders/gpu-regression/reproduction/`. This is a reproducible boundary failure,
not a reason to round the result down or relax the threshold.

The failed test body, `upscaler.cpp`, and `upscaler_dlss.cpp` match the checked
upstream commit byte-for-byte after line-ending normalization. The whole test
file differs because of the added timing regression test. This source comparison
does not substitute for running a pristine upstream build, so it does not prove
whether the failure predates this branch or identify a driver/model root cause.

All other GPU cases passed, including the hybrid upscaling image test (which
generates TAAU, FSR, DLSS and DLSS-RR), upscaler fallback, and motion-trail checks.
The failure does not mean RR was unavailable. The PR's clean-regression gate
remains open until this ordinary-DLSS discrepancy is understood and resolved.

Subsequent [linear/exposure diagnosis](dlss-diagnosis.md) localizes the local
Performance bias to the M runtime/model path. Explicit K passes the unchanged
bounds; M remains failing. A new seven-case focused run passes six cases,
including K/model-switch checks, and retains the M failure. The user accepted
deferring further M investigation on 2026-11-26 to continue Quality evaluation
and presentation work. This does not change the historical 49/50 result or
establish a completely passing current GPU suite.

- [Original failure JUnit](../renders/gpu-regression/case-35/junit.xml)
- [First reproduction](../renders/gpu-regression/reproduction/junit-1.xml)
- [Second reproduction](../renders/gpu-regression/reproduction/junit-2.xml)
- [Upstream source comparison](../renders/gpu-regression/upstream-source-comparison.json)

## Reproduction and pausing

From the repository root, with this workspace's local CMake and evaluation Python:

```powershell
& ../.build-tools/cmake-4.2.3-windows-x86_64/bin/ctest.exe `
  --test-dir build/msvc-vs2022 -C Release -N --show-only=json-v1 -R '^GPU' |
  Set-Content ../.build-tools/gpu-tests-inventory.json -Encoding UTF8
& ../.build-tools/eval-venv/Scripts/python.exe StarTraveler/tools/eval/run_gpu_regression.py `
  --inventory ../.build-tools/gpu-tests-inventory.json `
  --cpu-report renders/gpu-regression-cpu.xml --output renders/gpu-regression
```

The CPU XML comes from CTest's `-E '^GPU' --output-junit` run. The current runner
expects this snapshot's 50 discovered GPU cases and verifies the inventory,
test-binary, runtime-DLL, hardware/driver and CPU-report fingerprints on resume.
It preserves passed, failed and skipped results rather than silently rerunning a
failure until it passes. Use another output directory for an independent rerun.

The account guard checks both five-hour and weekly windows approximately every
20 seconds. At exhaustion or an unavailable quota reading it writes
`pause.request`; the runner checks about every 0.2 seconds and terminates the
directly launched test process. Completed results survive; the interrupted case
is not recorded as complete. The Python process has no account API itself and
must run under that guard for unattended quota enforcement. Tests verified
in-flight cancellation and that a failure in JUnit cannot be hidden by exit 0.

## Shared evaluator availability

The remote was queried again after the regression. Both
`refs/heads/feature/eval-direct-light` and `refs/heads/main` remain at
`dde6e1bb85e6afa33fa2d1241d55b947ad46b8c3`. The tree contains the two scene/model
builders in `StarTraveler/tools`, but no `StarTraveler/tools/eval`, root evaluator,
benchmark or FLIP runner. There is still no published shared evaluator to adapt
against; no upstream integration is claimed.

[Upstream check](../renders/gpu-regression/upstream-check.json) records the remote,
heads, tree finding and timestamp. The local 81-run denoiser results remain
available; their actual-upscaler/error rejection is implemented locally, while
the corresponding shared-tool TODO stays open until that tool is published.
