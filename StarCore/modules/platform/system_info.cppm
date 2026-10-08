export module starcore.platform:system_info;

import std;

export namespace starcore::platform
{

struct SystemInfo
{
    std::string glfwVersion;
    std::string imguiVersion;
    // VK_HEADER_VERSION_COMPLETE of the headers StarCore was compiled with.
    std::uint32_t vulkanHeaderVersion = 0;
    // Highest instance version supported by the installed loader;
    // nullopt if no Vulkan loader/driver could be found.
    std::optional<std::uint32_t> vulkanInstanceVersion;
    bool validationLayersAvailable = false;
};

/** @brief Query versions of the runtime dependencies and the Vulkan loader. */
[[nodiscard]] auto querySystemInfo() -> SystemInfo;

/** @brief Format a packed Vulkan version as "major.minor.patch". */
[[nodiscard]] auto formatVulkanVersion(std::uint32_t version) -> std::string;

}  // namespace starcore::platform
