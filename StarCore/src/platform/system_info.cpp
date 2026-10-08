#include <format>

#include <GLFW/glfw3.h>
#include <VkBootstrap.h>
#include <imgui.h>
#include <vulkan/vulkan.h>

#include "starcore/platform/system_info.hpp"

namespace starcore::platform
{

auto querySystemInfo() -> SystemInfo
{
    SystemInfo info{};
    info.glfwVersion = glfwGetVersionString();
    info.imguiVersion = IMGUI_VERSION;
    info.vulkanHeaderVersion = VK_HEADER_VERSION_COMPLETE;

    if (auto vkbInfo = vkb::SystemInfo::get_system_info()) {
        info.validationLayersAvailable =
            vkbInfo->validation_layers_available;
        std::uint32_t version = 0;
        if (vkEnumerateInstanceVersion(&version) == VK_SUCCESS) {
            info.vulkanInstanceVersion = version;
        }
    }
    return info;
}

auto formatVulkanVersion(std::uint32_t version) -> std::string
{
    return std::format("{}.{}.{}", VK_API_VERSION_MAJOR(version),
                       VK_API_VERSION_MINOR(version),
                       VK_API_VERSION_PATCH(version));
}

}  // namespace starcore::platform
