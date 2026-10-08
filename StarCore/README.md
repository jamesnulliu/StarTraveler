# StarCore

A small render engine with two frontends: C++ (`StarCore::Core`,
`StarCore::Gpu`, `StarCore::Runtime`) and Python (the `starcore` package).

- `StarCore::Core` -- headless: math, BVH, mesh/model assets (glTF), ECS scene
  and YAML serialization, CPU path tracer, 2D raster algorithms, collision
  detection.
- `StarCore::Gpu` -- headless Vulkan (context, buffers, compute pipelines) and
  `GpuPathTracer`: the CPU path tracer ported to a compute shader with ray
  queries (`shaders/`), on BLAS/TLAS acceleration structures.
- `StarCore::Runtime` -- GLFW window, Dear ImGui, textures, application and
  layers.
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

```cpp
auto context = starcore::vulkan::Context::create({});  // Headless.
auto tracer = starcore::GpuPathTracer::create(**context);  // Error without ray queries.
(*tracer)->resize(960, 540);
const starcore::RenderScene renderScene = starcore::buildRenderScene(scene);
for (int i = 0; i < 256; ++i) {
    (*tracer)->render(renderScene, camera, {});
}
starcore::writePng((*tracer)->readImage(), "out.png");
```
