#include <starcore/common/log.hpp>
#include <starcore/common/version.hpp>
#include <starcore/platform/system_info.hpp>

auto main() -> int
{
    starcore::log::init({.level = spdlog::level::debug});

    const auto info = starcore::platform::querySystemInfo();
    SC_INFO("StarTraveler (StarCore {})", starcore::kVersionString);
    SC_INFO("GLFW {} | Dear ImGui {} | Vulkan headers {}", info.glfwVersion,
            info.imguiVersion,
            starcore::platform::formatVulkanVersion(info.vulkanHeaderVersion));
    if (info.vulkanInstanceVersion.has_value()) {
        SC_INFO("Vulkan loader: {} (validation layers: {})",
                starcore::platform::formatVulkanVersion(
                    *info.vulkanInstanceVersion),
                info.validationLayersAvailable ? "available" : "not found");
    } else {
        SC_ERROR("No Vulkan loader/driver found.");
    }

    starcore::log::shutdown();
    return 0;
}
