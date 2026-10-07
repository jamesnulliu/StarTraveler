#include <cstdlib>
#include <exception>
#include <filesystem>
#include <vector>

#include <CLI/CLI.hpp>

#include <starcore/app/application.hpp>
#include <starcore/common/log.hpp>
#include <starcore/common/version.hpp>

#include "game_layer.hpp"

namespace
{

/**
 * @brief Default scene: next to the executable (assets are copied there by
 *        the build), in the working directory, or in the source tree.
 */
auto findDefaultScene(const char* argv0) -> std::filesystem::path
{
    const std::filesystem::path relative = "assets/scenes/legacy_demo.yaml";
    std::vector<std::filesystem::path> candidates{
        std::filesystem::absolute(argv0).parent_path() / relative,
        std::filesystem::current_path() / relative,
#ifdef STARTRAVELER_SOURCE_ASSET_DIR
        std::filesystem::path(STARTRAVELER_SOURCE_ASSET_DIR) / "scenes/legacy_demo.yaml",
#endif
    };
    for (const auto& candidate : candidates) {
        std::error_code error;
        if (std::filesystem::exists(candidate, error)) {
            return candidate;
        }
    }
    return {};
}

}  // namespace

auto main(int argc, char** argv) -> int
{
    CLI::App cli{"StarTraveler - fly through a path-traced universe and draw in it"};
    std::filesystem::path scenePath;
    bool validation = false;
    bool noVsync = false;
    bool noViewports = false;
    startraveler::AutomationOptions automation;
    cli.add_option("scene", scenePath, "Scene file to open (.yaml)")->check(CLI::ExistingFile);
    cli.add_flag("--validation", validation, "Enable Vulkan validation layers");
    cli.add_flag("--no-vsync", noVsync, "Present without vsync");
    cli.add_flag("--no-viewports", noViewports, "Keep all panels inside the main window");
    cli.add_option("--frames", automation.exitAfterFrames,
                   "Exit after this many frames (smoke tests)");
    cli.add_option("--screenshot", automation.screenshotPath,
                   "With --frames: save the viewport image as PNG before exiting");
    CLI11_PARSE(cli, argc, argv);

    starcore::log::init({.level = spdlog::level::debug});
    SC_INFO("StarTraveler (StarCore {})", starcore::kVersionString);
    if (scenePath.empty()) {
        scenePath = findDefaultScene(argv[0]);
    }

    int exitCode = EXIT_SUCCESS;
    try {
        starcore::Application application({
            .name = "StarTraveler",
            .vsync = !noVsync,
            .enableValidation = validation,
            .enableViewports = !noViewports,
        });
        auto& game = application.pushLayer<startraveler::GameLayer>(scenePath, automation);
        application.setMenubarCallback([&game] { game.drawMenuBar(); });
        application.run();
    } catch (const std::exception& error) {
        SC_CRITICAL("Fatal: {}", error.what());
        exitCode = EXIT_FAILURE;
    }
    starcore::log::shutdown();
    return exitCode;
}
