#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include <imgui.h>
#include <vulkan/vulkan.h>

#include "starcore/vulkan/context.hpp"

typedef struct VmaAllocation_T* VmaAllocation;  // NOLINT(modernize-use-using)

namespace starcore::ui
{

/**
 * @brief RGBA8 texture fed from the CPU every frame and drawable with
 *        ImGui::Image (replaces Walnut::Image).
 *
 * setPixels() only copies into a CPU buffer; the GPU copy is recorded into
 * the frame's command buffer before the UI pass (vulkan::FrameRecorder),
 * through a staging buffer per frame in flight, so the GPU is never stalled
 * (Walnut waited for the queue on every upload). Resizing recreates the image
 * (after a device wait, which is rare).
 */
class Texture2D final : public vulkan::FrameRecorder
{
public:
    explicit Texture2D(vulkan::Context& context);
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

    void recordFrame(VkCommandBuffer commandBuffer, std::uint32_t frameSlot) override;

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

    vulkan::Context* m_context;
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

/**
 * @brief An image rendered on the GPU (e.g. by GpuRenderer), drawable with
 *        ImGui::Image. Only registers a descriptor; the image is owned elsewhere.
 */
class ImGuiImage
{
public:
    ImGuiImage() = default;
    ~ImGuiImage();
    ImGuiImage(const ImGuiImage&) = delete;
    auto operator=(const ImGuiImage&) -> ImGuiImage& = delete;

    /**
     * @brief Show `view` (sampled in `layout`). A different view replaces the
     *        descriptor: the old view must no longer be in use by the GPU
     *        (renderers wait for the device before recreating their images).
     */
    void set(VkImageView view, VkImageLayout layout);
    void reset();

    /** @brief For ImGui::Image; 0 before set(). */
    [[nodiscard]] auto imguiTexture() const noexcept -> ImTextureID;

private:
    VkImageView m_view = VK_NULL_HANDLE;
    VkDescriptorSet m_descriptor = VK_NULL_HANDLE;
};

}  // namespace starcore::ui
