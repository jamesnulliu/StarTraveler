module;

#include <vulkan/vulkan.h>

#include "starcore/common/assert.hpp"

module starcore.vulkan;

import std;

namespace starcore::vulkan
{

auto toString(VkResult result) -> std::string_view
{
    switch (result) {
#define SC_VK_RESULT_CASE(name)                                                \
    case name:                                                                 \
        return #name;
        SC_VK_RESULT_CASE(VK_SUCCESS)
        SC_VK_RESULT_CASE(VK_NOT_READY)
        SC_VK_RESULT_CASE(VK_TIMEOUT)
        SC_VK_RESULT_CASE(VK_EVENT_SET)
        SC_VK_RESULT_CASE(VK_EVENT_RESET)
        SC_VK_RESULT_CASE(VK_INCOMPLETE)
        SC_VK_RESULT_CASE(VK_SUBOPTIMAL_KHR)
        SC_VK_RESULT_CASE(VK_ERROR_OUT_OF_HOST_MEMORY)
        SC_VK_RESULT_CASE(VK_ERROR_OUT_OF_DEVICE_MEMORY)
        SC_VK_RESULT_CASE(VK_ERROR_INITIALIZATION_FAILED)
        SC_VK_RESULT_CASE(VK_ERROR_DEVICE_LOST)
        SC_VK_RESULT_CASE(VK_ERROR_MEMORY_MAP_FAILED)
        SC_VK_RESULT_CASE(VK_ERROR_LAYER_NOT_PRESENT)
        SC_VK_RESULT_CASE(VK_ERROR_EXTENSION_NOT_PRESENT)
        SC_VK_RESULT_CASE(VK_ERROR_FEATURE_NOT_PRESENT)
        SC_VK_RESULT_CASE(VK_ERROR_INCOMPATIBLE_DRIVER)
        SC_VK_RESULT_CASE(VK_ERROR_TOO_MANY_OBJECTS)
        SC_VK_RESULT_CASE(VK_ERROR_FORMAT_NOT_SUPPORTED)
        SC_VK_RESULT_CASE(VK_ERROR_FRAGMENTED_POOL)
        SC_VK_RESULT_CASE(VK_ERROR_OUT_OF_POOL_MEMORY)
        SC_VK_RESULT_CASE(VK_ERROR_SURFACE_LOST_KHR)
        SC_VK_RESULT_CASE(VK_ERROR_NATIVE_WINDOW_IN_USE_KHR)
        SC_VK_RESULT_CASE(VK_ERROR_OUT_OF_DATE_KHR)
        SC_VK_RESULT_CASE(VK_ERROR_VALIDATION_FAILED_EXT)
#undef SC_VK_RESULT_CASE
    default:
        return "VK_RESULT_UNKNOWN";
    }
}

void check(VkResult result, std::source_location location)
{
    if (result >= 0) [[likely]] {
        return;
    }
    ::starcore::detail::assertFailed(
        "Vulkan call",
        std::format("{} ({})", toString(result), static_cast<int>(result)),
        location);
}

}  // namespace starcore::vulkan
