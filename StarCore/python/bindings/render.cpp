module;

#include <nanobind/nanobind.h>
#include <nanobind/ndarray.h>
#include <nanobind/stl/filesystem.h>
#include <nanobind/stl/optional.h>
#include <nanobind/stl/pair.h>
#include <entt/entity/registry.hpp>

#include "casters.hpp"

module starcore.python;

import std;
import starcore.common;
import starcore.math;
import starcore.render;
import starcore.scene;

using namespace nb::literals;

namespace starcore::python
{

namespace
{

void bindCamera(nb::module_& m)
{
    nb::class_<Camera>(m, "Camera",
                       "Perspective camera (+Y up, yaw/pitch in degrees). Pixel space has its "
                       "origin at the top-left corner and +y down.")
        .def(nb::init<float, float, float>(), "vertical_fov"_a = 45.0F, "near"_a = 0.1F,
             "far"_a = 1000.0F)
        .def("set_viewport", &Camera::setViewport, "width"_a, "height"_a)
        .def_prop_ro("viewport_size", &Camera::viewportSize)
        .def_prop_rw("position", &Camera::position, &Camera::setPosition)
        .def("set_rotation", &Camera::setRotation, "yaw"_a, "pitch"_a,
             "Yaw/pitch in degrees (pitch clamped to [-89, 89]).")
        .def("look_at", &Camera::lookAt, "target"_a)
        .def_prop_ro("yaw", &Camera::yaw)
        .def_prop_ro("pitch", &Camera::pitch)
        .def_prop_rw("vertical_fov", &Camera::verticalFov, &Camera::setVerticalFov)
        .def_prop_ro("near", &Camera::nearClip)
        .def_prop_ro("far", &Camera::farClip)
        .def_prop_ro("forward", &Camera::forward)
        .def_prop_ro("right", &Camera::right)
        .def_prop_ro("up", &Camera::up)
        // Fresh arrays: `move` instead of the default reference_internal policy.
        .def_prop_ro(
            "view", [](const Camera& c) { return toArray(c.view()); }, nb::rv_policy::move)
        .def_prop_ro(
            "projection", [](const Camera& c) { return toArray(c.projection()); },
            nb::rv_policy::move)
        .def("generate_ray", &Camera::generateRay, "pixel"_a,
             "Primary ray through a continuous pixel position.")
        .def("world_to_screen", &Camera::worldToScreen, "point"_a,
             "Pixel position, or None behind the near plane.")
        .def("screen_to_world", &Camera::screenToWorld, "pixel"_a, "depth"_a,
             "World point under a pixel at a depth along `forward`.")
        .def("depth_of", &Camera::depthOf, "point"_a);
}

void bindTracer(nb::module_& m)
{
    nb::class_<RenderScene>(m, "RenderScene",
                            "Flattened, render-ready snapshot of a Scene (instances, "
                            "geometries, materials).")
        .def_prop_ro("instance_count", [](const RenderScene& s) { return s.instances.size(); })
        .def_prop_ro("geometry_count", [](const RenderScene& s) { return s.geometries.size(); })
        .def_prop_ro("light_count", [](const RenderScene& s) { return s.lights.size(); },
                     "Emissive spheres sampled directly by the path tracer.")
        .def_prop_ro("triangle_count", &RenderScene::triangleCount,
                     "Triangles of the distinct meshes (BLAS), not counting instancing.")
        .def_ro("sky_color", &RenderScene::skyColor)
        .def_ro("version", &RenderScene::version,
                "Stamp of the last change of anything traced (equal stamps: equal data).")
        .def_ro("content_version", &RenderScene::contentVersion,
                "Stamp of the last change of anything but the instances' transforms.")
        .def(
            "instance_entity",
            [](const RenderScene& s, std::size_t index) {
                if (index >= s.instances.size()) {
                    throw nb::index_error();
                }
                return s.instances[index].entityId;
            },
            "index"_a, "Entity handle of an instance.");

    m.def("build_render_scene", &buildRenderScene, "scene"_a);

    nb::class_<SurfaceHit>(m, "SurfaceHit")
        .def_ro("t", &SurfaceHit::t)
        .def_ro("position", &SurfaceHit::position)
        .def_ro("normal", &SurfaceHit::normal,
                "Shading normal (interpolated), unit length, facing the incoming ray.")
        .def_ro("geometric_normal", &SurfaceHit::geometricNormal,
                "Face normal, unit length, facing the incoming ray.")
        .def_ro("front_face", &SurfaceHit::frontFace)
        .def_ro("instance_index", &SurfaceHit::instanceIndex);

    m.attr("ALL_INSTANCES") = kAllInstances;
    m.def("trace_closest_hit", &traceClosestHit, "render_scene"_a, "ray"_a, "t_min"_a = 0.0F,
          "t_max"_a = kInfinity, "mask"_a = kAllInstances,
          "Closest hit in (t_min, t_max) among instances whose mask intersects `mask`, or None.");
    m.def(
        "trace_path",
        [](const RenderScene& scene, const Ray& ray, const PathTracerSettings& settings,
           std::uint32_t seed) {
            Rng rng(seed);
            return tracePath(scene, ray, settings, rng);
        },
        "render_scene"_a, "ray"_a, "settings"_a, "seed"_a = 0U,
        "Linear radiance of one path sample (for estimator tests).");
    m.def(
        "shade_preview",
        [](const RenderScene& scene, const Ray& ray, const PathTracerSettings& settings) {
            return shadePreview(scene, ray, settings);
        },
        "render_scene"_a, "ray"_a, "settings"_a, "Preview-mode shading of one ray.");
    m.def("trace_any_hit", &traceAnyHit, "render_scene"_a, "ray"_a, "t_min"_a = 0.0F,
          "t_max"_a = kInfinity, "mask"_a = kAllInstances,
          "Whether anything is hit in (t_min, t_max) (shadow rays).");

    nb::enum_<RenderMode>(m, "RenderMode")
        .value("PATH_TRACED", RenderMode::PathTraced)
        .value("PREVIEW", RenderMode::Preview);

    {
        const PathTracerSettings d{};
        nb::class_<PathTracerSettings>(m, "PathTracerSettings")
            .def(
                "__init__",
                [](PathTracerSettings* self, RenderMode mode, std::uint32_t maxBounces,
                   std::uint32_t samplesPerPixel, bool accumulate, bool nextEventEstimation,
                   std::uint32_t seed, float exposure, ToneMapper toneMapper, bool srgbOutput,
                   std::uint32_t instanceMask, float fireflyClamp) {
                    new (self) PathTracerSettings{mode,       maxBounces,   samplesPerPixel,
                                                  accumulate, nextEventEstimation,
                                                  seed,       exposure,     toneMapper,
                                                  srgbOutput, instanceMask, fireflyClamp};
                },
                nb::kw_only(), "mode"_a = d.mode, "max_bounces"_a = d.maxBounces,
                "samples_per_pixel"_a = d.samplesPerPixel, "accumulate"_a = d.accumulate,
                "next_event_estimation"_a = d.nextEventEstimation, "seed"_a = d.seed,
                "exposure"_a = d.exposure, "tone_mapper"_a = d.toneMapper,
                "srgb_output"_a = d.srgbOutput, "instance_mask"_a = d.instanceMask,
                "firefly_clamp"_a = d.fireflyClamp)
            .def_rw("firefly_clamp", &PathTracerSettings::fireflyClamp,
                    "Largest indirect radiance per sample (0 = off; biased but no fireflies).")
            .def_rw("mode", &PathTracerSettings::mode)
            .def_rw("next_event_estimation", &PathTracerSettings::nextEventEstimation,
                    "Sample sphere lights directly (off: BSDF sampling only).")
            .def_rw("instance_mask", &PathTracerSettings::instanceMask,
                    "Only instances whose mask intersects it are visible (layers).")
            .def_rw("max_bounces", &PathTracerSettings::maxBounces)
            .def_rw("samples_per_pixel", &PathTracerSettings::samplesPerPixel)
            .def_rw("accumulate", &PathTracerSettings::accumulate)
            .def_rw("seed", &PathTracerSettings::seed)
            .def_rw("exposure", &PathTracerSettings::exposure)
            .def_rw("tone_mapper", &PathTracerSettings::toneMapper)
            .def_rw("srgb_output", &PathTracerSettings::srgbOutput);
    }

    nb::class_<CpuPathTracer>(m, "CpuPathTracer",
                              "Progressive multi-threaded CPU path tracer.")
        .def(nb::init<unsigned>(), "threads"_a = 0U, "threads=0 uses all hardware threads.")
        .def("resize", &CpuPathTracer::resize, "width"_a, "height"_a)
        .def("reset_accumulation", &CpuPathTracer::resetAccumulation)
        .def(
            "render",
            [](CpuPathTracer& tracer, const RenderScene& scene, const Camera& camera,
               const PathTracerSettings& settings, float timeBudgetMs,
               std::uint32_t firstPassStride) {
                tracer.render(scene, camera, settings,
                              {.timeBudgetMs = timeBudgetMs, .firstPassStride = firstPassStride});
            },
            "render_scene"_a, "camera"_a, "settings"_a, nb::kw_only(), "time_budget_ms"_a = 0.0F,
            "first_pass_stride"_a = 1U, nb::call_guard<nb::gil_scoped_release>(),
            "Add one pass of samples (restarts automatically when the scene version, camera, "
            "settings or size change). With time_budget_ms, render tiles until the time is up "
            "(passes continue across calls); first_pass_stride makes passes after a reset "
            "start coarse (one sample per block) and sharpen.")
        .def_prop_ro("pass_stride", &CpuPathTracer::passStride,
                     "Block size of the pass in progress (1 = full resolution).")
        .def(
            "image", [](const CpuPathTracer& t) { return toArray(t.image()); },
            "Tone-mapped result as a uint8 (height, width, 4) array.")
        .def(
            "entity_ids", [](const CpuPathTracer& t) { return toIdArray(t.entityIds()); },
            "Entity handle of the primary hit per pixel (0xFFFFFFFF = sky).")
        .def_prop_ro("accumulated_frames", &CpuPathTracer::accumulatedFrames)
        .def_prop_ro("thread_count", &CpuPathTracer::threadCount);
}

void bindOverlay(nb::module_& m)
{
    m.def(
        "render_overlay",
        [](const Scene& scene, const Camera& camera, std::optional<RgbaInput> background,
           std::uint32_t visibleLayers, std::uint32_t pickableLayers) {
            ColorImage color;
            if (background) {
                color = fromArray(*background);
            } else {
                const UVec2 size = camera.viewportSize();
                color.resize(size.x, size.y, 0U);
            }
            IdImage ids(color.width(), color.height(), kNoEntityId);
            renderOverlay(scene, camera, color, &ids, visibleLayers, pickableLayers);
            return std::pair(toArray(color), toIdArray(ids));
        },
        "scene"_a, "camera"_a, "background"_a = nb::none(), "visible_layers"_a = kAllLayers,
        "pickable_layers"_a = kAllLayers,
        "Rasterize the 2D shapes of a scene over `background` (or a transparent image of "
        "the camera's viewport size). Returns (rgba, entity_ids).");

    m.def(
        "pick_entity",
        [](const RenderScene& renderScene, const Camera& camera,
           std::optional<IdInput> overlayIds, Vec2 pixel,
           std::uint32_t mask) -> std::optional<std::uint32_t> {
            std::optional<IdImage> ids;
            if (overlayIds) {
                ids = fromArray(*overlayIds);
            }
            const auto picked =
                pickEntity(renderScene, camera, ids ? &*ids : nullptr, pixel, mask);
            if (!picked) {
                return std::nullopt;
            }
            return entt::to_integral(*picked);
        },
        "render_scene"_a, "camera"_a, "overlay_ids"_a.none(), "pixel"_a,
        "mask"_a = kAllInstances,
        "Entity handle under a pixel (overlay shapes first, then ray-traced geometry).");

    m.def(
        "write_png",
        [](const RgbaInput& rgba, const std::filesystem::path& path) {
            if (auto written = writePng(fromArray(rgba), path); !written) {
                throw std::runtime_error(written.error());
            }
        },
        "rgba"_a, "path"_a);
}

}  // namespace

void bindRender(nb::module_& m)
{
    bindCamera(m);
    bindTracer(m);
    bindOverlay(m);
}

}  // namespace starcore::python
