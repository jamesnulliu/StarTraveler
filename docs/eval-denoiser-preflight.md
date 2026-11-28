# Denoiser evaluation preflight

Initial check and build follow-up on 2026-11-25. **Release builds successfully,
and DLSS-RR has now rendered successfully on this machine.** Hardware and smoke
results are runtime observations; parameter caveats remain source-inspection
findings unless explicitly validated below. No formal quality or performance
comparison has been measured.

Subsequent protocol work added independent raw frames (`--no-accumulation`) and
submission-state CSV (`--frame-stats`). The native and DLSS Quality groups,
known-answer raw check, and measured DLSS reset sequences are now validated in
[denoiser protocol](denoiser-protocol.md). The initial CLI limitation below has
been resolved by that opt-in tooling; the original RR smoke run is retained here.

The three production shot definitions and local batch handoff have subsequently
passed moving-scene smoke and exact-endpoint checks; see
[evaluation shots](evaluation-shots.md). The upstream direct-light branch was
checked and still has no shared evaluator at the recorded commit.

The subsequent [measurement validation](measurement-validation.md) fixed the
timestamp slot-order issue and added frame-associated timing export, generated
the three 720p reference pairs, and validated FLIP plus a finite still-frame rule.
Historical timing/CLI findings below describe the initial snapshot; current
measurement behavior and limitations are in that validation document.

## Completed build and smoke validation

Installed Microsoft-signed Visual Studio Build Tools 2022 side by side at
`C:/BuildTools2022`: version 17.14.37906.3, MSVC 19.44.35230.0 (toolset directory
14.44.35207), Windows SDK 10.0.26100.0. Installer exit code was 0 and no reboot was
required. CMake 4.2.3 was downloaded from Kitware's release and verified against
its SHA-256 manifest. CMake and vcpkg are under the workspace's sibling
`.build-tools/` directory; these are not global PATH installations. vcpkg version
is 2026-10-29-139f2e53438b1529e178f6295bc563f4e0de734f, with metrics disabled.

All 19 manifest dependencies installed successfully. FSR 3.1 and DLSS SDK 310.9.1
were downloaded and hash-checked by the project's configure step. The
`msvc-vs2022` configure and `msvc-vs2022-release` build completed with exit code 0,
including the game, renderer, CPU/GPU tests, and game tests. `import std` remained
enabled. No renderer source change was needed.

Three smoke renders used `legacy_demo.yaml`, output 1280x720, 16 frames, 1 spp,
seed 17, camera (0, 2, 10), look-at (0, 0, 0), and ACES tone mapping.

| Configuration | Active upscaler | Internal size | Result |
|---|---|---|---|
| Hybrid Full, native | Off | 1280x720 | Exit 0; nonconstant PNG |
| Hybrid Full, DLSS Quality | DLSS | 853x480 | NGX available; exit 0; nonconstant PNG |
| Hybrid Full, DLSS-RR Quality | DLSS-RR | 853x480 | NGX available; exit 0; nonconstant PNG; visually inspected |

RR's actual log includes `DLSS-RR: available` and
`Upscaled with DLSS-RR: 853x480 -> 1280x720`. All three logs contain no logged
render errors, feature-creation failures, or NGX evaluation failures. Images
were checked for output dimensions and sampled color variation; RR's image was
also opened for visual inspection. This verifies RR feature creation and
evaluation at this size, not comparative denoising quality or general stability.

All 109 non-GPU CPU/game tests passed in 12.77 seconds, using the CTest filter
`-E '^GPU'`. No tests in that run were skipped. The unfiltered suite has 158
tests. An exploratory unfiltered run passed its first 99 tests (including four
GPU effects tests), then `GPU hybrid: a glossy surface a pane reflects keeps
its history while moving` hit the explicitly imposed 180-second timeout. The
remaining GPU suite was stopped; it is not a passing full regression result.
That case's performance/initialization time and assertions need a separate run
with sufficient time before a PR. An intermediate attempted non-GPU filter
`^GPU ` missed names beginning `GPU:`; that run was interrupted and is superseded
by the successful `^GPU` run. Its interrupted result is not a renderer assertion
failure. No GPU tests or renderer processes remain running.

