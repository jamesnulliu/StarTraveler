#pragma once
// RAII wrappers of VMA-allocated buffers and images.

#include <cstddef>
#include <cstdint>
#include <span>

#include <vulkan/vulkan.h>

typedef struct VmaAllocation_T* VmaAllocation;  // NOLINT(modernize-use-using)

namespace starcore::vulkan
{

class Context;

enum class MemoryUsage : std::uint8_t
{
    DeviceLocal,  // GPU only.
    Upload,       // Host-visible, persistently mapped, written sequentially by the CPU.
    Readback,     // Host-visible, persistently mapped, read by the CPU.
};

class Buffer
{
public:
    Buffer() = default;
    /** @param size Bytes (0 is rounded up to 4, Vulkan forbids empty buffers). */
    Buffer(Context& context, VkDeviceSize size, VkBufferUsageFlags usage,
           MemoryUsage memory);
    ~Buffer();
    Buffer(Buffer&& other) noexcept;
    auto operator=(Buffer&& other) noexcept -> Buffer&;
    Buffer(const Buffer&) = delete;
    auto operator=(const Buffer&) -> Buffer& = delete;

    [[nodiscard]] auto handle() const noexcept -> VkBuffer
    {
        return m_buffer;
    }
    [[nodiscard]] auto size() const noexcept -> VkDeviceSize
    {
        return m_size;
    }
    [[nodiscard]] auto isValid() const noexcept -> bool
    {
        return m_buffer != VK_NULL_HANDLE;
    }
    /** @brief Requires VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT. */
    [[nodiscard]] auto deviceAddress() const -> VkDeviceAddress;
    /** @brief Mapped memory of Upload/Readback buffers (nullptr otherwise). */
    [[nodiscard]] auto mapped() const noexcept -> std::byte*
    {
        return m_mapped;
    }

    /** @brief Copy into a mapped buffer and flush. */
    void write(std::span<const std::byte> data, VkDeviceSize offset = 0);
    /** @brief Make GPU writes of a Readback buffer visible to mapped(). */
    void invalidate();

private:
    void destroy() noexcept;

    Context* m_context = nullptr;
    VkBuffer m_buffer = VK_NULL_HANDLE;
    VmaAllocation m_allocation = nullptr;
    VkDeviceSize m_size = 0;
    std::byte* m_mapped = nullptr;
};

/**
 * @brief Device-local buffer filled with `data` through a staging buffer
 *        (blocks until the copy is done).
 */
[[nodiscard]] auto createDeviceBuffer(Context& context, std::span<const std::byte> data,
                                      VkBufferUsageFlags usage) -> Buffer;

/** @brief 2D image with one mip level and a full view. */
class Image
{
public:
    Image() = default;
    Image(Context& context, std::uint32_t width, std::uint32_t height, VkFormat format,
          VkImageUsageFlags usage);
    ~Image();
    Image(Image&& other) noexcept;
    auto operator=(Image&& other) noexcept -> Image&;
    Image(const Image&) = delete;
    auto operator=(const Image&) -> Image& = delete;

    [[nodiscard]] auto handle() const noexcept -> VkImage
    {
        return m_image;
    }
    [[nodiscard]] auto view() const noexcept -> VkImageView
    {
        return m_view;
    }
    [[nodiscard]] auto format() const noexcept -> VkFormat
    {
        return m_format;
    }
    [[nodiscard]] auto width() const noexcept -> std::uint32_t
    {
        return m_width;
    }
    [[nodiscard]] auto height() const noexcept -> std::uint32_t
    {
        return m_height;
    }
    [[nodiscard]] auto isValid() const noexcept -> bool
    {
        return m_image != VK_NULL_HANDLE;
    }
    /** @brief DEPTH (and STENCIL) for depth formats, else COLOR. */
    [[nodiscard]] auto aspect() const noexcept -> VkImageAspectFlags;

private:
    void destroy() noexcept;

    Context* m_context = nullptr;
    VkImage m_image = VK_NULL_HANDLE;
    VmaAllocation m_allocation = nullptr;
    VkImageView m_view = VK_NULL_HANDLE;
    VkFormat m_format = VK_FORMAT_UNDEFINED;
    std::uint32_t m_width = 0;
    std::uint32_t m_height = 0;
};

/** @brief Bilinear sampler, coordinates clamped to the edge (no mipmaps). */
class Sampler
{
public:
    Sampler() = default;
    explicit Sampler(Context& context);
    ~Sampler();
    Sampler(Sampler&& other) noexcept;
    auto operator=(Sampler&& other) noexcept -> Sampler&;
    Sampler(const Sampler&) = delete;
    auto operator=(const Sampler&) -> Sampler& = delete;

    [[nodiscard]] auto handle() const noexcept -> VkSampler
    {
        return m_sampler;
    }

private:
    void destroy() noexcept;

    Context* m_context = nullptr;
    VkSampler m_sampler = VK_NULL_HANDLE;
};

/** @brief Layout transition of a whole image (old layout UNDEFINED discards it). */
void transitionImage(VkCommandBuffer commandBuffer, VkImage image, VkImageLayout oldLayout,
                     VkImageLayout newLayout, VkPipelineStageFlags srcStage,
                     VkAccessFlags srcAccess, VkPipelineStageFlags dstStage,
                     VkAccessFlags dstAccess,
                     VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT);

/** @brief Global memory barrier. */
void memoryBarrier(VkCommandBuffer commandBuffer, VkPipelineStageFlags srcStage,
                   VkAccessFlags srcAccess, VkPipelineStageFlags dstStage,
                   VkAccessFlags dstAccess);

}  // namespace starcore::vulkan
