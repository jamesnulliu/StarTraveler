module;

#include <vulkan/vulkan.h>

export module starcore.vulkan:check;

import std;

export namespace starcore::vulkan
{

/** @brief Name of a VkResult code, e.g. "VK_ERROR_DEVICE_LOST". */
[[nodiscard]] auto toString(VkResult result) -> std::string_view;

/**
 * @brief Abort with a log message if `result` is an error (VK_SUBOPTIMAL_KHR
 *        and other positive codes are accepted).
 *
 * GrafixBoard's checkVkResult compiled to nothing because its assert macro
 * was disabled, so Vulkan errors went unnoticed.
 */
void check(VkResult result,
           std::source_location location = std::source_location::current());

}  // namespace starcore::vulkan
