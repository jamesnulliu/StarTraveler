# StarCore

A small render engine with two frontends: C++ (`StarCore::Core`,
`StarCore::Runtime`) and Python (the `starcore` package).

- `StarCore::Core` -- headless: math, BVH, mesh/model assets (glTF), ECS scene
  and YAML serialization, CPU path tracer, 2D raster algorithms, collision
  detection.
- `StarCore::Runtime` -- GLFW window, Vulkan context, Dear ImGui, application
  and layers.
- `starcore` -- nanobind bindings of `StarCore::Core`; images are numpy arrays.

This directory builds on its own (`cmake -S StarCore -B build`, or
`pip install StarCore/`); vcpkg is found through `VCPKG_ROOT`. See the
repository's README for presets, the game built on it and examples.

```python
import starcore as sc

scene = sc.Scene()
model = sc.load_model("ship.glb")
ship = scene.create_entity("Ship")
scene.instantiate(model, "ship.glb", ship)

world = sc.PhysicsWorld.build(scene)
hit = world.raycast(sc.Ray((0, 10, 0), (0, -1, 0)))
```
