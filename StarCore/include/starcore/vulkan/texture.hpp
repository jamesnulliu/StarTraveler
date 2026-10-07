#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include <imgui.h>
#include <vulkan/vulkan.h>

typedef struct VmaAllocation_T* VmaAllocation;  // NOLINT(modernize-use-using)

namespace starcore::vulkan
{

class Context;

/**
 * @brief RGBA8 texture fed from the CPU every frame and drawable with
 *        ImGui::Image (replaces Walnut::Image).
 *
 * setPixels() only copies into a CPU buffer; the GPU copy is recorded into
 * the frame's command buffer before the UI pass (Context::
 * recordTextureUploads), through a staging buffer per frame in flight, so the
 * GPU is never stalled (Walnut waited for the queue on every upload).
 * Resizing recreates the image (after a device wait, which is rare).
 */
class Texture2D
{
public:
    explicit Texture2D(Context& context);
    ~Texture2D();
    Texture2D(const Texture2D&) = delete;
    auto operator=(const Texture2D&) -> Texture2D& = delete;

    /**
     * @param pixels Packed RGBA8 (R in the lowest byte), row 0 at the top.
     */
    void setPixels(std::span<const std::uint32_t> pixels, std::uint32_t width,
                   std::uint32_t height);

    /** @brief For ImGui::Image; 0 until the first setPixels(). */
    [[nodiscard]] auto imguiTexture() const noexcept -> ImTextureID;
    [[nodiscard]] auto width() const noexcept -> std::uint32_t
    {
        return m_width;
    }
    [[nodiscard]] auto height() const noexcept -> std::uint32_t
    {
        return m_height;
    }

    /** @brief Called by Context::recordTextureUploads. */
    void recordUpload(VkCommandBuffer commandBuffer, std::uint32_t frameSlot);

private:
    struct StagingBuffer
    {
        VkBuffer buffer = VK_NULL_HANDLE;
        VmaAllocation allocation = nullptr;
        void* mapped = nullptr;
        std::uint64_t size = 0;
    };

    void createImage(std::uint32_t width, std::uint32_t height);
    void destroyImage();
    void destroyStaging(StagingBuffer& staging);

    Context* m_context;
    std::uint32_t m_width = 0;
    std::uint32_t m_height = 0;

    VkImage m_image = VK_NULL_HANDLE;
    VmaAllocation m_imageAllocation = nullptr;
    VkImageView m_view = VK_NULL_HANDLE;
    VkDescriptorSet m_descriptor = VK_NULL_HANDLE;
    bool m_layoutInitialized = false;

    std::vector<std::uint32_t> m_pending;
    bool m_dirty = false;
    std::vector<StagingBuffer> m_staging;  // One per frame in flight.
};

}  // namespace starcore::vulkan
