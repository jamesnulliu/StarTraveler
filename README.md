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
│  ├─ include/starcore/  src/    StarCore::Core (headless), ::Gpu (Vulkan), ::Runtime (window)
│  ├─ modules/                   C++20 named modules (starcore.raster)
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

Other presets: `msvc-vs2022`, `msvc-ninja` (inside a VS developer shell),
`mingw` (GCC from `scoop install mingw-winlibs`), and `linux-gcc` /
`linux-clang` (not tested yet). C++23 is the default; `-DSTARCORE_CXX_STANDARD=26`
opts into C++26.

C++20 modules: the 2D raster algorithms are the named module `starcore.raster`
(`StarCore/modules/`), imported by the overlay renderer, the editor, the tests
and the Python bindings (`import starcore.raster;`); the rest of the engine is
still headers. With `STARCORE_IMPORT_STD` (default ON) the module's units
`import std;` where the toolchain can: with Visual Studio MSBuild builds the
standard library module; with Ninja CMake does, behind an experimental gate
whose key changes with CMake's version (known for CMake 4.2 and 4.4; others
fall back to including the standard headers). Toggling it needs a fresh
configure (`cmake --preset mingw --fresh`): the compiler check is cached.

GPU path tracing needs a GPU with `VK_KHR_ray_query` (NVIDIA RTX, AMD RDNA2+,
Intel Arc); without one path tracing falls back to the CPU, the GPU only
rasterizes (any Vulkan 1.3 GPU with push descriptors) and the ray-tracing GPU
tests are skipped. No Vulkan SDK is needed: the shader compiler (glslang) comes
from vcpkg.
For development, `-DSTARCORE_VULKAN_VALIDATION=ON` also builds the Vulkan
validation layers (with synchronization validation); the GPU tests and
`StarTraveler.exe --validation` then use them. `STARCORE_VULKAN_DEVICE=Intel`
(any part of a device name) picks a specific GPU.

