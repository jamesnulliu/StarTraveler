#include <cstdlib>
#include <exception>
#include <filesystem>
#include <string>
#include <vector>

#include <CLI/CLI.hpp>

#include <starcore/app/application.hpp>
#include <starcore/common/log.hpp>
#include <starcore/common/version.hpp>

#include "game_layer.hpp"

namespace
{

/**
 * @brief The game's asset directory: next to the executable (the build copies
 *        it there), in the working directory, or in the source tree.
 */
auto findAssetRoot(const char* argv0) -> std::filesystem::path
{
    std::vector<std::filesystem::path> candidates{
        std::filesystem::absolute(argv0).parent_path() / "assets",
        std::filesystem::current_path() / "assets",
#ifdef STARTRAVELER_SOURCE_ASSET_DIR
        std::filesystem::path(STARTRAVELER_SOURCE_ASSET_DIR),
#endif
    };
    for (const auto& candidate : candidates) {
        std::error_code error;
        if (std::filesystem::exists(candidate / "models" / "cockpit.glb", error)) {
            return candidate;
        }
    }
    return candidates.front();
}

}  // namespace

auto main(int argc, char** argv) -> int
{
    CLI::App cli{"StarTraveler - fly through a path-traced universe and draw in it"};
    std::filesystem::path scenePath;
    bool validation = false;
    bool noVsync = false;
    bool noViewports = false;
    startraveler::GameOptions options;
    cli.add_option("scene", scenePath, "Scene file to open (.yaml)")->check(CLI::ExistingFile);
    cli.add_flag("--cpu", options.cpuRenderer,
                 "Path trace on the CPU (default: on the GPU if it supports ray queries)");
    cli.add_flag("--raster", options.raster,
                 "Start with ray tracing off: rasterize on the GPU (no rays, no shadows)");
    cli.add_flag("--cruise", options.cruise,
                 "Fly forward at full thrust (smoke tests, screenshots of the effects)");
    cli.add_flag("--validation", validation,
                 "Enable Vulkan validation layers (configure with STARCORE_VULKAN_VALIDATION=ON)");
    cli.add_flag("--no-vsync", noVsync, "Present without vsync");
    cli.add_flag("--no-viewports", noViewports, "Keep all panels inside the main window");
    cli.add_option("--frames", options.exitAfterFrames,
                   "Exit after this many frames (smoke tests)");
    cli.add_option("--screenshot", options.screenshotPath,
                   "With --frames: save the viewport image as PNG before exiting");
    std::string view = "Final";
    cli.add_option("--view", view,
                   "Hybrid pipeline: Final, Noisy, Albedo, Lighting, Specular, Glass, Variance, "
                   "Motion, History");
    CLI11_PARSE(cli, argc, argv);

    starcore::log::init({.level = spdlog::level::debug});
    SC_INFO("StarTraveler (StarCore {})", starcore::kVersionString);
    if (!starcore::enumFromString(view, options.view)) {
        SC_CRITICAL("Unknown view \"{}\"", view);
        starcore::log::shutdown();
        return EXIT_FAILURE;
    }
    const std::filesystem::path assetRoot = findAssetRoot(argv[0]);
    if (scenePath.empty()) {
        scenePath = assetRoot / "scenes" / "legacy_demo.yaml";
    }

    int exitCode = EXIT_SUCCESS;
    try {
        starcore::Application application({
            .name = "StarTraveler",
            .vsync = !noVsync,
            .enableValidation = validation,
            .enableViewports = !noViewports,
        });
        auto& game =
            application.pushLayer<startraveler::GameLayer>(scenePath, assetRoot, options);
        application.setMenubarCallback([&game] { game.drawMenuBar(); });
        application.run();
    } catch (const std::exception& error) {
        SC_CRITICAL("Fatal: {}", error.what());
        exitCode = EXIT_FAILURE;
    }
    starcore::log::shutdown();
    return exitCode;
}
