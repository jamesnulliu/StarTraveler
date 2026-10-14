#include <algorithm>
#include <cstring>
#include <utility>

#include <vk_mem_alloc.h>

#include "starcore/common/assert.hpp"
#include "starcore/vulkan/check.hpp"
#include "starcore/vulkan/context.hpp"
#include "starcore/vulkan/resources.hpp"

namespace starcore::vulkan
{

// Buffer ------------------------------------------------------------------------------------------

Buffer::Buffer(Context& context, VkDeviceSize size, VkBufferUsageFlags usage,
               MemoryUsage memory)
    : m_context(&context), m_size(std::max<VkDeviceSize>(size, 4))
{
    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = m_size;
    bufferInfo.usage = usage;

    VmaAllocationCreateInfo allocationInfo{};
    switch (memory) {
    case MemoryUsage::DeviceLocal:
        allocationInfo.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
        break;
    case MemoryUsage::Upload:
        allocationInfo.usage = VMA_MEMORY_USAGE_AUTO;
        allocationInfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                               VMA_ALLOCATION_CREATE_MAPPED_BIT;
        break;
    case MemoryUsage::Readback:
        allocationInfo.usage = VMA_MEMORY_USAGE_AUTO;
        allocationInfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT |
                               VMA_ALLOCATION_CREATE_MAPPED_BIT;
        break;
    }
    VmaAllocationInfo info{};
    check(vmaCreateBuffer(context.allocator(), &bufferInfo, &allocationInfo, &m_buffer,
                          &m_allocation, &info));
    m_mapped = static_cast<std::byte*>(info.pMappedData);
}

Buffer::~Buffer()
{
    destroy();
}

Buffer::Buffer(Buffer&& other) noexcept
    : m_context(std::exchange(other.m_context, nullptr)),
      m_buffer(std::exchange(other.m_buffer, VK_NULL_HANDLE)),
      m_allocation(std::exchange(other.m_allocation, nullptr)),
      m_size(std::exchange(other.m_size, 0)), m_mapped(std::exchange(other.m_mapped, nullptr))
{
}

auto Buffer::operator=(Buffer&& other) noexcept -> Buffer&
{
    if (this != &other) {
        destroy();
        m_context = std::exchange(other.m_context, nullptr);
        m_buffer = std::exchange(other.m_buffer, VK_NULL_HANDLE);
        m_allocation = std::exchange(other.m_allocation, nullptr);
        m_size = std::exchange(other.m_size, 0);
        m_mapped = std::exchange(other.m_mapped, nullptr);
    }
    return *this;
}

void Buffer::destroy() noexcept
{
    if (m_buffer != VK_NULL_HANDLE) {
        vmaDestroyBuffer(m_context->allocator(), m_buffer, m_allocation);
    }
    m_buffer = VK_NULL_HANDLE;
    m_allocation = nullptr;
    m_mapped = nullptr;
    m_size = 0;
}

auto Buffer::deviceAddress() const -> VkDeviceAddress
{
    VkBufferDeviceAddressInfo info{};
    info.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
    info.buffer = m_buffer;
    return vkGetBufferDeviceAddress(m_context->device(), &info);
}

void Buffer::write(std::span<const std::byte> data, VkDeviceSize offset)
{
    SC_VERIFY(m_mapped != nullptr, "Buffer::write needs a mapped buffer");
    SC_VERIFY(offset + data.size() <= m_size, "Buffer::write out of range ({} + {} > {})",
              offset, data.size(), m_size);
    if (data.empty()) {
        return;
    }
    std::memcpy(m_mapped + offset, data.data(), data.size());
    check(vmaFlushAllocation(m_context->allocator(), m_allocation, offset, data.size()));
}

void Buffer::invalidate()
{
    check(vmaInvalidateAllocation(m_context->allocator(), m_allocation, 0, VK_WHOLE_SIZE));
}

auto createDeviceBuffer(Context& context, std::span<const std::byte> data,
                        VkBufferUsageFlags usage) -> Buffer
{
    Buffer buffer(context, data.size(), usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                  MemoryUsage::DeviceLocal);
    if (data.empty()) {
        return buffer;
    }
    Buffer staging(context, data.size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT, MemoryUsage::Upload);
    staging.write(data);
    context.submitAndWait([&](VkCommandBuffer commandBuffer) {
        const VkBufferCopy region{0, 0, data.size()};
        vkCmdCopyBuffer(commandBuffer, staging.handle(), buffer.handle(), 1, &region);
        // Visible to whatever reads it next (shaders, acceleration structure builds).
        memoryBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                      VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                      VK_ACCESS_MEMORY_READ_BIT);
    });
    return buffer;
}

// Image -------------------------------------------------------------------------------------------

