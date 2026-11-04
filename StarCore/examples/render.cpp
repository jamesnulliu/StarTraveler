// starcore-render: headless renderer. Loads a scene file, path traces it on the
// CPU or the GPU (plus the 2D overlay) and writes a PNG.
//
//   starcore-render assets/scenes/legacy_demo.yaml -o renders/demo.png -f 64
//   starcore-render assets/scenes/legacy_demo.yaml -o renders/demo.png -f 1024 --gpu
//   starcore-render assets/scenes/legacy_demo.yaml -o renders/demo.png -f 30 --hybrid
//       --move 0 0 -0.05   (the hybrid pipeline, the camera moving every frame)
//   starcore-render assets/scenes/legacy_demo.yaml -o renders/demo.png -f 30 --hybrid
//       --move-entity "Red Sun" 0.05 0 0   (an entity moving every frame)
//   starcore-render scene.yaml -o renders/turn.png -f 2108 --gpu --turn-entity Ship 1
//       --still 2048   (a ship turning with the camera, then the reference at its end pose)
//   starcore-render assets/scenes/legacy_demo.yaml -o renders/demo.png -f 16 --raster

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <optional>
#include <string>
#include <vector>

#include <CLI/CLI.hpp>

#include <starcore/asset/model.hpp>
#include <starcore/common/enum_names.hpp>
#include <starcore/common/log.hpp>
#include <starcore/render/camera.hpp>
#include <starcore/render/overlay.hpp>
#include <starcore/render/path_tracer.hpp>
#include <starcore/scene/components.hpp>
#include <starcore/scene/entity.hpp>
#include <starcore/scene/scene.hpp>
#include <starcore/scene/serializer.hpp>

#ifdef STARCORE_RENDER_GPU
#include <starcore/gpu/gpu_renderer.hpp>
#include <starcore/vulkan/context.hpp>
#endif

