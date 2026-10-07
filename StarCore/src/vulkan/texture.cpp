#include <cstring>

#include <imgui_impl_vulkan.h>
#include <vk_mem_alloc.h>

#include "starcore/common/assert.hpp"
#include "starcore/vulkan/check.hpp"
#include "starcore/vulkan/context.hpp"
#include "starcore/vulkan/texture.hpp"

namespace starcore::vulkan
{

namespace
{

constexpr VkFormat kFormat = VK_FORMAT_R8G8B8A8_UNORM;

}  // namespace

Texture2D::Texture2D(Context& context) : m_context(&context)
{
    m_context->registerTexture(this);
}

Texture2D::~Texture2D()
{
    m_context->unregisterTexture(this);
    if (m_image != VK_NULL_HANDLE || !m_staging.empty()) {
        // Frames in flight may still sample the image or read staging.
        m_context->waitIdle();
    }
    destroyImage();
    for (auto& staging : m_staging) {
        destroyStaging(staging);
    }
}

auto Texture2D::imguiTexture() const noexcept -> ImTextureID
{
    return static_cast<ImTextureID>(reinterpret_cast<std::uintptr_t>(m_descriptor));
}

void Texture2D::setPixels(std::span<const std::uint32_t> pixels,
                          std::uint32_t width, std::uint32_t height)
{
    SC_VERIFY(pixels.size() == static_cast<std::size_t>(width) * height,
              "pixel count {} does not match {}x{}", pixels.size(), width,
              height);
    if (width == 0 || height == 0) {
        return;
    }
    if (width != m_width || height != m_height) {
        if (m_image != VK_NULL_HANDLE) {
            m_context->waitIdle();
            destroyImage();
        }
        createImage(width, height);
    }
    m_pending.assign(pixels.begin(), pixels.end());
    m_dirty = true;
}

void Texture2D::createImage(std::uint32_t width, std::uint32_t height)
{
    m_width = width;
    m_height = height;

    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = kFormat;
    imageInfo.extent = {width, height, 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    // STORAGE: the planned GPU path tracer will write this image directly.
    imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                      VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VmaAllocationCreateInfo allocationInfo{};
    allocationInfo.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    check(vmaCreateImage(m_context->allocator(), &imageInfo, &allocationInfo,
                         &m_image, &m_imageAllocation, nullptr));

    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = m_image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = kFormat;
    viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    check(vkCreateImageView(m_context->device(), &viewInfo, nullptr, &m_view));

    m_descriptor = ImGui_ImplVulkan_AddTexture(
        m_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    m_layoutInitialized = false;
}

void Texture2D::destroyImage()
{
    if (m_descriptor != VK_NULL_HANDLE) {
        ImGui_ImplVulkan_RemoveTexture(m_descriptor);
        m_descriptor = VK_NULL_HANDLE;
    }
    if (m_view != VK_NULL_HANDLE) {
        vkDestroyImageView(m_context->device(), m_view, nullptr);
        m_view = VK_NULL_HANDLE;
    }
    if (m_image != VK_NULL_HANDLE) {
        vmaDestroyImage(m_context->allocator(), m_image, m_imageAllocation);
        m_image = VK_NULL_HANDLE;
        m_imageAllocation = nullptr;
    }
    m_width = 0;
    m_height = 0;
}

void Texture2D::destroyStaging(StagingBuffer& staging)
{
    if (staging.buffer != VK_NULL_HANDLE) {
        vmaDestroyBuffer(m_context->allocator(), staging.buffer,
                         staging.allocation);
    }
    staging = {};
}

void Texture2D::recordUpload(VkCommandBuffer commandBuffer,
                             std::uint32_t frameSlot)
{
    if (!m_dirty || m_image == VK_NULL_HANDLE) {
        return;
    }
    m_dirty = false;

    // The staging buffer of this frame slot is free: the slot's previous
    // submission was waited on before recording started.
    if (frameSlot >= m_staging.size()) {
        m_staging.resize(frameSlot + 1);
    }
    StagingBuffer& staging = m_staging[frameSlot];
    const std::uint64_t byteSize = m_pending.size() * sizeof(std::uint32_t);
    if (staging.size < byteSize) {
        destroyStaging(staging);
        VkBufferCreateInfo bufferInfo{};
        bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufferInfo.size = byteSize;
        bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        VmaAllocationCreateInfo allocationInfo{};
        allocationInfo.usage = VMA_MEMORY_USAGE_AUTO;
        allocationInfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                               VMA_ALLOCATION_CREATE_MAPPED_BIT;
        VmaAllocationInfo info{};
        check(vmaCreateBuffer(m_context->allocator(), &bufferInfo,
                              &allocationInfo, &staging.buffer,
                              &staging.allocation, &info));
        staging.mapped = info.pMappedData;
        staging.size = byteSize;
    }
    std::memcpy(staging.mapped, m_pending.data(), byteSize);
    check(vmaFlushAllocation(m_context->allocator(), staging.allocation, 0,
                             byteSize));

    VkImageMemoryBarrier toTransfer{};
    toTransfer.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toTransfer.srcAccessMask = m_layoutInitialized ? VK_ACCESS_SHADER_READ_BIT : 0;
    toTransfer.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    toTransfer.oldLayout = m_layoutInitialized
                               ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                               : VK_IMAGE_LAYOUT_UNDEFINED;
    toTransfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toTransfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toTransfer.image = m_image;
    toTransfer.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(commandBuffer,
                         m_layoutInitialized ? VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT
                                             : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &toTransfer);

    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {m_width, m_height, 1};
    vkCmdCopyBufferToImage(commandBuffer, staging.buffer, m_image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    VkImageMemoryBarrier toShader = toTransfer;
    toShader.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    toShader.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    toShader.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toShader.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr,
                         0, nullptr, 1, &toShader);
    m_layoutInitialized = true;
}

}  // namespace starcore::vulkan
