// starcore.gpu -- the GPU renderer:
//
//   :gpu_scene     a RenderScene on the GPU: buffers updated per changed
//                  instance, BLAS/TLAS with ray queries
//   :gpu_renderer  GpuRenderer: path tracing, hybrid and raster pipelines,
//                  particles and image effects
//
// The pipelines' internals are internal partitions (src/gpu/*.cppm:
// :rasterization, :hybrid_renderer, :raster_renderer, :post_process), which
// only the module's own units import.

export module starcore.gpu;

export import :gpu_renderer;
export import :gpu_scene;