namespace
{

struct GpuOptions
{
    bool hybrid = false;
    bool raster = false;
    starcore::ShadowSettings shadows;  // Raster.
    std::string view = "Final";
    bool spatial = true;
    bool temporal = true;
    std::uint32_t filterPasses = starcore::DenoiserSettings{}.iterations;
    bool half = false;  // TraceResolution::Half
    bool shadowMaps = false;  // DirectShadows::ShadowMaps
    bool radianceCache = false;
    starcore::Vec3 move{0.0F};  // Camera translation per frame.
    starcore::Entity movedEntity;
    starcore::Vec3 entityMove{0.0F};  // Its translation per frame.
    // Turned about its origin (yaw, degrees per frame), the camera with it (a pilot steering).
    starcore::Entity turnedEntity;
    float entityTurn = 0.0F;
    std::uint32_t still = 0;  // The last frames: nothing moves.
    std::filesystem::path dumpPath;  // Every dumpEvery frames: the image (name_frame.png).
    std::uint32_t dumpEvery = 0;
    bool validation = false;
    // Effects (EffectSettings).
    bool bloom = false;
    bool motionBlur = false;
    float vignette = 0.0F;
    // Upscaling (UpscalerSettings).
    std::string upscaler = "Off";
    std::string quality = "Quality";
    float sharpness = 0.0F;
};

/** @brief Render `frames` frames on the GPU; nullopt (logged) if it cannot. */
auto renderOnGpu([[maybe_unused]] starcore::Scene& scene,
                 [[maybe_unused]] starcore::Camera camera,
                 [[maybe_unused]] const starcore::PathTracerSettings& settings,
                 [[maybe_unused]] std::uint32_t frames,
                 [[maybe_unused]] const GpuOptions& gpuOptions)
    -> std::optional<starcore::ColorImage>
{
#ifdef STARCORE_RENDER_GPU
    starcore::GpuRenderOptions options;
    if (gpuOptions.hybrid) {
        options.pipeline = starcore::GpuPipeline::Hybrid;
        options.denoiser.spatial = gpuOptions.spatial;
        options.denoiser.temporal = gpuOptions.temporal;
        options.denoiser.iterations = gpuOptions.filterPasses;
        options.traceResolution =
            gpuOptions.half ? starcore::TraceResolution::Half : starcore::TraceResolution::Full;
        options.directShadows = gpuOptions.shadowMaps ? starcore::DirectShadows::ShadowMaps
                                                      : starcore::DirectShadows::Traced;
        options.radianceCache = gpuOptions.radianceCache;
        if (!starcore::enumFromString(gpuOptions.view, options.view)) {
            SC_ERROR("Unknown view \"{}\"", gpuOptions.view);
            return std::nullopt;
        }
    }
    if (gpuOptions.raster) {
        options.pipeline = starcore::GpuPipeline::Raster;
    }
    options.shadows = gpuOptions.shadows;
    options.effects.bloom = gpuOptions.bloom;
    options.effects.motionBlur = gpuOptions.motionBlur;
    options.effects.vignette = gpuOptions.vignette;
    if (!starcore::enumFromString(gpuOptions.upscaler, options.upscaling.upscaler)) {
        SC_ERROR("Unknown upscaler \"{}\"", gpuOptions.upscaler);
        return std::nullopt;
    }
    if (!starcore::enumFromString(gpuOptions.quality, options.upscaling.quality)) {
        SC_ERROR("Unknown upscaler quality \"{}\"", gpuOptions.quality);
        return std::nullopt;
    }
    options.upscaling.sharpness = gpuOptions.sharpness;
    auto context = starcore::vulkan::Context::create(
        {.applicationName = "starcore-render", .enableValidation = gpuOptions.validation});
    if (!context) {
        SC_ERROR("Vulkan: {}", context.error());
        return std::nullopt;
    }
    auto tracer = starcore::GpuRenderer::create(**context);
    if (!tracer) {
        SC_ERROR("{}", tracer.error());
        return std::nullopt;
    }
    if (starcore::tracesRays(options.pipeline) && !(*tracer)->rayTracing()) {
        SC_ERROR("{} has no ray queries (--raster works)", (*context)->deviceInfo().name);
        return std::nullopt;
    }
    (*tracer)->resize(camera.viewportSize().x, camera.viewportSize().y);
    // Incremental: moving an entity reprojects, as in a game.
    starcore::RenderSceneBuilder builder;
    // The first frame builds the acceleration structures.
    (*tracer)->render(builder.update(scene), camera, settings, options);
    (*tracer)->waitIdle();
    const auto start = std::chrono::steady_clock::now();
    for (std::uint32_t i = 1; i < frames; ++i) {
        if (i + gpuOptions.still < frames) {
            camera.setPosition(camera.position() + gpuOptions.move);
            if (gpuOptions.movedEntity) {
                gpuOptions.movedEntity.patch<starcore::TransformComponent>(
                    [&](starcore::TransformComponent& t) {
                        t.translation += gpuOptions.entityMove;
                    });
            }
            if (gpuOptions.turnedEntity) {
                // Yaw as the game's ships (ShipSystem::orientation): about world +Y,
                // negative angles for a positive yaw.
                const starcore::Quat turn = glm::angleAxis(glm::radians(-gpuOptions.entityTurn),
                                                           starcore::Vec3(0.0F, 1.0F, 0.0F));
                starcore::Vec3 pivot(0.0F);
                gpuOptions.turnedEntity.patch<starcore::TransformComponent>(
                    [&](starcore::TransformComponent& t) {
                        pivot = t.translation;
                        t.rotation = turn * t.rotation;
                    });
                camera.setPosition(pivot + turn * (camera.position() - pivot));
                camera.setRotation(camera.yaw() + gpuOptions.entityTurn, camera.pitch());
            }
        }
        (*tracer)->render(builder.update(scene), camera, settings, options);
        if (gpuOptions.dumpEvery > 0 && (i + 1) % gpuOptions.dumpEvery == 0) {
            starcore::ColorImage frame = (*tracer)->readImage();
            starcore::renderOverlay(scene, camera, frame);
            const std::filesystem::path path =
                gpuOptions.dumpPath.parent_path() /
                std::format("{}_{:04}.png", gpuOptions.dumpPath.stem().string(), i + 1);
            if (auto written = starcore::writePng(frame, path); !written) {
                SC_ERROR("{}", written.error());
            }
        }
    }
    starcore::ColorImage image = (*tracer)->readImage();
    const std::chrono::duration<double, std::milli> elapsed =
        std::chrono::steady_clock::now() - start;
    if (frames > 1) {
        SC_INFO("GPU: {:.2f} ms/frame after setup (last frame: {:.2f} ms, {:.2f} tracing)",
                elapsed.count() / (frames - 1), (*tracer)->gpuTimeMs(),
                (*tracer)->traceTimeMs());
    }
    if (options.upscaling.upscaler != starcore::Upscaler::Off) {
        SC_INFO("Upscaled with {}: {}x{} -> {}x{}",
                starcore::enumToString((*tracer)->activeUpscaler()), (*tracer)->renderWidth(),
                (*tracer)->renderHeight(), (*tracer)->width(), (*tracer)->height());
    }
    return image;
#else
    SC_ERROR("Built without the GPU path tracer (STARCORE_BUILD_GPU=OFF)");
    return std::nullopt;
#endif
}

}  // namespace

