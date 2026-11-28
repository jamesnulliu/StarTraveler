# StarTraveler

**StarCore** is a small render engine: an ECS scene, glTF models, path tracers on the CPU and on
the GPU (Vulkan ray queries), the classic 2D raster algorithms and collision detection, usable
from C++ and from Python. **StarTraveler** is a game built on it in C++: fly a ship through a
path-traced universe from its 3D cockpit, draw shapes in space and open portals.

The game renders in real time with a hybrid pipeline: rasterized first hits, paths traced from
them with ray queries, samples reused across frames and denoised, then upscaled (DLSS, FSR or
the engine's own TAAU). The project's goals and status are in [TODO.md](TODO.md).

## Quick start

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
| T | open a portal in front (two portals link up) | |
| 1-7 | a demo render preset (raster through path tracing) | |
| 0 | hide or show that preset's name and GPU time | |
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

- [Building](docs/building.md): presets, CMake options, GPU requirements, C++ modules, the
  upscalers' SDKs, tests, the Python package.
- [Playing](docs/playing.md): command line, controls, editing and drawing, the game's render
  settings, effects, scenes and layers.
- [Demo](docs/demo.md): the seven presets on keys 1-7, the timing overlay, a route through
  the blue star.
- [Engine](docs/engine.md): StarCore's parts (scene, assets, CPU path tracer, particles, 2D
  raster, physics, runtime) and its Python API.
- [GPU rendering](docs/gpu-rendering.md): how the renderer works: path tracing, the hybrid
  pipeline (denoising, direct and indirect light), the raster pipeline, upscalers,
  post-processing, and offscreen renders with `starcore-render`.
- [Direct-light evaluation](docs/eval-direct-light.md): reproduce the three-shot comparison,
  reference noise estimates, plots and crops.
- [Models and scenes](docs/assets.md): regenerating the ship model and the blue star scene.
- [StarCore/README.md](StarCore/README.md): the engine on its own, with C++ and Python examples.

## License

MIT, see [LICENSE](LICENSE).
