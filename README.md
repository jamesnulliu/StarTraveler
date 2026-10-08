# StarTraveler

**StarCore** is a small render engine: an ECS scene, glTF models, path
tracers on the CPU and on the GPU (Vulkan ray queries), the classic 2D raster
algorithms and collision detection, usable from C++ and from Python.
**StarTraveler** is a game built on it in C++: fly a ship through a
path-traced universe from its 3D cockpit, draw shapes in space and open
portals. It is the modern rewrite of
[XDrawBoard](https://github.com/jamesnulliu/XDrawBoard-Legacy).

```
StarTraveler/                 repository root: a CMake workspace
├─ CMakeLists.txt             adds StarCore and StarTraveler
├─ CMakePresets.json          msvc, msvc-vs2022, msvc-ninja, mingw, linux-gcc, linux-clang
├─ StarCore/                  the engine; also builds on its own
│  ├─ CMakeLists.txt  vcpkg.json  pyproject.toml  cmake/
│  ├─ modules/                   module interfaces: starcore.<component>.cppm + partitions
│  ├─ src/                       implementation units; StarCore::Core (headless), ::Gpu
│  │                             (Vulkan), ::Runtime (window)
│  ├─ include/starcore/          the only headers: logging and assertion macros
│  ├─ shaders/                   GLSL shaders, compiled to SPIR-V at build time
│  ├─ tests/  examples/          Catch2 tests (tests/gpu: GPU vs CPU), starcore-render (CLI)
│  └─ python/                    the `starcore` package: bindings/, starcore/, tests/
└─ StarTraveler/              the game (C++ frontend of StarCore)
   ├─ src/  tests/
   ├─ assets/                 scenes/ (YAML), models/cockpit.glb
   └─ tools/build_cockpit.py  generates the ship model with the Python API
```

## Building

Requirements: Visual Studio 2026 (or 2022) with the C++ workload, CMake 4.0+,
[vcpkg](https://github.com/microsoft/vcpkg) cloned somewhere with `VCPKG_ROOT`
pointing at it, and for Python [uv](https://docs.astral.sh/uv/). All C++
dependencies come from vcpkg (`StarCore/vcpkg.json`) on the first configure.

```powershell
cmake --preset msvc                  # Visual Studio 18 2026 / MSBuild, x64-windows-static-md
cmake --build --preset msvc-release
ctest --preset msvc-release
build\msvc\bin\Release\StarTraveler.exe
```

Other presets: `msvc-vs2022`, `msvc-ninja` (inside a VS developer shell), and
`linux-gcc` / `linux-clang` (not tested yet). C++23 is the default;
`-DSTARCORE_CXX_STANDARD=26` opts into C++26.

### C++ modules

The engine and the game are C++20 named modules: code says
`import starcore.scene;`, not `#include`. Every component is one module --
`starcore.common`, `.math`, `.raster`, `.asset`, `.scene`, `.render`,
`.physics` (StarCore::Core), `.vulkan`, `.gpu` (StarCore::Gpu), `.platform`,
`.ui`, `.app` (StarCore::Runtime), and the game's `startraveler.editor`,
`.gameplay`, `.game`. Its primary interface (`modules/starcore.<component>.cppm`)
exports one partition per former header (`modules/<component>/*.cppm`), and
`src/<component>/` holds the implementation units (`module starcore.scene;`)
and internal partitions (the GPU pipelines). Only macros, which modules cannot
export, are still headers: `starcore/common/log.hpp` and `assert.hpp`.

- The standard library is `import std;` everywhere. Visual Studio's MSBuild
  builds the module itself; with Ninja, CMake does, behind an experimental gate
  whose key changes with CMake's version (known for 4.2 and 4.4, see
  `StarCore/cmake/starcore-setup.cmake`). Configuring fails with an explanation
  when neither is available.
- glm comes from its own module (`import glm;`, glm's `glm.cppm`, compiled into
  StarCore::Core), which `starcore.math` re-exports. EnTT has none:
  `starcore.scene` exports what users need (`entt::entity`, `entt::null`,
  `entt::registry`, `entt::to_integral` and EnTT's operators), and code that uses
  more of it (views) includes all of `<entt/entity/registry.hpp>`. Libraries
  that ship no module (Vulkan, GLFW, ImGui, nanobind, Catch2, ...) are
  included where they are used; the bindings' nanobind type casters stay a
  header too (`python/bindings/casters.hpp`: MSVC crashes compiling nanobind's
  calls when they come from a module).
- MinGW: GCC 16.2 cannot link modules on Windows -- it emits the function-local
  statics of imported inline functions (e.g. `std::make_shared`'s, EnTT's type
  ids) without COMDAT, so every importer defines them again. The `mingw` preset
  therefore stops at configure time until GCC is fixed (details and a 3-line
  reproduction in `StarCore/cmake/starcore-modules.cmake`).

GPU path tracing needs a GPU with `VK_KHR_ray_query` (NVIDIA RTX, AMD RDNA2+,
Intel Arc); without one path tracing falls back to the CPU, the GPU only
rasterizes (any Vulkan 1.3 GPU with push descriptors) and the ray-tracing GPU
tests are skipped. No Vulkan SDK is needed: the shader compiler (glslang) comes
from vcpkg.
For development, `-DSTARCORE_VULKAN_VALIDATION=ON` also builds the Vulkan
validation layers (with synchronization validation); the GPU tests and
`StarTraveler.exe --validation` then use them. `STARCORE_VULKAN_DEVICE=Intel`
(any part of a device name) picks a specific GPU.

### Python

The `starcore` package wraps `StarCore::Core` with nanobind and is built by
scikit-build-core through the same CMake project:

```powershell
uv venv
uv pip install -e "./StarCore[test]"
uv run pytest StarCore/python/tests
```

```python
import starcore as sc

scene = sc.Scene()
ball = scene.create_entity("Ball")
ball.add(sc.Sphere(radius=1.0))
ball.add(sc.Material(albedo=(1.0, 0.75, 0.3), metallic=1.0, roughness=0.4))

camera = sc.Camera()
camera.set_viewport(320, 180)
tracer = sc.CpuPathTracer()
tracer.resize(320, 180)
for _ in range(64):
    tracer.render(sc.build_render_scene(scene), camera, sc.PathTracerSettings())
sc.write_png(tracer.image(), "ball.png")   # tracer.image() is a numpy array
```

For C++/Python iteration without reinstalling, the `msvc-python` preset builds
the extension into `StarCore/python/starcore` (`$env:PYTHONPATH = "StarCore/python"`).

## Playing

| Input | Pilot view (default) | Free camera |
|---|---|---|
| W/S, A/D, E/Q | thrust forward/back, strafe, up/down | fly (while RMB is held) |
| hold RMB + mouse | steer the ship | look around |
| Shift | boost | |
| C | switch to the free camera | back to the cockpit |
| T | open a portal in front (two portals link up) | |
| Ctrl+N/O/S, Ctrl+D, Del, Esc | new/open/save scene, duplicate, delete, cancel tool | |

The drawing tools (line, ellipse, arc, polygon, curve, group, seed fill) work
in both views; shapes are placed `Draw depth` units in front of the camera.
The viewport is rendered on the GPU in real time by the hybrid pipeline
(rasterized first hits, path tracing, samples reused across frames and
denoised; see below), with samples per frame following a GPU time budget (8 ms
by default). The Render panel switches to plain path tracing (the reference,
which restarts whenever anything moves), tunes the denoiser and shows its
intermediate images. With `--cpu`, or on GPUs without ray queries, the CPU path
tracer shows a noise-free preview while moving and path traces when still.
Turning "Ray tracing" off (or `--raster`) rasterizes on the GPU instead: direct
light without shadows or reflections, no rays at all, about 0.2 ms per frame.
On the GPU the game adds bloom, motion blur (the cockpit stays sharp while the
world rushes by) and a vignette, and draws particles: engine exhaust that
follows the thrust, sparks when the hull scrapes something and space dust that
streaks past when flying fast (panel "Effects"; particle emitters can be added
to any entity in the inspector or with Create > Particle emitter).
Layers (panel "Layers") control visibility, locking and which layers collide;
the ship's hull and cockpit live on the locked "Ship" and "Cockpit" layers.

`StarTraveler.exe [scene.yaml] [--cpu] [--raster] [--cruise] [--frames N --screenshot out.png] [--validation]`
(`--cruise` flies forward at full thrust, for screenshots of the effects).

## Engine overview

- **Scene / ECS** (EnTT): entities with plain-data components -- transform,
  hierarchy, layer, material, sphere, mesh, collider, 2D shapes, portal. YAML
  scene files; meshes from models are saved as references to their glTF file.
- **Assets**: immutable shared `MeshData` (with its BVH), procedural primitives,
  glTF 2.0 loading (fastgltf) and `.glb` writing, an `AssetLibrary` cache.
- **Rendering**: a render scene shaped like Vulkan ray tracing input (BLAS per
  mesh, TLAS over instances, 8-bit instance masks), kept up to date
  incrementally by a `RenderSceneBuilder`: every entity keeps its instance
  slot and each change is stamped, so only what changed is rewritten and
  uploaded. A progressive, multi-threaded CPU path tracer with a GGX
  metal/dielectric BSDF, thin and refracting glass, next event estimation of
  sphere lights with MIS, a preview mode, coarse-to-fine passes and a
  per-frame time budget.
- **GPU rendering** (`StarCore::Gpu`, headless Vulkan, `GpuRenderer`): the
  render scene becomes storage buffers updated per changed instance and, with
  ray queries, a BLAS per mesh (cached across frames, spheres as procedural
  AABBs) and a TLAS rebuilt when instances move. Three pipelines:
  - *Path tracing*: a line-by-line port with the same random streams, so GPU
    and CPU images agree almost bit for bit (checked by the tests); in the
    cockpit about 70x faster than the CPU. The reference.
  - *Hybrid* (the game's default): a visibility buffer is rasterized (triangle
    ids; spheres as ray-cast impostors) with a jittered projection, and paths
    start from those hits with the same estimator. Smooth window panes form a
    layer of their own: paths are split there into what is seen through them
    and what they reflect, each with its own depth, normals and motion
    vectors, so a cockpit canopy neither smears the world outside nor its own
    reflections. Samples are reprojected across frames (motion vectors from
    each instance's previous transform, checked against the previous frame's
    surfaces) and the lighting, divided by the albedo, goes through SVGF's
    edge-avoiding a-trous filter, which fades out as samples accumulate: a
    still image converges to the path tracer's (also checked by the tests).
    Rasterized first hits are where textures will be sampled.
  - *Raster*: the same visibility buffer lit like the preview (sky, ambient,
    headlight, sphere lights) without shadows, the nearest window pane blended
    over: no rays, so it also runs on GPUs without ray tracing hardware;
    jittered frames are averaged while the view is still (anti-aliasing).
  Every pipeline writes linear radiance; a last stage draws particles over it
  (additive sprites, streaked along their screen motion, tested against the
  rasterized depth), adds bloom (a downsampled chain of blurs, Jimenez's
  filters), motion blur (McGuire et al.'s reconstruction filter over the
  motion vectors and depths) and a vignette, and tone maps. With every effect
  off the image is exactly the tone-mapped one, as the CPU's.
  `starcore-render scene.yaml --gpu` (or `--hybrid`, with `--move`, `--view`,
  or `--raster`; `--bloom`, `--motion-blur`, `--vignette`) renders offscreen.
- **Particles**: a CPU particle system (emitter components spreading their
  particles along the path they moved, bursts), drawn by the GPU renderer.
- **2D raster**: Bresenham/midpoint/DDA lines, midpoint ellipses, Bezier and
  B-spline curves, Cohen-Sutherland / midpoint-subdivision / Sutherland-Hodgman
  clipping, scan-line and flood fill -- the algorithms of the original drawing
  board, drawn as a world-space overlay.
- **Physics**: sphere, box and mesh colliders; raycasts, continuous sphere
  casts, overlaps and collide-and-slide, filtered by the layer collision matrix.
- **Runtime**: GLFW window, Vulkan (vk-bootstrap, VMA), Dear ImGui docking with
  multi-viewports, an application/layer framework.

Planned: Python bindings of the GPU renderer and the runtime, for games
written in Python.

## Regenerating the ship model

```powershell
uv run python StarTraveler/tools/build_cockpit.py
```

The script assembles the cockpit and hull from StarCore primitives and writes
`StarTraveler/assets/models/cockpit.glb`. Any glTF with a "Cockpit" and a "Hull"
root node in the same ship-local frame (-Z forward, eyes at (0, 0.42, 0.15)) can
replace it, e.g. a model exported from Blender.
