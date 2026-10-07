#include <algorithm>
#include <format>

// vulkan.h must precede glfw3.h to get glfwCreateWindowSurface.
#include <vulkan/vulkan.h>
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <VkBootstrap.h>
#include <vk_mem_alloc.h>

#include "starcore/common/assert.hpp"
#include "starcore/common/log.hpp"
#include "starcore/vulkan/check.hpp"
#include "starcore/vulkan/context.hpp"
#include "starcore/vulkan/texture.hpp"

namespace starcore::vulkan
{

struct Context::Impl
{
    vkb::Instance instance;
    vkb::PhysicalDevice physicalDevice;
    vkb::Device device;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    std::uint32_t queueFamily = 0;
    VmaAllocator allocator = VK_NULL_HANDLE;
    DeviceInfo info;

    ~Impl()
    {
        if (allocator != VK_NULL_HANDLE) {
            vmaDestroyAllocator(allocator);
        }
        if (device.device != VK_NULL_HANDLE) {
            vkb::destroy_device(device);
        }
        if (surface != VK_NULL_HANDLE) {
            vkb::destroy_surface(instance, surface);
        }
        if (instance.instance != VK_NULL_HANDLE) {
            vkb::destroy_instance(instance);
        }
    }
};

namespace
{

VKAPI_ATTR auto VKAPI_CALL debugCallback(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT /*types*/,
    const VkDebugUtilsMessengerCallbackDataEXT* data, void* /*userData*/)
    -> VkBool32
{
    if ((severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) != 0) {
        SC_CORE_ERROR("[Vulkan] {}", data->pMessage);
    } else if ((severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) != 0) {
        SC_CORE_WARN("[Vulkan] {}", data->pMessage);
    } else {
        SC_CORE_DEBUG("[Vulkan] {}", data->pMessage);
    }
    return VK_FALSE;
}

}  // namespace

auto Context::create(const ContextConfig& config, GLFWwindow* window)
    -> std::expected<std::unique_ptr<Context>, std::string>
{
    auto impl = std::make_unique<Impl>();

    // Instance ------------------------------------------------------------------------------------
    std::uint32_t glfwExtensionCount = 0;
    const char** glfwExtensions =
        glfwGetRequiredInstanceExtensions(&glfwExtensionCount);
    if (glfwExtensions == nullptr) {
        return std::unexpected("GLFW: Vulkan is not supported on this system");
    }

    vkb::InstanceBuilder instanceBuilder;
    instanceBuilder.set_app_name(config.applicationName)
        .set_engine_name("StarCore")
        .require_api_version(1, 3, 0)
        .enable_extensions(glfwExtensionCount, glfwExtensions);
    if (config.enableValidation) {
        instanceBuilder.request_validation_layers(true).set_debug_callback(
            debugCallback);
    }
    auto instance = instanceBuilder.build();
    if (!instance) {
        return std::unexpected(std::format("Vulkan instance: {}",
                                           instance.error().message()));
    }
    impl->instance = instance.value();
    if (config.enableValidation &&
        impl->instance.debug_messenger == VK_NULL_HANDLE) {
        SC_CORE_WARN("Vulkan validation layers requested but not installed");
    }

    // Surface -------------------------------------------------------------------------------------
    if (const VkResult result = glfwCreateWindowSurface(
            impl->instance.instance, window, nullptr, &impl->surface);
        result != VK_SUCCESS) {
        return std::unexpected(
            std::format("glfwCreateWindowSurface: {}", toString(result)));
    }

    // Physical device -----------------------------------------------------------------------------
    vkb::PhysicalDeviceSelector selector(impl->instance);
    auto physicalDevice =
        selector.set_surface(impl->surface)
            .set_minimum_version(1, 3)
            .prefer_gpu_device_type(vkb::PreferredDeviceType::discrete)
            .select();
    if (!physicalDevice) {
        return std::unexpected(std::format("no suitable GPU: {}",
                                           physicalDevice.error().message()));
    }
    impl->physicalDevice = physicalDevice.value();

    // Logical device + queue ----------------------------------------------------------------------
    vkb::DeviceBuilder deviceBuilder(impl->physicalDevice);
    auto device = deviceBuilder.build();
    if (!device) {
        return std::unexpected(
            std::format("Vulkan device: {}", device.error().message()));
    }
    impl->device = device.value();

    auto queue = impl->device.get_queue(vkb::QueueType::graphics);
    auto queueIndex = impl->device.get_queue_index(vkb::QueueType::graphics);
    if (!queue || !queueIndex) {
        return std::unexpected("no graphics queue");
    }
    impl->queue = queue.value();
    impl->queueFamily = queueIndex.value();

    VkBool32 presentSupported = VK_FALSE;
    vkGetPhysicalDeviceSurfaceSupportKHR(impl->physicalDevice.physical_device,
                                         impl->queueFamily, impl->surface,
                                         &presentSupported);
    if (presentSupported != VK_TRUE) {
        return std::unexpected("the graphics queue cannot present to the window");
    }

    // Allocator -----------------------------------------------------------------------------------
    VmaAllocatorCreateInfo allocatorInfo{};
    allocatorInfo.vulkanApiVersion = VK_API_VERSION_1_3;
    allocatorInfo.instance = impl->instance.instance;
    allocatorInfo.physicalDevice = impl->physicalDevice.physical_device;
    allocatorInfo.device = impl->device.device;
    if (const VkResult result = vmaCreateAllocator(&allocatorInfo, &impl->allocator);
        result != VK_SUCCESS) {
        return std::unexpected(
            std::format("vmaCreateAllocator: {}", toString(result)));
    }

    // Info ----------------------------------------------------------------------------------------
    impl->info.name = impl->physicalDevice.name;
    impl->info.apiVersion = impl->physicalDevice.properties.apiVersion;
    impl->info.discrete = impl->physicalDevice.properties.deviceType ==
                          VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;
    impl->info.rayQuerySupported =
        impl->physicalDevice.is_extension_present(
            VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME) &&
        impl->physicalDevice.is_extension_present(
            VK_KHR_RAY_QUERY_EXTENSION_NAME);

    SC_CORE_INFO("Vulkan device: {} (API {}.{}.{}, {}, ray query: {})",
                 impl->info.name, VK_API_VERSION_MAJOR(impl->info.apiVersion),
                 VK_API_VERSION_MINOR(impl->info.apiVersion),
                 VK_API_VERSION_PATCH(impl->info.apiVersion),
                 impl->info.discrete ? "discrete" : "integrated/other",
                 impl->info.rayQuerySupported ? "supported" : "not supported");

    return std::unique_ptr<Context>(new Context(std::move(impl)));
}

Context::Context(std::unique_ptr<Impl> impl) : m_impl(std::move(impl))
{
}

Context::~Context()
{
    if (m_impl && m_impl->device.device != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(m_impl->device.device);
    }
    SC_ASSERT(m_textures.empty(),
              "all Texture2D objects must be destroyed before the context");
}

auto Context::instance() const noexcept -> VkInstance
{
    return m_impl->instance.instance;
}

auto Context::physicalDevice() const noexcept -> VkPhysicalDevice
{
    return m_impl->physicalDevice.physical_device;
}

auto Context::device() const noexcept -> VkDevice
{
    return m_impl->device.device;
}

auto Context::surface() const noexcept -> VkSurfaceKHR
{
    return m_impl->surface;
}

auto Context::queue() const noexcept -> VkQueue
{
    return m_impl->queue;
}

auto Context::queueFamily() const noexcept -> std::uint32_t
{
    return m_impl->queueFamily;
}

auto Context::allocator() const noexcept -> VmaAllocator
{
    return m_impl->allocator;
}

auto Context::apiVersion() const noexcept -> std::uint32_t
{
    return VK_API_VERSION_1_3;
}

auto Context::deviceInfo() const noexcept -> const DeviceInfo&
{
    return m_impl->info;
}

void Context::waitIdle() const
{
    check(vkDeviceWaitIdle(device()));
}

void Context::registerTexture(Texture2D* texture)
{
    m_textures.push_back(texture);
}

void Context::unregisterTexture(Texture2D* texture)
{
    std::erase(m_textures, texture);
}

void Context::recordTextureUploads(VkCommandBuffer commandBuffer,
                                   std::uint32_t frameSlot)
{
    for (Texture2D* texture : m_textures) {
        texture->recordUpload(commandBuffer, frameSlot);
    }
}

}  // namespace starcore::vulkan
