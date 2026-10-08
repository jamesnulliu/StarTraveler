#pragma once

#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <vulkan/vulkan.h>

typedef struct VmaAllocator_T* VmaAllocator;  // NOLINT(modernize-use-using)

namespace starcore::vulkan
{

struct ContextConfig
{
    std::string applicationName = "StarCore";
    // Request VK_LAYER_KHRONOS_validation (used only if it is installed, see
    // STARCORE_VULKAN_VALIDATION).
    bool enableValidation = false;
    // A window surface: the instance extensions it needs and a function that
    // creates it. Without createSurface the context is headless (offscreen
    // rendering, tests).
    std::vector<std::string> surfaceExtensions;
    std::function<VkResult(VkInstance, VkSurfaceKHR*)> createSurface;
    // Enable acceleration structures + ray queries (the GPU renderer's
    // ray-traced pipelines) when the device supports them.
    bool enableRayQuery = true;
    // Use the GPU whose name contains this (case-insensitive) instead of the
    // default (discrete first). The environment variable STARCORE_VULKAN_DEVICE
    // overrides it, e.g. "Intel" or "RTX" on a laptop with two GPUs.
    std::string deviceName;
};

struct DeviceInfo
{
    std::string name;
    std::uint32_t apiVersion = 0;
    bool discrete = false;
    // VK_KHR_push_descriptor is enabled: the GPU renderer can run (its
    // rasterizing pipeline needs nothing else).
    bool pushDescriptors = false;
    // VK_KHR_acceleration_structure and VK_KHR_ray_query are enabled: so can
    // its ray-traced pipelines.
    bool rayQuery = false;
    // VkPhysicalDeviceAccelerationStructurePropertiesKHR (0 without ray query).
    std::uint32_t scratchAlignment = 0;
    // Nanoseconds per timestamp tick; 0 if the queue has no timestamps.
    float timestampPeriod = 0.0F;
};

/** @brief Device functions of extensions (the loader does not export them). */
struct DeviceFunctions
{
    PFN_vkCreateAccelerationStructureKHR createAccelerationStructure = nullptr;
    PFN_vkDestroyAccelerationStructureKHR destroyAccelerationStructure = nullptr;
    PFN_vkGetAccelerationStructureBuildSizesKHR getAccelerationStructureBuildSizes = nullptr;
    PFN_vkGetAccelerationStructureDeviceAddressKHR getAccelerationStructureDeviceAddress =
        nullptr;
    PFN_vkCmdBuildAccelerationStructuresKHR cmdBuildAccelerationStructures = nullptr;
    PFN_vkCmdPushDescriptorSetKHR cmdPushDescriptorSet = nullptr;
};

/**
 * @brief Records commands at the start of every presented frame, before the
 *        UI pass (e.g. CPU -> GPU texture copies).
 */
class FrameRecorder
{
public:
    /**
     * @param frameSlot Index of the frame in flight owning `commandBuffer`;
     *        its previous submission has completed.
     */
    virtual void recordFrame(VkCommandBuffer commandBuffer, std::uint32_t frameSlot) = 0;

protected:
    FrameRecorder() = default;
    ~FrameRecorder() = default;
    FrameRecorder(const FrameRecorder&) = default;
    auto operator=(const FrameRecorder&) -> FrameRecorder& = default;
};

/**
 * @brief Vulkan instance, optional window surface, device (Vulkan 1.3,
 *        discrete GPU preferred), one graphics+compute queue and a VMA
 *        allocator.
 */
class Context
{
public:
    [[nodiscard]] static auto create(const ContextConfig& config)
        -> std::expected<std::unique_ptr<Context>, std::string>;

    ~Context();
    Context(const Context&) = delete;
    auto operator=(const Context&) -> Context& = delete;

    [[nodiscard]] auto instance() const noexcept -> VkInstance;
    [[nodiscard]] auto physicalDevice() const noexcept -> VkPhysicalDevice;
    [[nodiscard]] auto device() const noexcept -> VkDevice;
    /** @brief VK_NULL_HANDLE for a headless context. */
    [[nodiscard]] auto surface() const noexcept -> VkSurfaceKHR;
    [[nodiscard]] auto queue() const noexcept -> VkQueue;
    [[nodiscard]] auto queueFamily() const noexcept -> std::uint32_t;
    [[nodiscard]] auto allocator() const noexcept -> VmaAllocator;
    [[nodiscard]] auto apiVersion() const noexcept -> std::uint32_t;
    [[nodiscard]] auto deviceInfo() const noexcept -> const DeviceInfo&;
    [[nodiscard]] auto functions() const noexcept -> const DeviceFunctions&;

    /** @brief Validation errors reported so far (0 without validation layers). */
    [[nodiscard]] auto validationErrors() const noexcept -> std::uint32_t;
    /** @brief Whether VK_LAYER_KHRONOS_validation is active. */
    [[nodiscard]] auto validationEnabled() const noexcept -> bool;

    void waitIdle() const;

    /**
     * @brief Record one-off commands, submit them and wait for completion
     *        (uploads, acceleration structure builds).
     */
    void submitAndWait(const std::function<void(VkCommandBuffer)>& record);

    // Per-frame recorders (see FrameRecorder) -----------------------------------------------------

    void registerFrameRecorder(FrameRecorder* recorder);
    void unregisterFrameRecorder(FrameRecorder* recorder);
    /** @brief Called by the Application before the UI pass. */
    void recordFrame(VkCommandBuffer commandBuffer, std::uint32_t frameSlot);

private:
    struct Impl;
    explicit Context(std::unique_ptr<Impl> impl);

    std::unique_ptr<Impl> m_impl;
    std::vector<FrameRecorder*> m_recorders;
};

}  // namespace starcore::vulkan
