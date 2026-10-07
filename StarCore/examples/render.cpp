// starcore-render: headless renderer. Loads a scene file, path traces it on the
// CPU (plus the 2D overlay) and writes a PNG.
//
//   starcore-render assets/scenes/legacy_demo.yaml -o renders/demo.png -f 64

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include <CLI/CLI.hpp>

#include <starcore/common/log.hpp>
#include <starcore/render/camera.hpp>
#include <starcore/render/overlay.hpp>
#include <starcore/render/path_tracer.hpp>
#include <starcore/scene/scene.hpp>
#include <starcore/scene/serializer.hpp>

auto main(int argc, char** argv) -> int
{
    CLI::App app{"Render a StarCore scene to PNG on the CPU"};

    std::filesystem::path scenePath;
    std::filesystem::path outputPath = "renders/output.png";
    std::uint32_t width = 960;
    std::uint32_t height = 540;
    std::uint32_t frames = 32;
    unsigned threads = 0;
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
    app.add_option("-j,--threads", threads, "Worker threads (0 = all)");
    app.add_option("--camera", cameraPosition, "Camera position x y z")->expected(3);
    app.add_option("--look-at", cameraLookAt, "Camera target x y z")->expected(3);
    CLI11_PARSE(app, argc, argv);

    starcore::Scene scene;
    if (auto loaded = starcore::loadScene(scenePath, scene); !loaded) {
        SC_ERROR("{}", loaded.error());
        return EXIT_FAILURE;
    }

    starcore::Camera camera;
    camera.setViewport(width, height);
    camera.setPosition({cameraPosition[0], cameraPosition[1], cameraPosition[2]});
    if (cameraLookAt.size() == 3) {
        camera.lookAt({cameraLookAt[0], cameraLookAt[1], cameraLookAt[2]});
    }

    const auto renderScene = starcore::buildRenderScene(scene);
    starcore::CpuPathTracer tracer(threads);
    tracer.resize(width, height);

    SC_INFO("Rendering {} ({} instances) at {}x{}, {} frames x {} spp on {} threads",
            scenePath.string(), renderScene.instances.size(), width, height,
            frames, settings.samplesPerPixel, tracer.threadCount());
    const auto start = std::chrono::steady_clock::now();
    for (std::uint32_t i = 0; i < frames; ++i) {
        tracer.render(renderScene, camera, settings);
    }
    const std::chrono::duration<double> elapsed =
        std::chrono::steady_clock::now() - start;

    starcore::ColorImage image = tracer.image();
    starcore::renderOverlay(scene, camera, image);

    if (auto written = starcore::writePng(image, outputPath); !written) {
        SC_ERROR("{}", written.error());
        return EXIT_FAILURE;
    }
    SC_INFO("Wrote {} in {:.2f} s ({:.1f} ms/frame)", outputPath.string(),
            elapsed.count(), elapsed.count() * 1000.0 / frames);
    return EXIT_SUCCESS;
}