The complete non-GPU results are in `../.build-tools/ctest-non-gpu.log`; the
partial unfiltered log is `../.build-tools/ctest-release.log`. Runtime GPU tests
also warned that Vulkan validation layers are not installed. The build used
the repository default `STARCORE_VULKAN_VALIDATION=OFF`, so this check does not
establish validation-layer cleanliness.

Artifacts are in `renders/preflight/`: `hybrid-native`, `hybrid-dlss-quality`,
and `hybrid-rr-quality`, each as `.png` and `.log`. `results.json` records the
arguments, image checks, and SHA-256 hashes. The local smoke runner is
`../.build-tools/run-smoke.ps1`; it explicitly rejects fallback upscalers,
unexpected sizes, logged errors, and nearly constant images. The shared
evaluation tool still needs equivalent checks.

Build/configure logs are `../.build-tools/configure.log` and
`../.build-tools/build-release.log`. From the repository root, repeat the build
using this machine's installed tools:

```powershell
$tools = (Resolve-Path ../.build-tools).Path
$env:VCPKG_ROOT = Join-Path $tools vcpkg
$env:VCPKG_DISABLE_METRICS = '1'
$env:PATH = (Join-Path $tools 'cmake-4.2.3-windows-x86_64/bin') + ';' + $env:PATH
cmake --preset msvc-vs2022 -DCMAKE_GENERATOR_INSTANCE=C:/BuildTools2022
cmake --build --preset msvc-vs2022-release --parallel 4
# Completed CPU/game check:
ctest --test-dir build/msvc-vs2022 -C Release -E '^GPU' --output-on-failure --timeout 180
# Full regression, still required before a PR (allow long GPU initialization):
ctest --test-dir build/msvc-vs2022 -C Release --output-on-failure
```

Repeat just the RR smoke render from the repository root:

```powershell
build/msvc-vs2022/bin/Release/starcore-render.exe StarTraveler/assets/scenes/legacy_demo.yaml --hybrid --upscaler DLSS-RR --quality Quality -W 1280 -H 720 -f 16 -s 1 --seed 17 --camera 0 2 10 --look-at 0 0 0 --tone-mapper ACES -o renders/preflight/hybrid-rr-quality.png
```

The initial environment findings below are retained as the pre-install snapshot.

## Initial environment

| Check | Result |
|---|---|
| GPU | NVIDIA GeForce RTX 3070 Laptop GPU, 8192 MiB |
| NVIDIA driver | 610.91 (`nvidia-smi` and `vulkaninfo`) |
| Vulkan | Loader and device report 1.4.341 |
| Required GPU extensions | `VK_KHR_ray_query`, `VK_KHR_acceleration_structure`, `VK_KHR_buffer_device_address`, `VK_KHR_push_descriptor` reported |
| GPU timestamps | `timestampComputeAndGraphics = true`, `timestampPeriod = 1` ns |
| Visual Studio | Enterprise 2022, 17.3.32901.215 (17.3.4) |
| MSVC C++ tools | Not detected by `vswhere -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64`; installation has no `VC/Tools/MSVC` |
| CMake, CTest, Ninja, uv, Python | Not found on PATH; VS-bundled CMake/Ninja not found at their standard locations |
| vcpkg | `VCPKG_ROOT` unset; not found in checked standard locations |
| Build artifacts | No build directory, CMake cache, or `starcore-render.exe` in the supplied source tree |
| Git | Git executable available; supplied source tree has no `.git` metadata |
| Scene assets | `blue_star.yaml`, `cockpit.glb`, and `strange_rocks.glb` present |
| Shared evaluation scripts | `StarTraveler/tools/eval/` absent |

`vulkaninfo` completed with a loader warning about registry layer manifests.
The device and required extensions were still enumerated. The Khronos validation
layer was not among the enumerated layers. The CIM video-controller query was
denied; the hardware conclusions use NVIDIA-SMI and Vulkan instead.

The machine meets the inspected GPU capability requirements. This does not prove
that this application's hybrid pipeline or DLSS-RR runs successfully.

