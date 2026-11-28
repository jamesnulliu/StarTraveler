# StarTraveler

Denoiser branch review: [upload scope and remaining work](GITHUB_UPLOAD.md). Submitted for pull-request review; no merge into main is performed.

Latest local denoiser evidence: [fixed-input Quality error/time results](docs/quality-fixed-exposure-results.md), [R2 materials](docs/final-materials/README.md) and [team handoff](HANDOFF.md).

**StarCore** is a small render engine: an ECS scene, glTF models, path tracers on the CPU and on
the GPU (Vulkan ray queries), the classic 2D raster algorithms and collision detection, usable
from C++ and from Python. **StarTraveler** is a game built on it in C++: fly a ship through a
path-traced universe from its 3D cockpit, draw shapes in space and open portals.

The game renders in real time with a hybrid pipeline: rasterized first hits, paths traced from
them with ray queries, samples reused across frames and denoised, then upscaled (DLSS, FSR or
the engine's own TAAU). The project's goals and status are in [TODO.md](TODO.md).

## Quick start

Team handoff: [交接说明](HANDOFF.md) and [未完成问题清单](OPEN_ISSUES.md).

Requirements: Visual Studio 2026 (or 2022) with the C++ workload, CMake 4.0+,
[vcpkg](https://github.com/microsoft/vcpkg) with `VCPKG_ROOT` pointing at it, and
[uv](https://docs.astral.sh/uv/) for Python. Ray tracing needs a GPU with `VK_KHR_ray_query`
(NVIDIA RTX, AMD RDNA2+, Intel Arc); without one the game path traces on the CPU or rasterizes.

```powershell
cmake --preset msvc                  # the first configure installs the vcpkg dependencies
cmake --build --preset msvc-release
ctest --preset msvc-release
build\msvc\bin\Release\StarTraveler.exe
```

The Python package (`starcore`):

```powershell
uv venv
uv pip install -e "./StarCore[test]"
uv run pytest StarCore/python/tests
```

## Controls

A click on the view captures the mouse; Esc opens the pause menu and frees it.

| Input | Pilot view (default) | Free camera |
|---|---|---|
| mouse | steer the ship | look around |
| W/S, A/D | thrust forward/back, strafe | fly forward/back, sideways |
| Space, Ctrl | thrust up, down | fly up, down |
| Shift | boost | |
| C | switch to the free camera | back to the cockpit |
| 1–7 | select a presentation profile | same |
| F2 | hide/show profile and actual GPU status | same |
| T | open a portal in front (two portals link up) | |
| Esc | pause menu | |

Editing, drawing, the render settings and the command line: [docs/playing.md](docs/playing.md).

## Repository

```
StarTraveler/                 repository root: a CMake workspace
├─ CMakeLists.txt  CMakePresets.json
├─ docs/                      the documentation below
├─ StarCore/                  the engine; also builds on its own
│  ├─ include/starcore/  src/    StarCore::Core (headless), ::Gpu (Vulkan), ::Runtime (window)
│  ├─ modules/                   C++20 named modules (starcore.raster)
│  ├─ shaders/                   GLSL shaders, compiled to SPIR-V at build time
│  ├─ tests/  examples/          Catch2 tests (tests/gpu: GPU vs CPU), starcore-render (CLI)
│  └─ python/                    the `starcore` package: bindings/, starcore/, tests/
└─ StarTraveler/              the game (C++ frontend of StarCore)
   ├─ src/  tests/
   ├─ assets/                 scenes/ (YAML), models/ (cockpit.glb, strange_rocks.glb)
   └─ tools/                  generate those models and the default scene with the Python API
```

## Documentation

- [Presentation profiles](docs/demo-presets.md): seven keyboard profiles,
  actual upscaler/resolution, live GPU timestamps, F2 and real-window validation.
- [Study bibliography](docs/references-denoiser.md) and [AI assistance record](docs/ai-assistance.md).

- [Building](docs/building.md): presets, CMake options, GPU requirements, C++ modules, the
  upscalers' SDKs, tests, the Python package.
- [Playing](docs/playing.md): command line, controls, editing and drawing, the game's render
  settings, effects, scenes and layers.
- [Engine](docs/engine.md): StarCore's parts (scene, assets, CPU path tracer, particles, 2D
  raster, physics, runtime) and its Python API.
- [GPU rendering](docs/gpu-rendering.md): how the renderer works: path tracing, the hybrid
  pipeline (denoising, direct and indirect light), the raster pipeline, upscalers,
  post-processing, and offscreen renders with `starcore-render`.
- [Models and scenes](docs/assets.md): regenerating the ship model and the blue star scene.
- [Denoiser comparison](docs/denoiser-protocol.md): configuration groups, independent raw
  frames, and minimal checks of resolution and DLSS history reset.
- [Evaluation shots](docs/evaluation-shots.md): the three camera/motion setups, scene
  preparation, minimal runs, and shared-tool handoff status.
- [Measurement validation](docs/measurement-validation.md): frame-associated GPU timing,
  reference noise, LDR-FLIP, and a tested finite-capture convergence rule.
- [Formal evaluation](docs/formal-evaluation.md): alternating three-seed collection,
  background GPU checks, checkpoints, and extended lag captures.
- [Final denoiser report](docs/final-denoiser-report.md): fixed-exposure production
  results, [figures, editable slides and speaker script](docs/final-materials/README.md).
- [Denoiser results](docs/denoiser-results.md): preserved 81-run historical AC
  comparison, reference sensitivity, crops, and the 512-frame small lag result.
- [Production settling results](docs/production-lag-results.md): fixed temporal
  input / DLSS K exposure, all three shots and seeds, two references, strict
  prefix checks and independently verified 720p convergence curves.
- [Full GPU regression](docs/gpu-regression.md): 49/50 GPU cases pass, one DLSS
  assertion reproduces, 109 CPU/game cases pass; shared evaluator remains unpublished.
- [DLSS model diagnosis](docs/dlss-diagnosis.md): explicit `--dlss-preset K`
  passes original color bounds; default Performance M remains a known failure.
  Defaults and the completed Quality dataset are preserved.
- [Final window backup](docs/final-window-rehearsal.md): accepted fixed-exposure
  real-window flight, 139.87 seconds, seven verified phases and chapter index.
- [Denoiser presentation](docs/denoiser-demo.md): 12-minute outline, Quality/K
  launch configuration, measurement wording and rehearsal/fallback plan.
- [StarCore/README.md](StarCore/README.md): the engine on its own, with C++ and Python examples.

## License

MIT, see [LICENSE](LICENSE).
