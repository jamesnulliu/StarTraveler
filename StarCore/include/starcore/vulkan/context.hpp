#pragma once

#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <vector>

#include <vulkan/vulkan.h>

struct GLFWwindow;
typedef struct VmaAllocator_T* VmaAllocator;  // NOLINT(modernize-use-using)

namespace starcore::vulkan
{

class Texture2D;

struct ContextConfig
{
    std::string applicationName = "StarCore";
    // Request VK_LAYER_KHRONOS_validation (used only if it is installed).
    bool enableValidation = false;
};

struct DeviceInfo
{
    std::string name;
    std::uint32_t apiVersion = 0;
    bool discrete = false;
    // VK_KHR_acceleration_structure + VK_KHR_ray_query available (needed by
    // the planned GPU path tracer; not enabled yet).
    bool rayQuerySupported = false;
};

/**
 * @brief Vulkan instance, window surface, device (Vulkan 1.3, discrete GPU
 *        preferred), one graphics+present queue and a VMA allocator.
 */
class Context
{
public:
    [[nodiscard]] static auto create(const ContextConfig& config,
                                     GLFWwindow* window)
        -> std::expected<std::unique_ptr<Context>, std::string>;

    ~Context();
    Context(const Context&) = delete;
    auto operator=(const Context&) -> Context& = delete;

    [[nodiscard]] auto instance() const noexcept -> VkInstance;
    [[nodiscard]] auto physicalDevice() const noexcept -> VkPhysicalDevice;
    [[nodiscard]] auto device() const noexcept -> VkDevice;
    [[nodiscard]] auto surface() const noexcept -> VkSurfaceKHR;
    [[nodiscard]] auto queue() const noexcept -> VkQueue;
    [[nodiscard]] auto queueFamily() const noexcept -> std::uint32_t;
    [[nodiscard]] auto allocator() const noexcept -> VmaAllocator;
    [[nodiscard]] auto apiVersion() const noexcept -> std::uint32_t;
    [[nodiscard]] auto deviceInfo() const noexcept -> const DeviceInfo&;

    void waitIdle() const;

    // Texture uploads (see Texture2D) ------------------------------------------------------------

    void registerTexture(Texture2D* texture);
    void unregisterTexture(Texture2D* texture);

    /**
     * @brief Record pending CPU->GPU texture copies into `commandBuffer`
     *        (called by the Application before the UI render pass).
     * @param frameSlot Index of the frame in flight owning `commandBuffer`;
     *        its previous submission has completed.
     */
    void recordTextureUploads(VkCommandBuffer commandBuffer,
                              std::uint32_t frameSlot);

private:
    struct Impl;
    explicit Context(std::unique_ptr<Impl> impl);

    std::unique_ptr<Impl> m_impl;
    std::vector<Texture2D*> m_textures;
};

}  // namespace starcore::vulkan
