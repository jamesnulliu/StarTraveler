#include <iterator>

#include "starcore/common/assert.hpp"
#include "starcore/vulkan/check.hpp"
#include "starcore/vulkan/context.hpp"
#include "starcore/vulkan/pipeline.hpp"

namespace starcore::vulkan
{

// Pipeline ----------------------------------------------------------------------------------------

Pipeline::Pipeline(Context& context, VkPipelineBindPoint bindPoint, VkShaderStageFlags stages,
                   std::span<const VkDescriptorType> bindings, std::uint32_t pushConstantSize)
    : m_context(&context), m_bindPoint(bindPoint), m_stages(stages)
{
    const VkDevice device = context.device();

    std::vector<VkDescriptorSetLayoutBinding> layoutBindings;
    for (std::uint32_t i = 0; i < bindings.size(); ++i) {
        layoutBindings.push_back(VkDescriptorSetLayoutBinding{
            .binding = i,
            .descriptorType = bindings[i],
            .descriptorCount = 1,
            .stageFlags = stages,
            .pImmutableSamplers = nullptr,
        });
    }
    VkDescriptorSetLayoutCreateInfo setInfo{};
    setInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    setInfo.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
    setInfo.bindingCount = static_cast<std::uint32_t>(layoutBindings.size());
    setInfo.pBindings = layoutBindings.data();
    check(vkCreateDescriptorSetLayout(device, &setInfo, nullptr, &m_setLayout));

    const VkPushConstantRange pushRange{stages, 0, pushConstantSize};
    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &m_setLayout;
    layoutInfo.pushConstantRangeCount = pushConstantSize > 0 ? 1 : 0;
    layoutInfo.pPushConstantRanges = &pushRange;
    check(vkCreatePipelineLayout(device, &layoutInfo, nullptr, &m_layout));
}

Pipeline::~Pipeline()
{
    const VkDevice device = m_context->device();
    vkDestroyPipeline(device, m_pipeline, nullptr);
    vkDestroyPipelineLayout(device, m_layout, nullptr);
    vkDestroyDescriptorSetLayout(device, m_setLayout, nullptr);
}

auto Pipeline::createShaderModule(std::span<const std::uint32_t> spirv) const -> VkShaderModule
{
    VkShaderModuleCreateInfo moduleInfo{};
    moduleInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    moduleInfo.codeSize = spirv.size_bytes();
    moduleInfo.pCode = spirv.data();
    VkShaderModule module = VK_NULL_HANDLE;
    check(vkCreateShaderModule(m_context->device(), &moduleInfo, nullptr, &module));
    return module;
}

void Pipeline::bind(VkCommandBuffer commandBuffer) const
{
    vkCmdBindPipeline(commandBuffer, m_bindPoint, m_pipeline);
}

void Pipeline::pushDescriptors(VkCommandBuffer commandBuffer,
                               std::span<const VkWriteDescriptorSet> writes) const
{
    m_context->functions().cmdPushDescriptorSet(commandBuffer, m_bindPoint, m_layout, 0,
                                                static_cast<std::uint32_t>(writes.size()),
                                                writes.data());
}

void Pipeline::pushConstants(VkCommandBuffer commandBuffer, const void* data,
                             std::uint32_t size) const
{
    vkCmdPushConstants(commandBuffer, m_layout, m_stages, 0, size, data);
}

// ComputePipeline ---------------------------------------------------------------------------------

ComputePipeline::ComputePipeline(Context& context, std::span<const std::uint32_t> spirv,
                                 std::span<const VkDescriptorType> bindings,
                                 std::uint32_t pushConstantSize)
    : Pipeline(context, VK_PIPELINE_BIND_POINT_COMPUTE, VK_SHADER_STAGE_COMPUTE_BIT, bindings,
               pushConstantSize)
{
    const VkShaderModule module = createShaderModule(spirv);
    VkComputePipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipelineInfo.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    pipelineInfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    pipelineInfo.stage.module = module;
    pipelineInfo.stage.pName = "main";
    pipelineInfo.layout = m_layout;
    const VkResult result = vkCreateComputePipelines(context.device(), VK_NULL_HANDLE, 1,
                                                     &pipelineInfo, nullptr, &m_pipeline);
    vkDestroyShaderModule(context.device(), module, nullptr);
    check(result);
}

// GraphicsPipeline --------------------------------------------------------------------------------

GraphicsPipeline::GraphicsPipeline(Context& context, const GraphicsPipelineConfig& config)
    : Pipeline(context, VK_PIPELINE_BIND_POINT_GRAPHICS,
               VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, config.bindings,
               config.pushConstantSize)
{
    const VkShaderModule vertex = createShaderModule(config.vertexSpirv);
    const VkShaderModule fragment = createShaderModule(config.fragmentSpirv);
    VkPipelineShaderStageCreateInfo stages[2]{};
    for (VkPipelineShaderStageCreateInfo& stage : stages) {
        stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stage.pName = "main";
    }
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertex;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fragment;

    VkPipelineVertexInputStateCreateInfo vertexInput{};
    vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
    inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo viewport{};
    viewport.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport.viewportCount = 1;
    viewport.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rasterization{};
    rasterization.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterization.polygonMode = VK_POLYGON_MODE_FILL;
    rasterization.cullMode = VK_CULL_MODE_NONE;
    rasterization.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterization.lineWidth = 1.0F;
    VkPipelineMultisampleStateCreateInfo multisample{};
    multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo depthStencil{};
    depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencil.depthTestEnable = config.depthFormat != VK_FORMAT_UNDEFINED ? VK_TRUE : VK_FALSE;
    depthStencil.depthWriteEnable = config.depthWrite ? VK_TRUE : VK_FALSE;
    depthStencil.depthCompareOp = config.depthCompare;

    std::vector<VkPipelineColorBlendAttachmentState> blendAttachments(
        config.colorFormats.size());
    for (VkPipelineColorBlendAttachmentState& attachment : blendAttachments) {
        attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                    VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    }
    VkPipelineColorBlendStateCreateInfo colorBlend{};
    colorBlend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlend.attachmentCount = static_cast<std::uint32_t>(blendAttachments.size());
    colorBlend.pAttachments = blendAttachments.data();

    const VkDynamicState dynamicStates[] = {VK_DYNAMIC_STATE_VIEWPORT,
                                            VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{};
    dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic.dynamicStateCount = static_cast<std::uint32_t>(std::size(dynamicStates));
    dynamic.pDynamicStates = dynamicStates;

    VkPipelineRenderingCreateInfo rendering{};
    rendering.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    rendering.colorAttachmentCount = static_cast<std::uint32_t>(config.colorFormats.size());
    rendering.pColorAttachmentFormats = config.colorFormats.data();
    rendering.depthAttachmentFormat = config.depthFormat;

    VkGraphicsPipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineInfo.pNext = &rendering;
    pipelineInfo.stageCount = 2;
    pipelineInfo.pStages = stages;
    pipelineInfo.pVertexInputState = &vertexInput;
    pipelineInfo.pInputAssemblyState = &inputAssembly;
    pipelineInfo.pViewportState = &viewport;
    pipelineInfo.pRasterizationState = &rasterization;
    pipelineInfo.pMultisampleState = &multisample;
    pipelineInfo.pDepthStencilState = &depthStencil;
    pipelineInfo.pColorBlendState = &colorBlend;
    pipelineInfo.pDynamicState = &dynamic;
    pipelineInfo.layout = m_layout;
    const VkResult result = vkCreateGraphicsPipelines(context.device(), VK_NULL_HANDLE, 1,
                                                      &pipelineInfo, nullptr, &m_pipeline);
    vkDestroyShaderModule(context.device(), vertex, nullptr);
    vkDestroyShaderModule(context.device(), fragment, nullptr);
    check(result);
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

auto DescriptorWrites::buffer(std::uint32_t binding, VkBuffer buffer, VkDescriptorType type)
    -> DescriptorWrites&
{
    SC_VERIFY(m_buffers.size() < m_buffers.capacity(), "DescriptorWrites capacity exceeded");
    m_buffers.push_back({buffer, 0, VK_WHOLE_SIZE});
    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstBinding = binding;
    write.descriptorCount = 1;
    write.descriptorType = type;
    write.pBufferInfo = &m_buffers.back();
    m_writes.push_back(write);
    return *this;
}

auto DescriptorWrites::storageBuffer(std::uint32_t binding, VkBuffer buffer)
    -> DescriptorWrites&
{
    return this->buffer(binding, buffer, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
}

auto DescriptorWrites::uniformBuffer(std::uint32_t binding, VkBuffer buffer)
    -> DescriptorWrites&
{
    return this->buffer(binding, buffer, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
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
