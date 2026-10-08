// starcore.render -- rendering on the CPU and what all renderers share:
//
//   :camera        perspective camera, rays through pixels, projection
//   :render_scene  the flattened, render-ready scene (instances, materials,
//                  lights, TLAS) and the builder that keeps it up to date
//   :sampling      GGX, Fresnel, cone sampling, MIS (the path tracers' helpers)
//   :path_tracer   the progressive, multi-threaded CPU path tracer
//   :particles     the particle system (drawn by the GPU renderer)
//   :overlay       the 2D overlay shapes, rasterized over an image; picking

export module starcore.render;

export import :camera;
export import :overlay;
export import :particles;
export import :path_tracer;
export import :render_scene;
export import :sampling;
