#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include <vulkan/vulkan.h>

namespace starcore::vulkan
{

class Context;

/**
 * @brief Compute pipeline whose descriptor set 0 is pushed at record time
 *        (VK_KHR_push_descriptor: no pools, nothing to keep alive per frame).
 *
 * Binding i of set 0 has type bindings[i]; one descriptor each.
 */
class ComputePipeline
{
public:
    ComputePipeline(Context& context, std::span<const std::uint32_t> spirv,
                    std::span<const VkDescriptorType> bindings,
                    std::uint32_t pushConstantSize = 0);
    ~ComputePipeline();
    ComputePipeline(const ComputePipeline&) = delete;
    auto operator=(const ComputePipeline&) -> ComputePipeline& = delete;

    [[nodiscard]] auto pipeline() const noexcept -> VkPipeline
    {
        return m_pipeline;
    }
    [[nodiscard]] auto layout() const noexcept -> VkPipelineLayout
    {
        return m_layout;
    }

    void bind(VkCommandBuffer commandBuffer) const;
    /** @brief Push descriptor set 0 (each write's dstBinding/type must match the layout). */
    void pushDescriptors(VkCommandBuffer commandBuffer,
                         std::span<const VkWriteDescriptorSet> writes) const;
    void pushConstants(VkCommandBuffer commandBuffer, const void* data,
                       std::uint32_t size) const;

private:
    Context* m_context;
    VkDescriptorSetLayout m_setLayout = VK_NULL_HANDLE;
    VkPipelineLayout m_layout = VK_NULL_HANDLE;
    VkPipeline m_pipeline = VK_NULL_HANDLE;
};

/**
 * @brief Builder of VkWriteDescriptorSet arrays for ComputePipeline::
 *        pushDescriptors (owns the info structs the writes point to).
 */
class DescriptorWrites
{
public:
    explicit DescriptorWrites(std::size_t capacity = 16);

    auto storageBuffer(std::uint32_t binding, VkBuffer buffer) -> DescriptorWrites&;
    auto storageImage(std::uint32_t binding, VkImageView view,
                      VkImageLayout layout = VK_IMAGE_LAYOUT_GENERAL) -> DescriptorWrites&;
    auto accelerationStructure(std::uint32_t binding, VkAccelerationStructureKHR tlas)
        -> DescriptorWrites&;

    [[nodiscard]] auto writes() const -> std::span<const VkWriteDescriptorSet>
    {
        return m_writes;
    }

private:
    // Reserved up front: the writes keep pointers into these.
    std::vector<VkDescriptorBufferInfo> m_buffers;
    std::vector<VkDescriptorImageInfo> m_images;
    std::vector<VkWriteDescriptorSetAccelerationStructureKHR> m_accelerationStructures;
    std::vector<VkAccelerationStructureKHR> m_tlasHandles;
    std::vector<VkWriteDescriptorSet> m_writes;
};

}  // namespace starcore::vulkan
