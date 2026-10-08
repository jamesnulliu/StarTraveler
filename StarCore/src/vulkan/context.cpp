module;

#include <VkBootstrap.h>
#include <vk_mem_alloc.h>

#include "starcore/common/assert.hpp"
#include "starcore/common/log.hpp"

module starcore.vulkan;

import std;

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
    DeviceFunctions functions;
    std::atomic<std::uint32_t> validationErrors{0};
    bool validation = false;

    // submitAndWait().
    VkCommandPool commandPool = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;

    Impl() = default;
    Impl(const Impl&) = delete;
    auto operator=(const Impl&) -> Impl& = delete;

    ~Impl()
    {
        if (fence != VK_NULL_HANDLE) {
            vkDestroyFence(device.device, fence, nullptr);
        }
        if (commandPool != VK_NULL_HANDLE) {
            vkDestroyCommandPool(device.device, commandPool, nullptr);
        }
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
    const VkDebugUtilsMessengerCallbackDataEXT* data, void* userData)
    -> VkBool32
{
    const std::string_view id = data->pMessageIdName != nullptr ? data->pMessageIdName : "";
    // Dear ImGui's ImGui_ImplVulkanH_CreateOrResizeWindow (v1.92) moves all
    // swapchain images to PRESENT_SRC without acquiring them; harmless, and
    // not ours to fix.
    if (id == "UNASSIGNED-non-acquired-swapchain-image-used") {
        return VK_FALSE;
    }
    if ((severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) != 0) {
        static_cast<std::atomic<std::uint32_t>*>(userData)->fetch_add(1);
        SC_CORE_ERROR("[Vulkan] {} {}", id, data->pMessage);
    } else if ((severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) != 0) {
        SC_CORE_WARN("[Vulkan] {}", data->pMessage);
    } else {
        SC_CORE_DEBUG("[Vulkan] {}", data->pMessage);
    }
    return VK_FALSE;
}

/** @brief Point the loader at the validation layer built by vcpkg (unless the user set a path). */
void addValidationLayerPath()
{
#ifdef STARCORE_VULKAN_LAYER_DIR
    if (std::getenv("VK_ADD_LAYER_PATH") != nullptr || std::getenv("VK_LAYER_PATH") != nullptr) {
        return;
    }
    std::error_code error;
    if (!std::filesystem::exists(STARCORE_VULKAN_LAYER_DIR, error)) {
        return;
    }
#ifdef _WIN32
    _putenv_s("VK_ADD_LAYER_PATH", STARCORE_VULKAN_LAYER_DIR);
#else
    setenv("VK_ADD_LAYER_PATH", STARCORE_VULKAN_LAYER_DIR, 0);
#endif
#endif
}

auto lowercase(std::string text) -> std::string
{
    std::ranges::transform(text, text.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return text;
}

/**
 * @brief The suitable device whose name contains `name` (or the environment
 *        variable STARCORE_VULKAN_DEVICE), ignoring case; nullopt for the default.
 */
auto chooseDevice(const vkb::PhysicalDeviceSelector& selector, std::string name)
    -> std::optional<vkb::PhysicalDevice>
{
    if (const char* environment = std::getenv("STARCORE_VULKAN_DEVICE");
        environment != nullptr && *environment != '\0') {
        name = environment;
    }
    if (name.empty()) {
        return std::nullopt;
    }
    auto devices = selector.select_devices();
    if (devices) {
        for (vkb::PhysicalDevice& device : *devices) {
            if (lowercase(device.name).contains(lowercase(name))) {
                return device;
            }
        }
    }
    SC_CORE_WARN("No suitable Vulkan device matches \"{}\"; using the default", name);
    return std::nullopt;
}

template <typename T>
void loadFunction(VkDevice device, T& function, const char* name)
{
    function = reinterpret_cast<T>(vkGetDeviceProcAddr(device, name));
}

}  // namespace

