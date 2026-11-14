# Engine

StarCore has two frontends, C++ and Python:

- `StarCore::Core`: headless; math, BVH, mesh and model assets (glTF), the ECS scene and YAML
  serialization, the CPU path tracer, the particle system, the 2D raster algorithms (the C++20
  module `starcore.raster`) and collision detection.
- `StarCore::Gpu`: headless Vulkan and the GPU renderer ([GPU rendering](gpu-rendering.md)).
- `StarCore::Runtime`: the window, Dear ImGui, textures, the application and its layers.
- `starcore`: Python bindings (nanobind) of `StarCore::Core`; images are numpy arrays.

`StarCore/` builds on its own (`cmake -S StarCore -B build`, or `pip install StarCore/`), and
[StarCore/README.md](../StarCore/README.md) has short C++ examples.

## Scene

An ECS (EnTT): entities with plain-data components -- transform, hierarchy, layer, material,
sphere, mesh, collider, 2D shapes, portal. Scenes are YAML files; meshes from models are saved as
references to their glTF file.

## Assets

Immutable shared `MeshData` (with its BVH), procedural primitives, glTF 2.0 loading (fastgltf) and
`.glb` writing, and an `AssetLibrary` cache.

## Render scene and CPU path tracer

The render scene is shaped like Vulkan ray tracing input: a BLAS per mesh, a TLAS over instances,
8-bit instance masks. A `RenderSceneBuilder` keeps it up to date incrementally: every entity keeps
its instance slot and each change is stamped, so only what changed is rewritten and uploaded.

The CPU path tracer is progressive and multi-threaded, with a GGX metal/dielectric BSDF, thin and
refracting glass, next event estimation of sphere lights with MIS, a preview mode, coarse-to-fine
passes and a per-frame time budget. The GPU's path tracer is a port of it
([GPU rendering](gpu-rendering.md#path-tracing)).

## Particles

A CPU particle system: emitter components spread their particles along the path they moved, and
bursts. The GPU renderer draws them.

## 2D raster

Bresenham, midpoint and DDA lines, midpoint ellipses, Bezier and B-spline curves,
Cohen-Sutherland, midpoint-subdivision and Sutherland-Hodgman clipping, scan-line and flood fill,
drawn as a world-space overlay.

## Physics

Sphere, box and mesh colliders; raycasts, continuous sphere casts, overlaps and collide-and-slide,
filtered by the layer collision matrix.

## Runtime

A GLFW window, Vulkan (vk-bootstrap, VMA), Dear ImGui docking with multi-viewports, and an
application/layer framework.

## Python

The `starcore` package wraps `StarCore::Core` (building it: [Building](building.md#python)):

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

The game's ship model and default scene are generated with it ([Models and scenes](assets.md)).

Planned: Python bindings of the GPU renderer and the runtime, for games written in Python.
