#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include <vulkan/vulkan.h>

namespace starcore::vulkan
{

class Context;

/**
 * @brief A pipeline whose descriptor set 0 is pushed at record time
 *        (VK_KHR_push_descriptor: no pools, nothing to keep alive per frame).
 *
 * Binding i of set 0 has type bindings[i]; one descriptor each, visible to
 * every stage of the pipeline, as is the push constant range.
 */
class Pipeline
{
public:
    ~Pipeline();
    Pipeline(const Pipeline&) = delete;
    auto operator=(const Pipeline&) -> Pipeline& = delete;

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

protected:
    Pipeline(Context& context, VkPipelineBindPoint bindPoint, VkShaderStageFlags stages,
             std::span<const VkDescriptorType> bindings, std::uint32_t pushConstantSize);

    [[nodiscard]] auto createShaderModule(std::span<const std::uint32_t> spirv) const
        -> VkShaderModule;

    Context* m_context;
    VkPipelineBindPoint m_bindPoint;
    VkShaderStageFlags m_stages;
    VkDescriptorSetLayout m_setLayout = VK_NULL_HANDLE;
    VkPipelineLayout m_layout = VK_NULL_HANDLE;
    VkPipeline m_pipeline = VK_NULL_HANDLE;
};

class ComputePipeline final : public Pipeline
{
public:
    ComputePipeline(Context& context, std::span<const std::uint32_t> spirv,
                    std::span<const VkDescriptorType> bindings,
                    std::uint32_t pushConstantSize = 0);
};

struct GraphicsPipelineConfig
{
    std::span<const std::uint32_t> vertexSpirv;
    std::span<const std::uint32_t> fragmentSpirv;
    std::span<const VkDescriptorType> bindings;
    std::uint32_t pushConstantSize = 0;
    std::span<const VkFormat> colorFormats;
    VkFormat depthFormat = VK_FORMAT_UNDEFINED;
    VkCompareOp depthCompare = VK_COMPARE_OP_GREATER;  // Reversed depth.
    bool depthWrite = true;
};

/**
 * @brief Triangle-list pipeline for dynamic rendering (no render pass), no
 *        vertex input (shaders pull vertices from buffers), no culling, no
 *        blending, dynamic viewport and scissor.
 */
class GraphicsPipeline final : public Pipeline
{
public:
    GraphicsPipeline(Context& context, const GraphicsPipelineConfig& config);
};

/**
 * @brief Builder of VkWriteDescriptorSet arrays for Pipeline::pushDescriptors
 *        (owns the info structs the writes point to).
 */
class DescriptorWrites
{
public:
    explicit DescriptorWrites(std::size_t capacity = 24);

    auto storageBuffer(std::uint32_t binding, VkBuffer buffer) -> DescriptorWrites&;
    auto uniformBuffer(std::uint32_t binding, VkBuffer buffer) -> DescriptorWrites&;
    auto storageImage(std::uint32_t binding, VkImageView view,
                      VkImageLayout layout = VK_IMAGE_LAYOUT_GENERAL) -> DescriptorWrites&;
    auto accelerationStructure(std::uint32_t binding, VkAccelerationStructureKHR tlas)
        -> DescriptorWrites&;

    [[nodiscard]] auto writes() const -> std::span<const VkWriteDescriptorSet>
    {
        return m_writes;
    }

private:
    auto buffer(std::uint32_t binding, VkBuffer buffer, VkDescriptorType type)
        -> DescriptorWrites&;

    // Reserved up front: the writes keep pointers into these.
    std::vector<VkDescriptorBufferInfo> m_buffers;
    std::vector<VkDescriptorImageInfo> m_images;
    std::vector<VkWriteDescriptorSetAccelerationStructureKHR> m_accelerationStructures;
    std::vector<VkAccelerationStructureKHR> m_tlasHandles;
    std::vector<VkWriteDescriptorSet> m_writes;
};

}  // namespace starcore::vulkan
