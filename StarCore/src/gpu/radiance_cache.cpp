#include "radiance_cache.hpp"
#include "starcore/vulkan/context.hpp"

namespace starcore::gpu
{

namespace
{

#include "shaders/cache_resolve.comp.spv.h"

// Entries (a power of two times radiance_cache.glsl's kCacheBucket): 40 bytes each.
constexpr std::uint32_t kCapacity = 1U << 20U;
// The samples an entry's light averages at most: where many paths pass every frame it
// follows changing light within a frame or two, where few do over more frames.
constexpr float kMaxSamples = 128.0F;
// Frames an entry is kept without samples (its surface out of sight).
constexpr std::uint32_t kMaxAge = 240;

/** @brief Push constants of cache_resolve.comp. */
struct ResolveParams
{
    std::uint32_t capacity = kCapacity;
    float maxSamples = kMaxSamples;
    std::uint32_t maxAge = kMaxAge;
};

constexpr VkDescriptorType kResolveBindings[] = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                                 VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                                 VK_DESCRIPTOR_TYPE_STORAGE_BUFFER};

auto storage(vulkan::Context& context, VkDeviceSize size) -> vulkan::Buffer
{
    return vulkan::Buffer(context, size,
                          VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                              VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                          vulkan::MemoryUsage::DeviceLocal);
}

void addressOf(const vulkan::Buffer& buffer, std::uint32_t* pair)
{
    const VkDeviceAddress address = buffer.deviceAddress();
    pair[0] = static_cast<std::uint32_t>(address & 0xFFFFFFFFU);
    pair[1] = static_cast<std::uint32_t>(address >> 32U);
}

}  // namespace

RadianceCache::RadianceCache(vulkan::Context& context)
    : m_context(&context),
      m_resolve(context, cache_resolve_comp_spv, kResolveBindings,
                static_cast<std::uint32_t>(sizeof(ResolveParams))),
      m_keys(storage(context, VkDeviceSize{kCapacity} * 8)),
      m_sums(storage(context, VkDeviceSize{kCapacity} * 16)),
      m_values(storage(context, VkDeviceSize{kCapacity} * 16))
{
    context.submitAndWait([&](VkCommandBuffer commandBuffer) { clear(commandBuffer); });
}

void RadianceCache::describe(FrameUniforms& uniforms) const
{
    addressOf(m_keys, &uniforms.cacheBuffers[0]);
    addressOf(m_sums, &uniforms.cacheBuffers[2]);
    addressOf(m_values, &uniforms.cacheValues[0]);
    uniforms.cacheValues[2] = kCapacity;
}

void RadianceCache::clear(VkCommandBuffer commandBuffer) const
{
    for (const vulkan::Buffer* buffer : {&m_keys, &m_sums, &m_values}) {
        vkCmdFillBuffer(commandBuffer, buffer->handle(), 0, VK_WHOLE_SIZE, 0);
    }
    vulkan::memoryBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                          VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                          VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
}

void RadianceCache::resolve(VkCommandBuffer commandBuffer) const
{
    vulkan::DescriptorWrites writes;
    writes.storageBuffer(0, m_keys.handle())
        .storageBuffer(1, m_sums.handle())
        .storageBuffer(2, m_values.handle());
    const ResolveParams params;
    m_resolve.bind(commandBuffer);
    m_resolve.pushDescriptors(commandBuffer, writes.writes());
    m_resolve.pushConstants(commandBuffer, &params, sizeof(params));
    vkCmdDispatch(commandBuffer, (kCapacity + 63) / 64, 1, 1);
    vulkan::memoryBarrier(commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                          VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                          VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
}

}  // namespace starcore::gpu