## Initial build and DLSS-RR gates

The initial environment could not build the project: the C++ tools and dependency
toolchain are missing from the checked locations. The default `msvc` preset asks
for Visual Studio 2026; this machine has 2022. The repository also provides an
`msvc-vs2022` configure preset and `msvc-vs2022-release` build preset, but no test
preset of that name. The installed 17.3.4 toolchain has not been validated for
this project's C++23/modules requirements; installing the C++ workload alone
does not establish compiler compatibility.

Before rendering, provision a supported C++ toolchain, CMake 4.0+, and vcpkg with
`VCPKG_ROOT` set. Python/uv and evaluation dependencies are also needed for the
later analysis. Use a Git checkout before creating branches or a pull request.
No software installation, download, source commit, or renderer change was made
as part of this check.

DLSS support defaults on, but requires Windows x64/MSVC and successful download
of the pinned DLSS SDK 310.9.1 into the build tree. Both `nvngx_dlss.dll` and
`nvngx_dlssd.dll`, NGX linkage, and the compile definition `STARCORE_HAS_DLSS`
must be present. SDK downloads can fail while leaving vendor upscalers out of
the build; a successful build alone is insufficient evidence of RR support.

Runtime acceptance must include all of the following:

1. The application's Vulkan context and hybrid pipeline initialize.
2. NGX reports `DLSS-RR: available`.
3. RR feature creation succeeds at the chosen render/output sizes.
4. Evaluation produces no `DLSS-RR: evaluating failed` error and writes an image.
5. The final log reports `Upscaled with DLSS-RR` and the expected actual sizes.

The renderer can fall back from RR to DLSS, FSR, and TAAU. Reject such a run as an
RR measurement. Even `activeUpscaler()` being RR is insufficient if evaluation
logged an error: `NgxUpscaler::record` logs that failure without changing the
active-upscaler label or failing the render command.

## Parameter behavior

| Parameter | Behavior confirmed in source | Evaluation consequence |
|---|---|---|
| `--gpu -s 1` | Path tracing; accumulates samples while camera/scene remain unchanged | A static multi-frame render is not a single-sample baseline |
| `--gpu` during motion | Camera or scene-version changes reset accumulation and its RNG frame index | Moving frames use one frame of samples, but reuse the same per-pixel random stream index at each reset |
| `--hybrid` | CLI defaults to Full tracing, temporal and spatial filters enabled | The CLI default differs from the game's Half default |
| `--no-spatial` | Sets a-trous iterations to zero; temporal pass remains | At Full with upscaler Off this isolates temporal reuse; Half still borrows neighboring samples |
| `--no-temporal` | Resets hybrid history every frame; hybrid uses the global frame number for fresh RNG/jitter | Spatial filtering remains, but a temporal upscaler is also reset every frame |
| `--half` | One pixel in each 2x2 block is traced per frame, rotating over four phases | Approximately one quarter of pixel paths, not half; does not reduce output dimensions |
| RR with `--half` | RR forces trace stride 1 | There is no distinct RR Half configuration |
| RR filtering | Skips the engine temporal/a-trous passes for layers other than Glass | Window-pane reflections retain the engine's filtering |
| `-s N` | Samples per pixel per frame; default 1 | Pin explicitly in every configuration |
| `-b N` | Passed to both GPU path tracing and hybrid uniforms; default 5 | Propagation is confirmed; equivalent direct-light-only images still need a known-answer render |
| `--seed N` | Passed to the GPU RNG; default 0 | Use explicit seeds and record them |
| `--upscaler` | CLI default Off; path-tracing pipeline ignores upscaling | Adding DLSS flags to `--gpu` does not make its internal resolution match an upscaled hybrid |
| `--quality` | Native 1, Quality 1/1.5, Balanced 1/1.7, Performance 1/2, Ultra performance 1/3 per dimension | Dimensions are rounded with `lround`; record actual sizes from the log |
| `--move x y z` | World-space camera displacement per moving frame | Motion is per frame, independent of wall time |
| `--turn-entity name a` | Rotates the entity about world Y by negative `a`; camera position orbits its origin and camera yaw increases by `a` | Preserve identical frame counts to preserve the final pose |
| `--still N` | Last N frames are still; initial setup frame never moves | For total F frames and 0 <= N < F, there are F-N-1 motion updates |
| `--dump-every 1` | Dumps frames 2 through F as `stem_0002.png` etc.; frame 1 is not dumped | Use the written frame index, not array position, for lag analysis |

