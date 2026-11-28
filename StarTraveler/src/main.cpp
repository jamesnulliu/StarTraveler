#include <cmath>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <string>
#include <vector>

#include <CLI/CLI.hpp>
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

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
    cli.add_option("--rehearsal-seconds", options.rehearsalSeconds,
                   "Automatic windowed flight: seconds per phase (0 disables)")
        ->check(CLI::Range(0.0F, 120.0F));
    cli.add_option("--frames", options.exitAfterFrames,
                   "Exit after this many frames (smoke tests)");
    cli.add_option("--demo-preset", options.demoPreset, "Presentation profile 1-7 (0: defaults)")
        ->check(CLI::Range(0, 7));
    cli.add_option("--screenshot", options.screenshotPath,
                   "With --frames: save the viewport image as PNG before exiting");
    std::string view = "Final";
    cli.add_option("--view", view,
                   "Hybrid pipeline: Final, Noisy, Albedo, Lighting, Specular, Glass, Variance, "
                   "Motion, History");
    std::string upscaler = "DLSS";
    cli.add_option("--upscaler", upscaler,
                   "Off, TAAU, FSR, DLSS (default; falls back to FSR, then TAAU), DLSS-RR (also "
                   "denoises the hybrid pipeline; falls back to DLSS)");
    std::string quality = "Quality";
    std::string dlssPreset = "Default";
    cli.add_option("--quality", quality,
                   "Upscaler: Native, Quality, Balanced, Performance, \"Ultra performance\"");
    cli.add_option("--dlss-preset", dlssPreset, "Ordinary DLSS model: Default, K, M");
    cli.add_option("--dlss-exposure", options.upscaling.dlssExposure,
                   "DLSS K: fixed HDR exposure; 0 retains internal auto exposure");
    CLI11_PARSE(cli, argc, argv);

    starcore::log::init({.level = spdlog::level::debug});
    if (options.rehearsalSeconds > 0.0F) {
        starcore::log::client().flush_on(spdlog::level::info);
        starcore::log::core().flush_on(spdlog::level::info);
    }
    SC_INFO("StarTraveler (StarCore {})", starcore::kVersionString);
    if (options.demoPreset > 0 && options.rehearsalSeconds > 0.0F) {
        SC_CRITICAL("Choose either a demo preset or the automatic rehearsal route");
        starcore::log::shutdown();
        return EXIT_FAILURE;
    }
    if (options.demoPreset > 0 && options.cpuRenderer) {
        SC_CRITICAL("GPU demo presets cannot be combined with --cpu");
        starcore::log::shutdown();
        return EXIT_FAILURE;
    }
    if (!starcore::enumFromString(view, options.view)) {
        SC_CRITICAL("Unknown view \"{}\"", view);
        starcore::log::shutdown();
        return EXIT_FAILURE;
    }
    if (!starcore::enumFromString(dlssPreset, options.upscaling.dlssPreset)) {
        SC_CRITICAL("Unknown DLSS model \"{}\"", dlssPreset);
        starcore::log::shutdown();
        return EXIT_FAILURE;
    }
    if (!std::isfinite(options.upscaling.dlssExposure) || options.upscaling.dlssExposure < 0.0F ||
        (options.upscaling.dlssExposure > 0.0F &&
         options.upscaling.dlssPreset != starcore::DlssPreset::K)) {
        SC_CRITICAL("Fixed DLSS exposure requires a finite positive value and explicit K");
        starcore::log::shutdown();
        return EXIT_FAILURE;
    }
    if (!starcore::enumFromString(upscaler, options.upscaling.upscaler) ||
        !starcore::enumFromString(quality, options.upscaling.quality)) {
        SC_CRITICAL("Unknown upscaler \"{}\" or quality \"{}\"", upscaler, quality);
        starcore::log::shutdown();
        return EXIT_FAILURE;
    }
    const std::filesystem::path assetRoot = findAssetRoot(argv[0]);
    if (scenePath.empty()) {
        scenePath = assetRoot / "scenes" / "blue_star.yaml";
    }

    int exitCode = EXIT_SUCCESS;
    try {
        starcore::Application application({
            .name = "StarTraveler",
            .width = options.rehearsalSeconds > 0.0F || options.demoPreset > 0 ? 1280U : 1600U,
            .height = options.rehearsalSeconds > 0.0F || options.demoPreset > 0 ? 720U : 900U,
            .vsync = !noVsync,
            .enableValidation = validation,
            .enableViewports = !noViewports,
        });
        auto& game =
            application.pushLayer<startraveler::GameLayer>(scenePath, assetRoot, options);
        if (options.rehearsalSeconds > 0.0F) {
            // Keep this opt-in recording window unobscured; ordinary play is unchanged.
            glfwSetWindowAttrib(application.window().handle(), GLFW_FLOATING, GLFW_TRUE);
            glfwShowWindow(application.window().handle());
            glfwFocusWindow(application.window().handle());
            int x = 0;
            int y = 0;
            glfwGetWindowPos(application.window().handle(), &x, &y);
            const auto size = application.window().framebufferSize();
            SC_INFO("Rehearsal window: x={} y={} width={} height={}", x, y, size.x, size.y);
        }
        if (options.demoPreset > 0) {
            int x = 0;
            int y = 0;
            glfwGetWindowPos(application.window().handle(), &x, &y);
            const auto size = application.window().framebufferSize();
            SC_INFO("Demo window: x={} y={} width={} height={}", x, y, size.x, size.y);
        }
        application.setMenubarCallback([&game] { game.drawMenuBar(); });
        application.run();
    } catch (const std::exception& error) {
        SC_CRITICAL("Fatal: {}", error.what());
        exitCode = EXIT_FAILURE;
    }
    starcore::log::shutdown();
    return exitCode;
}