auto Context::create(const ContextConfig& config)
    -> std::expected<std::unique_ptr<Context>, std::string>
{
    auto impl = std::make_unique<Impl>();
    const bool headless = !config.createSurface;

    // Instance ------------------------------------------------------------------------------------
    bool validation = false;
    if (config.enableValidation) {
        addValidationLayerPath();
        const auto system = vkb::SystemInfo::get_system_info();
        validation = system && system->validation_layers_available;
        if (!validation) {
            SC_CORE_WARN("Vulkan validation layers requested but not installed "
                         "(configure with -DSTARCORE_VULKAN_VALIDATION=ON)");
        }
    }

    vkb::InstanceBuilder instanceBuilder;
    instanceBuilder.set_app_name(config.applicationName.c_str())
        .set_engine_name("StarCore")
        .require_api_version(1, 3, 0)
        .set_headless(headless);
    for (const std::string& extension : config.surfaceExtensions) {
        instanceBuilder.enable_extension(extension.c_str());
    }
    if (validation) {
        instanceBuilder.enable_validation_layers()
            .set_debug_callback(debugCallback)
            .set_debug_callback_user_data_pointer(&impl->validationErrors)
            .set_debug_messenger_severity(VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                                          VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
            // Catches missing barriers, the most likely GPU bug.
            .add_validation_feature_enable(
                VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT);
    }
    auto instance = instanceBuilder.build();
    if (!instance) {
        return std::unexpected(std::format("Vulkan instance: {}",
                                           instance.error().message()));
    }
    impl->instance = instance.value();

    // Surface -------------------------------------------------------------------------------------
    if (!headless) {
        if (const VkResult result = config.createSurface(impl->instance.instance, &impl->surface);
            result != VK_SUCCESS) {
            return std::unexpected(std::format("window surface: {}", toString(result)));
        }
    }

    // Physical device -----------------------------------------------------------------------------
    // Buffer device addresses, dynamic rendering and demote-to-helper (what
    // `discard` compiles to for Vulkan 1.3) are core and mandatory in Vulkan 1.3.
    VkPhysicalDeviceVulkan12Features features12{};
    features12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    features12.bufferDeviceAddress = VK_TRUE;
    VkPhysicalDeviceVulkan13Features features13{};
    features13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    features13.dynamicRendering = VK_TRUE;
    features13.shaderDemoteToHelperInvocation = VK_TRUE;

    vkb::PhysicalDeviceSelector selector(impl->instance);
    selector.set_minimum_version(1, 3)
        .set_required_features_12(features12)
        .set_required_features_13(features13)
        .prefer_gpu_device_type(vkb::PreferredDeviceType::discrete);
    if (headless) {
        selector.require_present(false);
    } else {
        selector.set_surface(impl->surface);
    }
    auto physicalDevice = selector.select();
    if (!physicalDevice) {
        return std::unexpected(std::format("no suitable GPU: {}",
                                           physicalDevice.error().message()));
    }
    impl->physicalDevice = physicalDevice.value();
    if (const auto chosen = chooseDevice(selector, config.deviceName)) {
        impl->physicalDevice = *chosen;
    }
    vkb::PhysicalDevice& gpu = impl->physicalDevice;

    // Push descriptors (every GPU renderer pipeline) and ray queries: optional,
    // the CPU path tracer works everywhere.
    bool pushDescriptors = gpu.enable_extension_if_present(VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME);
    const char* rayQueryExtensions[] = {
        VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME,
        VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME,
        VK_KHR_RAY_QUERY_EXTENSION_NAME,
    };
    bool rayQuery = config.enableRayQuery &&
                    std::ranges::all_of(rayQueryExtensions, [&](const char* extension) {
                        return gpu.is_extension_present(extension);
                    });
    if (rayQuery) {
        VkPhysicalDeviceAccelerationStructureFeaturesKHR accelerationFeatures{};
        accelerationFeatures.sType =
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR;
        accelerationFeatures.accelerationStructure = VK_TRUE;
        VkPhysicalDeviceRayQueryFeaturesKHR rayQueryFeatures{};
        rayQueryFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR;
        rayQueryFeatures.rayQuery = VK_TRUE;
        rayQuery = gpu.enable_extensions_if_present(std::size(rayQueryExtensions),
                                                    rayQueryExtensions) &&
                   gpu.enable_extension_features_if_present(accelerationFeatures) &&
                   gpu.enable_extension_features_if_present(rayQueryFeatures);
    }

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

    if (!headless) {
        VkBool32 presentSupported = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(gpu.physical_device, impl->queueFamily,
                                             impl->surface, &presentSupported);
        if (presentSupported != VK_TRUE) {
            return std::unexpected("the graphics queue cannot present to the window");
        }
    }

    // Extension functions ------------------------------------------------------------------------
    const VkDevice vkDevice = impl->device.device;
    DeviceFunctions& functions = impl->functions;
    if (rayQuery) {
        loadFunction(vkDevice, functions.createAccelerationStructure,
                     "vkCreateAccelerationStructureKHR");
        loadFunction(vkDevice, functions.destroyAccelerationStructure,
                     "vkDestroyAccelerationStructureKHR");
        loadFunction(vkDevice, functions.getAccelerationStructureBuildSizes,
                     "vkGetAccelerationStructureBuildSizesKHR");
        loadFunction(vkDevice, functions.getAccelerationStructureDeviceAddress,
                     "vkGetAccelerationStructureDeviceAddressKHR");
        loadFunction(vkDevice, functions.cmdBuildAccelerationStructures,
                     "vkCmdBuildAccelerationStructuresKHR");
        rayQuery = functions.createAccelerationStructure != nullptr &&
                   functions.destroyAccelerationStructure != nullptr &&
                   functions.getAccelerationStructureBuildSizes != nullptr &&
                   functions.getAccelerationStructureDeviceAddress != nullptr &&
                   functions.cmdBuildAccelerationStructures != nullptr;
    }
    if (pushDescriptors) {
        loadFunction(vkDevice, functions.cmdPushDescriptorSet, "vkCmdPushDescriptorSetKHR");
        pushDescriptors = functions.cmdPushDescriptorSet != nullptr;
    }

    // Allocator -----------------------------------------------------------------------------------
    VmaAllocatorCreateInfo allocatorInfo{};
    allocatorInfo.flags = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;
    allocatorInfo.vulkanApiVersion = VK_API_VERSION_1_3;
    allocatorInfo.instance = impl->instance.instance;
    allocatorInfo.physicalDevice = gpu.physical_device;
    allocatorInfo.device = vkDevice;
    if (const VkResult result = vmaCreateAllocator(&allocatorInfo, &impl->allocator);
        result != VK_SUCCESS) {
        return std::unexpected(
            std::format("vmaCreateAllocator: {}", toString(result)));
    }

    // One-off submissions -------------------------------------------------------------------------
    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    poolInfo.queueFamilyIndex = impl->queueFamily;
    check(vkCreateCommandPool(vkDevice, &poolInfo, nullptr, &impl->commandPool));
    VkFenceCreateInfo fenceInfo{};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    check(vkCreateFence(vkDevice, &fenceInfo, nullptr, &impl->fence));

    // Info ----------------------------------------------------------------------------------------
    DeviceInfo& info = impl->info;
    info.name = gpu.name;
    info.apiVersion = gpu.properties.apiVersion;
    info.discrete = gpu.properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;
    info.pushDescriptors = pushDescriptors;
    info.rayQuery = rayQuery;
    if (rayQuery) {
        VkPhysicalDeviceAccelerationStructurePropertiesKHR accelerationProperties{};
        accelerationProperties.sType =
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR;
        VkPhysicalDeviceProperties2 properties{};
        properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
        properties.pNext = &accelerationProperties;
        vkGetPhysicalDeviceProperties2(gpu.physical_device, &properties);
        info.scratchAlignment = accelerationProperties.minAccelerationStructureScratchOffsetAlignment;
    }
    const auto families = gpu.get_queue_families();
    if (impl->queueFamily < families.size() &&
        families[impl->queueFamily].timestampValidBits > 0) {
        info.timestampPeriod = gpu.properties.limits.timestampPeriod;
    }

    SC_CORE_INFO("Vulkan device: {} (API {}.{}.{}, {}, ray query: {}{}{}{})", info.name,
                 VK_API_VERSION_MAJOR(info.apiVersion), VK_API_VERSION_MINOR(info.apiVersion),
                 VK_API_VERSION_PATCH(info.apiVersion),
                 info.discrete ? "discrete" : "integrated/other",
                 info.rayQuery ? "enabled" : "not available",
                 info.pushDescriptors ? "" : ", no push descriptors", headless ? ", headless" : "",
                 validation ? ", validation" : "");

    impl->validation = validation;
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
    SC_ASSERT(m_recorders.empty(),
              "all frame recorders (textures) must be destroyed before the context");
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

auto Context::functions() const noexcept -> const DeviceFunctions&
{
    return m_impl->functions;
}

auto Context::validationErrors() const noexcept -> std::uint32_t
{
    return m_impl->validationErrors.load();
}

auto Context::validationEnabled() const noexcept -> bool
{
    return m_impl->validation;
}

void Context::waitIdle() const
{
    check(vkDeviceWaitIdle(device()));
}

void Context::submitAndWait(const std::function<void(VkCommandBuffer)>& record)
{
    const VkDevice vkDevice = device();
    VkCommandBufferAllocateInfo allocateInfo{};
    allocateInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocateInfo.commandPool = m_impl->commandPool;
    allocateInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocateInfo.commandBufferCount = 1;
    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
    check(vkAllocateCommandBuffers(vkDevice, &allocateInfo, &commandBuffer));

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check(vkBeginCommandBuffer(commandBuffer, &beginInfo));
    record(commandBuffer);
    check(vkEndCommandBuffer(commandBuffer));

    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &commandBuffer;
    check(vkQueueSubmit(m_impl->queue, 1, &submitInfo, m_impl->fence));
    check(vkWaitForFences(vkDevice, 1, &m_impl->fence, VK_TRUE, UINT64_MAX));
    check(vkResetFences(vkDevice, 1, &m_impl->fence));
    vkFreeCommandBuffers(vkDevice, m_impl->commandPool, 1, &commandBuffer);
}

void Context::registerFrameRecorder(FrameRecorder* recorder)
{
    m_recorders.push_back(recorder);
}

void Context::unregisterFrameRecorder(FrameRecorder* recorder)
{
    std::erase(m_recorders, recorder);
}

void Context::recordFrame(VkCommandBuffer commandBuffer, std::uint32_t frameSlot)
{
    for (FrameRecorder* recorder : m_recorders) {
        recorder->recordFrame(commandBuffer, frameSlot);
    }
}

}  // namespace starcore::vulkan