Image::Image(Context& context, std::uint32_t width, std::uint32_t height, VkFormat format,
             VkImageUsageFlags usage)
    : m_context(&context), m_format(format), m_width(width), m_height(height)
{
    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = format;
    imageInfo.extent = {width, height, 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = usage;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VmaAllocationCreateInfo allocationInfo{};
    allocationInfo.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    check(vmaCreateImage(context.allocator(), &imageInfo, &allocationInfo, &m_image,
                         &m_allocation, nullptr));

    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = m_image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = format;
    viewInfo.subresourceRange = {aspect(), 0, 1, 0, 1};
    check(vkCreateImageView(context.device(), &viewInfo, nullptr, &m_view));
}

Image::~Image()
{
    destroy();
}

Image::Image(Image&& other) noexcept
    : m_context(std::exchange(other.m_context, nullptr)),
      m_image(std::exchange(other.m_image, VK_NULL_HANDLE)),
      m_allocation(std::exchange(other.m_allocation, nullptr)),
      m_view(std::exchange(other.m_view, VK_NULL_HANDLE)),
      m_format(std::exchange(other.m_format, VK_FORMAT_UNDEFINED)),
      m_width(std::exchange(other.m_width, 0)), m_height(std::exchange(other.m_height, 0))
{
}

auto Image::operator=(Image&& other) noexcept -> Image&
{
    if (this != &other) {
        destroy();
        m_context = std::exchange(other.m_context, nullptr);
        m_image = std::exchange(other.m_image, VK_NULL_HANDLE);
        m_allocation = std::exchange(other.m_allocation, nullptr);
        m_view = std::exchange(other.m_view, VK_NULL_HANDLE);
        m_format = std::exchange(other.m_format, VK_FORMAT_UNDEFINED);
        m_width = std::exchange(other.m_width, 0);
        m_height = std::exchange(other.m_height, 0);
    }
    return *this;
}

auto Image::aspect() const noexcept -> VkImageAspectFlags
{
    switch (m_format) {
    case VK_FORMAT_D16_UNORM:
    case VK_FORMAT_X8_D24_UNORM_PACK32:
    case VK_FORMAT_D32_SFLOAT:
        return VK_IMAGE_ASPECT_DEPTH_BIT;
    case VK_FORMAT_D16_UNORM_S8_UINT:
    case VK_FORMAT_D24_UNORM_S8_UINT:
    case VK_FORMAT_D32_SFLOAT_S8_UINT:
        return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    default:
        return VK_IMAGE_ASPECT_COLOR_BIT;
    }
}

void Image::destroy() noexcept
{
    if (m_view != VK_NULL_HANDLE) {
        vkDestroyImageView(m_context->device(), m_view, nullptr);
    }
    if (m_image != VK_NULL_HANDLE) {
        vmaDestroyImage(m_context->allocator(), m_image, m_allocation);
    }
    m_view = VK_NULL_HANDLE;
    m_image = VK_NULL_HANDLE;
    m_allocation = nullptr;
    m_width = 0;
    m_height = 0;
}

// Sampler -----------------------------------------------------------------------------------------

Sampler::Sampler(Context& context, VkFilter filter) : m_context(&context)
{
    VkSamplerCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    info.magFilter = filter;
    info.minFilter = filter;
    info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    info.maxLod = 0.0F;
    check(vkCreateSampler(context.device(), &info, nullptr, &m_sampler));
}

Sampler::~Sampler()
{
    destroy();
}

Sampler::Sampler(Sampler&& other) noexcept
    : m_context(std::exchange(other.m_context, nullptr)),
      m_sampler(std::exchange(other.m_sampler, VK_NULL_HANDLE))
{
}

auto Sampler::operator=(Sampler&& other) noexcept -> Sampler&
{
    if (this != &other) {
        destroy();
        m_context = std::exchange(other.m_context, nullptr);
        m_sampler = std::exchange(other.m_sampler, VK_NULL_HANDLE);
    }
    return *this;
}

void Sampler::destroy() noexcept
{
    if (m_sampler != VK_NULL_HANDLE) {
        vkDestroySampler(m_context->device(), m_sampler, nullptr);
    }
    m_sampler = VK_NULL_HANDLE;
}

// Barriers ----------------------------------------------------------------------------------------

void transitionImage(VkCommandBuffer commandBuffer, VkImage image, VkImageLayout oldLayout,
                     VkImageLayout newLayout, VkPipelineStageFlags srcStage,
                     VkAccessFlags srcAccess, VkPipelineStageFlags dstStage,
                     VkAccessFlags dstAccess, VkImageAspectFlags aspect)
{
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.srcAccessMask = srcAccess;
    barrier.dstAccessMask = dstAccess;
    barrier.oldLayout = oldLayout;
    barrier.newLayout = newLayout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = {aspect, 0, 1, 0, 1};
    vkCmdPipelineBarrier(commandBuffer, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1,
                         &barrier);
}

void memoryBarrier(VkCommandBuffer commandBuffer, VkPipelineStageFlags srcStage,
                   VkAccessFlags srcAccess, VkPipelineStageFlags dstStage,
                   VkAccessFlags dstAccess)
{
    VkMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    barrier.srcAccessMask = srcAccess;
    barrier.dstAccessMask = dstAccess;
    vkCmdPipelineBarrier(commandBuffer, srcStage, dstStage, 0, 1, &barrier, 0, nullptr, 0,
                         nullptr);
}

}  // namespace starcore::vulkan