auto main(int argc, char** argv) -> int
{
    CLI::App app{"Render a StarCore scene to PNG with the CPU or GPU path tracer"};

    std::filesystem::path scenePath;
    std::filesystem::path outputPath = "renders/output.png";
    std::uint32_t width = 960;
    std::uint32_t height = 540;
    std::uint32_t frames = 32;
    unsigned threads = 0;
    bool gpu = false;
    GpuOptions gpuOptions;
    bool noSpatial = false;
    bool noTemporal = false;
    bool noShadows = false;
    std::vector<float> move;
    std::vector<std::string> moveEntity;
    std::vector<std::string> turnEntity;
    starcore::PathTracerSettings settings;
    std::vector<float> cameraPosition{0.0F, 0.0F, 10.0F};
    std::vector<float> cameraLookAt;

    app.add_option("scene", scenePath, "Scene file (.yaml)")->required()->check(CLI::ExistingFile);
    app.add_option("-o,--output", outputPath, "Output PNG");
    app.add_option("-W,--width", width, "Image width")->check(CLI::Range(1U, 16384U));
    app.add_option("-H,--height", height, "Image height")->check(CLI::Range(1U, 16384U));
    app.add_option("-f,--frames", frames, "Accumulated frames")->check(CLI::Range(1U, 100000U));
    app.add_option("-s,--spp", settings.samplesPerPixel, "Samples per pixel per frame");
    app.add_option("-b,--bounces", settings.maxBounces, "Max path segments");
    app.add_option("--seed", settings.seed, "Random seed");
    app.add_option("--firefly-clamp", settings.fireflyClamp,
                   "Clamp indirect light to this (0 = off; the game uses 8)");
    app.add_option("-j,--threads", threads, "Worker threads (0 = all)");
    app.add_flag("--gpu", gpu, "Path trace on the GPU (Vulkan ray queries)");
    app.add_flag("--hybrid", gpuOptions.hybrid,
                 "GPU, hybrid pipeline: rasterized first hits, reprojection and denoising");
    app.add_flag("--raster", gpuOptions.raster,
                 "GPU, raster pipeline: direct light, shadow maps, no rays");
    app.add_flag("--no-shadows", noShadows, "Raster: no shadow maps");
    app.add_option("--shadow-distance", gpuOptions.shadows.distance,
                   "Raster: how far from the camera far lights cast shadows");
    app.add_option("--shadow-resolution", gpuOptions.shadows.resolution,
                   "Raster: texels per side of a shadow map");
    app.add_option("--view", gpuOptions.view,
                   "Hybrid: Final, Noisy, Albedo, Lighting, Specular, Glass, Variance, Motion, "
                   "History");
    app.add_flag("--no-spatial", noSpatial, "Hybrid: no spatial filter");
    app.add_flag("--no-temporal", noTemporal, "Hybrid: no reprojection");
    app.add_option("--filter-passes", gpuOptions.filterPasses,
                   "Hybrid: a-trous iterations of the spatial filter (radius 2^N pixels)")
        ->check(CLI::Range(1U, 8U));
    app.add_flag("--half", gpuOptions.half,
                 "Hybrid: paths from one pixel of each 2x2 per frame, in turn");
    app.add_flag("--shadow-maps", gpuOptions.shadowMaps,
                 "Hybrid: the sphere lights' direct light shadowed with shadow maps");
    app.add_flag("--cache", gpuOptions.radianceCache,
                 "Hybrid: paths end where the radiance cache knows the light");
    app.add_option("--move", move, "GPU: camera translation per frame x y z")->expected(3);
    app.add_option("--move-entity", moveEntity,
                   "GPU: translation per frame of an entity (by name): name x y z")
        ->expected(4);
    app.add_option("--turn-entity", turnEntity,
                   "GPU: yaw per frame (degrees) of an entity (by name) about its origin, the "
                   "camera turning with it: name degrees")
        ->expected(2);
    app.add_option("--still", gpuOptions.still,
                   "GPU: of the frames, the last this many still (references at the end pose)");
    app.add_option("--dump-every", gpuOptions.dumpEvery,
                   "GPU: also write every this many frames' image (output_frame.png)");
    app.add_flag("--validation", gpuOptions.validation, "GPU: Vulkan validation layers");
    app.add_flag("--bloom", gpuOptions.bloom, "GPU: glow around bright light");
    app.add_flag("--motion-blur", gpuOptions.motionBlur,
                 "GPU (hybrid, raster): blur along the motion, see --move");
    app.add_option("--vignette", gpuOptions.vignette, "GPU: darker corners, 0-1");
    app.add_option("--upscaler", gpuOptions.upscaler,
                   "GPU (hybrid, raster): Off, TAAU, FSR, DLSS (falls back to FSR, then TAAU), "
                   "DLSS-RR (also denoises the hybrid pipeline; falls back to DLSS)");
    app.add_option("--quality", gpuOptions.quality,
                   "Upscaler: Native, Quality, Balanced, Performance, \"Ultra performance\"");
    app.add_option("--sharpness", gpuOptions.sharpness, "Upscaler: FSR's sharpening, 0-1");
    app.add_option("--camera", cameraPosition, "Camera position x y z")->expected(3);
    app.add_option("--look-at", cameraLookAt, "Camera target x y z")->expected(3);
    float verticalFov = 0.0F;
    app.add_option("--fov", verticalFov, "Vertical field of view, degrees (default 45)");
    app.add_option("--exposure", settings.exposure,
                   "Exposure (a factor; with --auto-exposure, a compensation)");
    app.add_flag("--auto-exposure", settings.autoExposure, "Meter the exposure");
    std::string toneMapper = "None";
    app.add_option("--tone-mapper", toneMapper, "None, Reinhard, ACES, AgX");
    CLI11_PARSE(app, argc, argv);
    if (!starcore::enumFromString(toneMapper, settings.toneMapper)) {
        SC_ERROR("Unknown tone mapper \"{}\"", toneMapper);
        return EXIT_FAILURE;
    }
    gpuOptions.spatial = !noSpatial;
    gpuOptions.temporal = !noTemporal;
    gpuOptions.shadows.enabled = !noShadows;
    if (move.size() == 3) {
        gpuOptions.move = {move[0], move[1], move[2]};
    }
    gpu = gpu || gpuOptions.hybrid || gpuOptions.raster;

    starcore::Scene scene;
    starcore::AssetLibrary assets;
    if (auto loaded = starcore::loadScene(scenePath, scene, &assets); !loaded) {
        SC_ERROR("{}", loaded.error());
        return EXIT_FAILURE;
    }
    if (moveEntity.size() == 4) {
        gpuOptions.movedEntity = scene.findEntityByName(moveEntity[0]);
        if (!gpuOptions.movedEntity) {
            SC_ERROR("No entity named \"{}\"", moveEntity[0]);
            return EXIT_FAILURE;
        }
        gpuOptions.entityMove = {std::stof(moveEntity[1]), std::stof(moveEntity[2]),
                                 std::stof(moveEntity[3])};
    }
    if (turnEntity.size() == 2) {
        gpuOptions.turnedEntity = scene.findEntityByName(turnEntity[0]);
        if (!gpuOptions.turnedEntity) {
            SC_ERROR("No entity named \"{}\"", turnEntity[0]);
            return EXIT_FAILURE;
        }
        gpuOptions.entityTurn = std::stof(turnEntity[1]);
    }
    gpuOptions.dumpPath = outputPath;

    starcore::Camera camera;
    camera.setViewport(width, height);
    if (verticalFov > 0.0F) {
        camera.setVerticalFov(verticalFov);
    }
    camera.setPosition({cameraPosition[0], cameraPosition[1], cameraPosition[2]});
    if (cameraLookAt.size() == 3) {
        camera.lookAt({cameraLookAt[0], cameraLookAt[1], cameraLookAt[2]});
    }

    const auto renderScene = starcore::buildRenderScene(scene);
    SC_INFO("Rendering {} ({} instances, {} triangles) at {}x{}, {} frames x {} spp on the {}",
            scenePath.string(), renderScene.instances.size(), renderScene.triangleCount(), width,
            height, frames, settings.samplesPerPixel, gpu ? "GPU" : "CPU");
    const auto start = std::chrono::steady_clock::now();
    starcore::ColorImage image;
    if (gpu) {
        auto rendered = renderOnGpu(scene, camera, settings, frames, gpuOptions);
        if (!rendered) {
            return EXIT_FAILURE;
        }
        image = std::move(*rendered);
    } else {
        starcore::CpuPathTracer tracer(threads);
        tracer.resize(width, height);
        for (std::uint32_t i = 0; i < frames; ++i) {
            tracer.render(renderScene, camera, settings);
        }
        image = tracer.image();
    }
    const std::chrono::duration<double> elapsed = std::chrono::steady_clock::now() - start;

    starcore::renderOverlay(scene, camera, image);

    if (auto written = starcore::writePng(image, outputPath); !written) {
        SC_ERROR("{}", written.error());
        return EXIT_FAILURE;
    }
    SC_INFO("Wrote {} in {:.2f} s ({:.1f} ms/frame)", outputPath.string(),
            elapsed.count(), elapsed.count() * 1000.0 / frames);
    return EXIT_SUCCESS;
}