The vendors' upscalers come from their SDKs, downloaded on configure into
`build/<preset>/_deps/upscalers` (checked against fixed hashes, not part of the
repository): AMD FSR 3.1 (FidelityFX SDK 1.1.4, MIT; its prebuilt Vulkan DLL,
Windows, any compiler) and NVIDIA DLSS (DLSS SDK 310.9.1, NVIDIA's RTX SDK
license; its NGX library links with MSVC only, so the MinGW build has FSR and
the engine's own TAAU). `-DSTARCORE_FSR=OFF` / `-DSTARCORE_DLSS=OFF` leave them
out; without network access they are left out too.

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

A click on the view captures the mouse: it disappears behind the crosshair and
steers until Esc opens the pause menu (the ship and the particles stand still,
the mouse comes back; Esc or a click on the view flies on).

| Input | Pilot view (default) | Free camera |
|---|---|---|
| mouse | steer the ship | look around |
| W/S, A/D | thrust forward/back, strafe | fly forward/back, sideways |
| Space, Ctrl | thrust up, down | fly up, down |
| Shift | boost | |
| C | switch to the free camera | back to the cockpit |
| T | open a portal in front (two portals link up) | |
| Esc | pause menu | |
| Ctrl+N/O/S, Ctrl+D, Del | new/open/save scene, duplicate, delete (while the mouse is free) | |

The Tools panel switches from flying (Fly) to selecting and editing (Select)
and the drawing tools (line, ellipse, arc, polygon, curve, group, seed fill);
they take the clicks on the view instead, hold RMB to look around meanwhile,
and Esc goes back to flying. They work in both views; shapes are placed
`Draw depth` units in front of the camera.
The viewport is rendered on the GPU in real time by the hybrid pipeline
(rasterized first hits, paths from a quarter of the pixels per frame, samples
reused across frames and denoised; see below), with samples per frame
following a GPU time budget (8 ms by default; less for a higher frame rate).
As games do, it renders 2/3 of the view's width and height and DLSS
reconstructs the rest (FSR where DLSS cannot run, else the engine's TAAU): about
half the GPU time at 1080p. The Render panel's "Upscaling" picks the upscaler
(or Off) and how much is rendered, from Native (anti-aliasing only) to Ultra
performance (1/3); `--upscaler Off|TAAU|FSR|DLSS` and `--quality` set them at
start.
The sphere lights' direct light is shadowed with shadow maps every frame, as
games do ("Direct shadows" in the Render panel; Traced accumulates shadow rays
instead, converging to the path tracer's image when still).
The Render panel switches to plain path tracing (the reference, which restarts
whenever anything moves), traces every pixel every frame (Trace resolution
Full: less noise while moving, almost twice the GPU time), tunes the denoiser
and shows its intermediate images. With `--cpu`, or on GPUs without ray queries, the CPU path
tracer shows a noise-free preview while moving and path traces when still.
Turning "Ray tracing" off (or `--raster`) rasterizes on the GPU instead: direct
light with soft shadows from shadow maps, no reflections or bounced light, no
rays at all, about 2 ms per frame in the cockpit (0.4 ms without the shadows).
On the GPU the game adds bloom, motion blur (the cockpit stays sharp while the
world rushes by) and a vignette, and draws particles: engine exhaust that
follows the thrust, sparks when the hull scrapes something and space dust that
streaks past when flying fast (panel "Effects"; particle emitters can be added
to any entity in the inspector or with Create > Particle emitter).
Layers (panel "Layers") control visibility, locking and which layers collide;
the ship's hull and cockpit live on the locked "Ship" and "Cockpit" layers.

`StarTraveler.exe [scene.yaml] [--cpu] [--raster] [--cruise] [--upscaler NAME] [--quality NAME] [--frames N --screenshot out.png] [--validation]`
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
    reflections. Each sample per pixel is a path of every layer. Samples are
    reprojected across frames (motion vectors from each instance's previous
    transform, checked against the previous frame's surfaces; sharp
    reflections move with their reflected image) and the lighting, divided by
    the albedo, goes through SVGF's edge-avoiding a-trous filter, which fades
    out as samples accumulate: a still image converges to the path tracer's
    (also checked by the tests). Where the light changed, the history is
    dropped (A-SVGF's temporal gradients: each frame re-traces some of the
    previous frame's samples with their random numbers; of the sphere lights'
    highlights on glossy surfaces, their closed forms then and now, without
    noise), so neither a moving shadow nor a highlight sliding along a glossy
    tube trails, nor the sunlit patch a canopy reflects as the ship turns. The
    light the sphere lights (a sun) give the surfaces' diffuse lobe is a layer
    of its own, as a ratio estimator (as engines shade lights and denoise only
    their shadows): what it would be unshadowed is in closed form every frame,
    so it follows the light at once, and only how much of it arrives
    accumulates. Or (`DirectShadows::ShadowMaps`, the game's, as engines light
    with their lights) nothing of it accumulates: each light's closed form is
    shadowed every frame with the raster pipeline's shadow maps (below), a
    light without them with a ray to its center; where a big light's center is
    below a surface's horizon, a ray to the middle of its part above it (the
    maps' receiver test needs the center above). No noise, no lag behind moving
    shadows (a test sweeping a light: 9x less), the same error against the
    path tracer in the cockpit and 22% less noise while turning, at the same
    cost; the soft shadows are approximated. Native, a recent mean anti-aliases
    it, clipped to this frame's around each pixel (mean +- a standard
    deviation, so smooth light moving does not lag); upscaled, the upscaler
    does.
    At half trace resolution (the game's) paths start from one pixel of each
    2x2 per frame, each in turn; the others borrow their neighbors' samples on
    the same surface: only those until they have their own (just uncovered),
    and while things move as much as one frame of their own (the more of them,
    the less history the gradients left), not into their history, so a still
    image still converges, and the cockpit flies at 6.1 instead of 10.7 ms per
    frame (932x856, an RTX 4060 laptop GPU), about 50% noisier.
    Rasterized first hits are where textures will be sampled.
  - *Raster*: the same visibility buffer lit like the preview (sky, ambient,
    headlight, sphere lights), the nearest window pane blended over: no rays,
    so it also runs on GPUs without ray tracing hardware; jittered frames are
    averaged while the view is still (anti-aliasing). Shadows come from shadow
    maps: the brightest sphere lights render the scene's depth from their
    center into one atlas -- six views around a light near the camera, three
    views nested around the camera for a far one (cascades, moved by whole
    texels so edges do not crawl) -- and shading compares each surface's
    distance with them, per texel against the surface's plane (no acne at any
    slope), softened over the penumbra the light's size and the occluders'
    distance make (percentage-closer soft shadows). Their shadows match the
    path tracer's shadow rays (checked by the tests).
  Every pipeline writes linear radiance. Hybrid and Raster may render fewer
  pixels than the output has (`GpuRenderOptions::upscaling`): each frame at
  another position in its pixels (the jitter, a Halton sequence), from which a
  temporal upscaler reconstructs the output's pixels, reprojecting the earlier
  frames with the motion vectors (which leave the jitter out) -- how games make
  ray tracing affordable at high resolutions. Three of them: *TAAU*, the
  engine's own (each output pixel blends the rendered samples near it into its
  history, reprojected with the motion of the nearest surface around and
  clipped to the colors around it, Salvi's variance clipping), and the
  vendors' *FSR 3.1* (AMD, any GPU) and *DLSS* (NVIDIA RTX GPUs); one that
  cannot run falls back to FSR, then TAAU. Upscaled, the hybrid pipeline
  composes each frame with that frame's albedo, emission and unshadowed light
  (the filtered lighting stays as it is), so the upscaler gets every frame's
  own samples. In the cockpit at 1920x1080 (an RTX 4060 laptop GPU, Quality:
  2/3 of each side) a frame takes 9.2 ms with TAAU, 9.9 with FSR, 11.2 with
  DLSS instead of 19.6, with the same noise and error against the path tracer
  while turning and cruising; DLSS looks cleanest, TAAU the softest.
  A last stage, at the output's size, draws particles over it (additive
  sprites, streaked along their screen motion, tested against the rasterized
  depth), adds bloom (a downsampled chain of blurs, Jimenez's filters), motion
  blur (McGuire et al.'s reconstruction filter over the motion vectors and
  depths) and a vignette, and tone maps. With every effect off the image is
  exactly the tone-mapped one, as the CPU's.
  `starcore-render scene.yaml --gpu` (or `--hybrid`, with `--move`,
  `--turn-entity`, `--view`, `--half`, `--shadow-maps`, or `--raster`, `--no-shadows`;
  `--bloom`, `--motion-blur`, `--vignette`; `--upscaler`, `--quality`,
  `--sharpness`) renders offscreen.
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
