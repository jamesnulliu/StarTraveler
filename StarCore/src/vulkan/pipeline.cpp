#include "starcore/common/assert.hpp"
#include "starcore/vulkan/check.hpp"
#include "starcore/vulkan/context.hpp"
#include "starcore/vulkan/pipeline.hpp"

namespace starcore::vulkan
{

ComputePipeline::ComputePipeline(Context& context, std::span<const std::uint32_t> spirv,
                                 std::span<const VkDescriptorType> bindings,
                                 std::uint32_t pushConstantSize)
    : m_context(&context)
{
    const VkDevice device = context.device();

    std::vector<VkDescriptorSetLayoutBinding> layoutBindings;
    for (std::uint32_t i = 0; i < bindings.size(); ++i) {
        layoutBindings.push_back(VkDescriptorSetLayoutBinding{
            .binding = i,
            .descriptorType = bindings[i],
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
            .pImmutableSamplers = nullptr,
        });
    }
    VkDescriptorSetLayoutCreateInfo setInfo{};
    setInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    setInfo.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
    setInfo.bindingCount = static_cast<std::uint32_t>(layoutBindings.size());
    setInfo.pBindings = layoutBindings.data();
    check(vkCreateDescriptorSetLayout(device, &setInfo, nullptr, &m_setLayout));

    const VkPushConstantRange pushRange{VK_SHADER_STAGE_COMPUTE_BIT, 0, pushConstantSize};
    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &m_setLayout;
    layoutInfo.pushConstantRangeCount = pushConstantSize > 0 ? 1 : 0;
    layoutInfo.pPushConstantRanges = &pushRange;
    check(vkCreatePipelineLayout(device, &layoutInfo, nullptr, &m_layout));

    VkShaderModuleCreateInfo moduleInfo{};
    moduleInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    moduleInfo.codeSize = spirv.size_bytes();
    moduleInfo.pCode = spirv.data();
    VkShaderModule module = VK_NULL_HANDLE;
    check(vkCreateShaderModule(device, &moduleInfo, nullptr, &module));

    VkComputePipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipelineInfo.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    pipelineInfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    pipelineInfo.stage.module = module;
    pipelineInfo.stage.pName = "main";
    pipelineInfo.layout = m_layout;
    const VkResult result =
        vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &m_pipeline);
    vkDestroyShaderModule(device, module, nullptr);
    check(result);
}

ComputePipeline::~ComputePipeline()
{
    const VkDevice device = m_context->device();
    vkDestroyPipeline(device, m_pipeline, nullptr);
    vkDestroyPipelineLayout(device, m_layout, nullptr);
    vkDestroyDescriptorSetLayout(device, m_setLayout, nullptr);
}

void ComputePipeline::bind(VkCommandBuffer commandBuffer) const
{
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipeline);
}

void ComputePipeline::pushDescriptors(VkCommandBuffer commandBuffer,
                                      std::span<const VkWriteDescriptorSet> writes) const
{
    m_context->functions().cmdPushDescriptorSet(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                                                m_layout, 0,
                                                static_cast<std::uint32_t>(writes.size()),
                                                writes.data());
}

void ComputePipeline::pushConstants(VkCommandBuffer commandBuffer, const void* data,
                                    std::uint32_t size) const
{
    vkCmdPushConstants(commandBuffer, m_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, size, data);
}

// DescriptorWrites --------------------------------------------------------------------------------

DescriptorWrites::DescriptorWrites(std::size_t capacity)
{
    m_buffers.reserve(capacity);
    m_images.reserve(capacity);
    m_accelerationStructures.reserve(capacity);
    m_tlasHandles.reserve(capacity);
    m_writes.reserve(capacity);
}

auto DescriptorWrites::storageBuffer(std::uint32_t binding, VkBuffer buffer)
    -> DescriptorWrites&
{
    SC_VERIFY(m_buffers.size() < m_buffers.capacity(), "DescriptorWrites capacity exceeded");
    m_buffers.push_back({buffer, 0, VK_WHOLE_SIZE});
    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstBinding = binding;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    write.pBufferInfo = &m_buffers.back();
    m_writes.push_back(write);
    return *this;
}

auto DescriptorWrites::storageImage(std::uint32_t binding, VkImageView view,
                                    VkImageLayout layout) -> DescriptorWrites&
{
    SC_VERIFY(m_images.size() < m_images.capacity(), "DescriptorWrites capacity exceeded");
    m_images.push_back({VK_NULL_HANDLE, view, layout});
    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstBinding = binding;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    write.pImageInfo = &m_images.back();
    m_writes.push_back(write);
    return *this;
}

auto DescriptorWrites::accelerationStructure(std::uint32_t binding,
                                             VkAccelerationStructureKHR tlas)
    -> DescriptorWrites&
{
    SC_VERIFY(m_accelerationStructures.size() < m_accelerationStructures.capacity(),
              "DescriptorWrites capacity exceeded");
    m_tlasHandles.push_back(tlas);
    VkWriteDescriptorSetAccelerationStructureKHR info{};
    info.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR;
    info.accelerationStructureCount = 1;
    info.pAccelerationStructures = &m_tlasHandles.back();
    m_accelerationStructures.push_back(info);
    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.pNext = &m_accelerationStructures.back();
    write.dstBinding = binding;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
    m_writes.push_back(write);
    return *this;
}

}  // namespace starcore::vulkan