For F total frames, S still frames, and 1 <= S < F, the first still frame is
number F-S+1 (one based). A moving render of M frames and a reference render of
M+2048 frames with `--still 2048` both apply M-1 motion updates. The reference
accumulates the last moving frame at the final pose plus the 2048 still frames,
so at `-s 4` it has 8196 spp when motion resets accumulation on that final move,
not exactly 8192. Use an explicit end-pose scene with `--gpu -f 2048 -s 4` for an
exact 8192 spp reference. Invalid or excessive `--still` values are not bounded
against F by the CLI and should be rejected by the evaluation tool.

The initial CLI had no exposed switch to disable path-tracer accumulation.
`--gpu --no-accumulation -s 1` now supplies a sustained raw baseline with the seed
advanced by the zero-based frame index. A four-frame raw run's last image was
verified identical to a standalone one-sample image at the matching seed. The
native ablation and equal-resolution DLSS Quality comparison are separate groups;
the raw path tracer still does not upscale. See the protocol for actual results.

With `--no-temporal --upscaler DLSS`, `needsReset` sets the hybrid frame index to
zero every frame, and `upscalerReset` consequently becomes true every frame.
This is not the same temporal-upscaling treatment as normal A-SVGF plus DLSS.
Report this behavior or separate the native-resolution ablation from the
end-to-end DLSS comparison rather than calling them isolated filter timings.

## Time and lag measurement

The log's `GPU: ... ms/frame after setup` is CPU wall time around rendering and
the final image readback. Intermediate readbacks, overlays, and PNG writes are
inside that interval when dumping is enabled. Do not use that number as GPU
time or compare it across dump/no-dump runs.

The parenthesized GPU/trace times come from timestamp queries, not a multi-frame
average. `waitIdle()` collects frame slots in slot order, so the final values
are not guaranteed to belong to the chronological last frame despite the log's
`last frame` label. Per-frame dump runs wait each frame, but have intrusive
readbacks and output work. Run lag captures separately from performance runs;
correctly associate timestamp results with frames before using final-frame
timings. The trace interval also excludes other ray work such as temporal
gradients, so it is not total ray-tracing cost.

When A-SVGF is still, its temporal-history cap becomes zero (uncapped), and the
spatial filter fades as variance decreases. Error may continue decreasing
throughout a finite capture. Define the reported terminal error as a statistic
over a fixed tail window, disclose that window and capture length, and require
sustained entry within 10% of that value. If the tail is still trending, extend
the capture or report that settling was not established. Do not claim a fixed
steady-state error from the `historyFrames = 64` moving-history setting.

## Next validation

The build, smoke tests, reference pairs, FLIP, timestamps, alternating 81-run
comparison, tables/plots/crops, and extended 512-frame small lag capture are
complete; see [controlled AC results](denoiser-results.md). The subsequent
[full GPU regression](gpu-regression.md) ran every case: 49 pass, one reproducible
ordinary-DLSS reconstruction assertion fails, zero skipped. The old glass-history
case passed in 155 seconds. Resolve the DLSS discrepancy before claiming a clean
regression/PR gate. Upstream shared-evaluator integration remains pending: a
fresh remote check still finds no published evaluator at `dde6e1b`.

Source entry points: `StarCore/examples/render.cpp`,
`StarCore/src/gpu/gpu_renderer.cpp`, `StarCore/src/gpu/hybrid_renderer.cpp`,
`StarCore/src/gpu/upscaler_dlss.cpp`, `StarCore/shaders/path_trace.comp`,
`StarCore/shaders/temporal.comp`, `StarCore/shaders/atrous.comp`,
`StarCore/include/starcore/gpu/gpu_renderer.hpp`, and
`StarCore/cmake/libraries/upscalers.cmake`.
