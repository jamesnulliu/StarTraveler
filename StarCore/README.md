# StarCore

A small render engine with two frontends: C++ (`StarCore::Core`,
`StarCore::Gpu`, `StarCore::Runtime`) and Python (the `starcore` package).

- `StarCore::Core` -- headless: math, BVH, mesh/model assets (glTF), ECS scene
  and YAML serialization, CPU path tracer, 2D raster algorithms, collision
  detection.
- `StarCore::Gpu` -- headless Vulkan (context, buffers, compute and graphics
  pipelines) and `GpuRenderer`: the CPU path tracer ported to compute shaders
  with ray queries (`shaders/`), on BLAS/TLAS acceleration structures; plain
  progressive path tracing, the hybrid pipeline (rasterized first hits,
  reprojection across frames, SVGF denoising) for real time, or pure
  rasterization (no ray tracing hardware needed).
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
auto renderer = starcore::GpuRenderer::create(**context);
(*renderer)->resize(960, 540);
const starcore::RenderScene renderScene = starcore::buildRenderScene(scene);
for (int i = 0; i < 256; ++i) {
    (*renderer)->render(renderScene, camera, {});  // Path tracing: needs rayTracing().
}
starcore::writePng((*renderer)->readImage(), "out.png");

// Real time: the camera and objects may move every frame. The builder keeps
// the render scene up to date; only changed instances are uploaded.
starcore::RenderSceneBuilder builder;
const starcore::GpuRenderOptions hybrid{.pipeline = starcore::GpuPipeline::Hybrid};
(*renderer)->render(builder.update(scene), camera, {.samplesPerPixel = 2}, hybrid);
```
