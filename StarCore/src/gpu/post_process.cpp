module;

#include <vulkan/vulkan.h>

module starcore.gpu;

import std;
import starcore.math;
import starcore.vulkan;
import :post_process;

namespace starcore::gpu
{

namespace
{

#include "shaders/bloom_down.comp.spv.h"
#include "shaders/bloom_prefilter.comp.spv.h"
#include "shaders/bloom_up.comp.spv.h"
#include "shaders/motion_neighbors.comp.spv.h"
#include "shaders/motion_tiles.comp.spv.h"
#include "shaders/particle.frag.spv.h"
#include "shaders/particle.vert.spv.h"
#include "shaders/post.comp.spv.h"

constexpr VkFormat kParticleFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
constexpr VkFormat kBloomFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
constexpr VkFormat kDepthFormat = VK_FORMAT_D32_SFLOAT;  // VisibilityPass's.
constexpr std::uint32_t kMaxBloomLevels = 6;
constexpr std::uint32_t kMinBloomSize = 8;  // Pixels of the smallest level.
constexpr std::uint32_t kMotionTile = 16;   // motion_blur.glsl.
// The reconstruction filter looks one tile around: larger blurs would be cut.
constexpr float kMaxBlurRadius = 1.5F * static_cast<float>(kMotionTile);

constexpr VkDescriptorType kBuffer = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
constexpr VkDescriptorType kImage = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
constexpr VkDescriptorType kSampled = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
// particle.vert: 1 frame uniforms, 3 particles (0, 2 unused).
constexpr VkDescriptorType kParticleBindings[] = {kBuffer, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                                                  kBuffer, kBuffer};
constexpr VkDescriptorType kPrefilterBindings[] = {kImage, kImage, kImage};
constexpr VkDescriptorType kResampleBindings[] = {kSampled, kImage};
constexpr VkDescriptorType kTileBindings[] = {kImage, kImage};
constexpr VkDescriptorType kFinalBindings[] = {kImage, kImage, kImage, kSampled, kImage, kImage};

/** @brief Push constants of bloom_prefilter.comp. */
struct PrefilterParams
{
    float exposure = 1.0F;
    std::uint32_t particles = 0;
};

/** @brief Push constants of motion_tiles.comp. */
struct TileParams
{
    float shutter = 0.0F;
    float maxRadius = 0.0F;
};

/** @brief Push constants of post.comp. */
struct FinalParams
{
    float exposure = 1.0F;
    std::uint32_t toneMapper = 0;
    std::uint32_t flags = 0;
    float bloomStrength = 0.0F;
    float bloomScale = 1.0F;
    float shutter = 0.0F;
    float maxRadius = 0.0F;
    float vignette = 0.0F;
};

auto blurRadius(const EffectSettings& effects) -> float
{
    return std::clamp(0.5F * effects.maxBlur, 0.5F, kMaxBlurRadius);
}

// post.comp flags.
constexpr std::uint32_t kPostRaw = 1U;
constexpr std::uint32_t kPostSrgb = 2U;
constexpr std::uint32_t kPostParticles = 4U;
constexpr std::uint32_t kPostMotionBlur = 8U;
constexpr std::uint32_t kPostBloom = 16U;

auto groups(std::uint32_t size) -> std::uint32_t
{
    return (size + kWorkgroupSize - 1) / kWorkgroupSize;
}

void computeBarrier(VkCommandBuffer commandBuffer)
{
    vulkan::memoryBarrier(commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                          VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                          VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
}

}  // namespace

PostProcess::PostProcess(vulkan::Context& context)
    : m_context(&context),
      m_particlePipeline(context, {.vertexSpirv = particle_vert_spv,
                                   .fragmentSpirv = particle_frag_spv,
                                   .bindings = kParticleBindings,
                                   .pushConstantSize = sizeof(float),
                                   .colorFormats = std::span(&kParticleFormat, 1),
                                   .depthFormat = kDepthFormat,
                                   .depthCompare = VK_COMPARE_OP_GREATER_OR_EQUAL,
                                   .depthWrite = false,
                                   .additiveBlending = true}),
      m_bloomPrefilter(context, bloom_prefilter_comp_spv, kPrefilterBindings,
                       static_cast<std::uint32_t>(sizeof(PrefilterParams))),
      m_bloomDown(context, bloom_down_comp_spv, kResampleBindings),
      m_bloomUp(context, bloom_up_comp_spv, kResampleBindings),
      m_motionTiles(context, motion_tiles_comp_spv, kTileBindings,
                    static_cast<std::uint32_t>(sizeof(TileParams))),
      m_motionNeighbors(context, motion_neighbors_comp_spv, kTileBindings),
      m_final(context, post_comp_spv, kFinalBindings,
              static_cast<std::uint32_t>(sizeof(FinalParams))),
      m_sampler(context)
{
}

void PostProcess::resize(std::uint32_t width, std::uint32_t height)
{
    m_particles = {};
    m_bloom.clear();
    m_tileBlur = {};
    m_neighborBlur = {};
    if (width == 0 || height == 0) {
        return;
    }
    constexpr VkImageUsageFlags kReadWrite = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    m_particles = vulkan::Image(*m_context, width, height, kParticleFormat,
                                kReadWrite | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
    const std::uint32_t tilesX = (width + kMotionTile - 1) / kMotionTile;
    const std::uint32_t tilesY = (height + kMotionTile - 1) / kMotionTile;
    for (vulkan::Image* tiles : {&m_tileBlur, &m_neighborBlur}) {
        *tiles = vulkan::Image(*m_context, tilesX, tilesY, VK_FORMAT_R32G32B32A32_SFLOAT,
                               VK_IMAGE_USAGE_STORAGE_BIT);
    }
    for (std::uint32_t level = 0; level < kMaxBloomLevels; ++level) {
        const std::uint32_t levelWidth = width >> (level + 1);
        const std::uint32_t levelHeight = height >> (level + 1);
        if (std::min(levelWidth, levelHeight) < kMinBloomSize) {
            break;
        }
        m_bloom.emplace_back(*m_context, levelWidth, levelHeight, kBloomFormat, kReadWrite);
    }
    // GENERAL from now on.
    m_context->submitAndWait([&](VkCommandBuffer commandBuffer) {
        auto toGeneral = [&](const vulkan::Image& image) {
            vulkan::transitionImage(commandBuffer, image.handle(), VK_IMAGE_LAYOUT_UNDEFINED,
                                    VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0,
                                    VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                    VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);
        };
        toGeneral(m_particles);
        toGeneral(m_tileBlur);
        toGeneral(m_neighborBlur);
        for (const vulkan::Image& level : m_bloom) {
            toGeneral(level);
        }
    });
}

auto PostProcess::drawParticles(VkCommandBuffer commandBuffer, const PostFrame& frame) -> bool
{
    if (frame.particles.empty() || frame.depth == nullptr || frame.raw) {
        return false;
    }
    const std::uint32_t width = m_particles.width();
    const std::uint32_t height = m_particles.height();

    FrameUniforms uniforms{};
    cameraBasis(*frame.camera, uniforms.cameraPosition, uniforms.cameraForward,
                uniforms.cameraRight, uniforms.cameraUp);
    cameraBasis(*frame.previousCamera, uniforms.previousPosition, uniforms.previousForward,
                uniforms.previousRight, uniforms.previousUp);
    uniforms.cameraPosition.w = kNearPlane;
    uniforms.viewport = Vec4(static_cast<float>(width), static_cast<float>(height), 0.5F, 0.5F);
    writeUniforms(*m_context, m_uniforms[frame.frameSlot], uniforms);

    // The slot's previous submission completed: its buffer may be rewritten.
    const auto bytes = std::as_bytes(frame.particles);
    vulkan::Buffer& buffer = m_particleBuffers[frame.frameSlot];
    if (!buffer.isValid() || buffer.size() < bytes.size()) {
        buffer = vulkan::Buffer(*m_context, std::max<VkDeviceSize>(bytes.size(), buffer.size() * 2),
                                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, vulkan::MemoryUsage::Upload);
    }
    buffer.write(bytes);

    // The visibility pass wrote the depth (and copied it).
    vulkan::memoryBarrier(commandBuffer,
                          VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                          VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                          VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                              VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                          VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT);

    VkRenderingAttachmentInfo color{};
    color.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    color.imageView = m_particles.view();
    color.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingAttachmentInfo depth{};
    depth.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    depth.imageView = frame.depth->view();
    depth.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    depth.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    depth.storeOp = VK_ATTACHMENT_STORE_OP_NONE;  // Only tested.
    VkRenderingInfo rendering{};
    rendering.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    rendering.renderArea = {{0, 0}, {width, height}};
    rendering.layerCount = 1;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachments = &color;
    rendering.pDepthAttachment = &depth;
    vkCmdBeginRendering(commandBuffer, &rendering);
    const VkViewport viewport{0.0F, 0.0F, static_cast<float>(width), static_cast<float>(height),
                              0.0F, 1.0F};
    const VkRect2D scissor{{0, 0}, {width, height}};
    vkCmdSetViewport(commandBuffer, 0, 1, &viewport);
    vkCmdSetScissor(commandBuffer, 0, 1, &scissor);

    vulkan::DescriptorWrites writes;
    writes.uniformBuffer(1, m_uniforms[frame.frameSlot].handle()).storageBuffer(3, buffer.handle());
    m_particlePipeline.bind(commandBuffer);
    m_particlePipeline.pushDescriptors(commandBuffer, writes.writes());
    const float shutter = frame.effects.motionBlur ? frame.effects.shutter : 0.0F;
    m_particlePipeline.pushConstants(commandBuffer, &shutter, sizeof(shutter));
    vkCmdDraw(commandBuffer, static_cast<std::uint32_t>(6 * frame.particles.size()), 1, 0, 0);
    vkCmdEndRendering(commandBuffer);

    vulkan::memoryBarrier(commandBuffer, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                          VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                          VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
    return true;
}

void PostProcess::recordBloom(VkCommandBuffer commandBuffer, const PostFrame& frame,
                              const vulkan::Image& radiance, bool particles)
{
    {
        vulkan::DescriptorWrites writes;
        writes.storageImage(0, radiance.view())
            .storageImage(1, m_particles.view())
            .storageImage(2, m_bloom[0].view());
        const PrefilterParams params{.exposure = frame.settings.exposure,
                                     .particles = particles ? 1U : 0U};
        m_bloomPrefilter.bind(commandBuffer);
        m_bloomPrefilter.pushDescriptors(commandBuffer, writes.writes());
        m_bloomPrefilter.pushConstants(commandBuffer, &params, sizeof(params));
        vkCmdDispatch(commandBuffer, groups(m_bloom[0].width()), groups(m_bloom[0].height()), 1);
        computeBarrier(commandBuffer);
    }
    auto resample = [&](const vulkan::ComputePipeline& pipeline, const vulkan::Image& source,
                        const vulkan::Image& target) {
        vulkan::DescriptorWrites writes;
        writes.sampledImage(0, source.view(), m_sampler.handle()).storageImage(1, target.view());
        pipeline.bind(commandBuffer);
        pipeline.pushDescriptors(commandBuffer, writes.writes());
        vkCmdDispatch(commandBuffer, groups(target.width()), groups(target.height()), 1);
        computeBarrier(commandBuffer);
    };
    for (std::size_t level = 1; level < m_bloom.size(); ++level) {
        resample(m_bloomDown, m_bloom[level - 1], m_bloom[level]);
    }
    for (std::size_t level = m_bloom.size() - 1; level-- > 0;) {
        resample(m_bloomUp, m_bloom[level + 1], m_bloom[level]);
    }
}

void PostProcess::recordMotionTiles(VkCommandBuffer commandBuffer, const PostFrame& frame)
{
    {
        vulkan::DescriptorWrites writes;
        writes.storageImage(0, frame.motion->view()).storageImage(1, m_tileBlur.view());
        const TileParams params{.shutter = frame.effects.shutter,
                                .maxRadius = blurRadius(frame.effects)};
        m_motionTiles.bind(commandBuffer);
        m_motionTiles.pushDescriptors(commandBuffer, writes.writes());
        m_motionTiles.pushConstants(commandBuffer, &params, sizeof(params));
        vkCmdDispatch(commandBuffer, groups(m_tileBlur.width()), groups(m_tileBlur.height()), 1);
        computeBarrier(commandBuffer);
    }
    vulkan::DescriptorWrites writes;
    writes.storageImage(0, m_tileBlur.view()).storageImage(1, m_neighborBlur.view());
    m_motionNeighbors.bind(commandBuffer);
    m_motionNeighbors.pushDescriptors(commandBuffer, writes.writes());
    vkCmdDispatch(commandBuffer, groups(m_neighborBlur.width()), groups(m_neighborBlur.height()),
                  1);
    computeBarrier(commandBuffer);
}

void PostProcess::record(VkCommandBuffer commandBuffer, const PostFrame& frame,
                         const vulkan::Image& radiance, const vulkan::Image& output)
{
    const EffectSettings& effects = frame.effects;
    const bool particles = drawParticles(commandBuffer, frame);
    // The pipeline's radiance and motion vectors.
    computeBarrier(commandBuffer);
    const bool bloom = effects.bloom && !frame.raw && !m_bloom.empty();
    if (bloom) {
        recordBloom(commandBuffer, frame, radiance, particles);
    }
    const bool motionBlur = effects.motionBlur && !frame.raw && frame.motion != nullptr;
    if (motionBlur) {
        recordMotionTiles(commandBuffer, frame);
    }

    FinalParams params{
        .exposure = frame.settings.exposure,
        .toneMapper = static_cast<std::uint32_t>(frame.settings.toneMapper),
        .bloomStrength = effects.bloomStrength,
        .bloomScale = m_bloom.empty() ? 1.0F : 1.0F / static_cast<float>(m_bloom.size()),
        .shutter = effects.shutter,
        .maxRadius = blurRadius(effects),
        .vignette = frame.raw ? 0.0F : std::clamp(effects.vignette, 0.0F, 1.0F),
    };
    params.flags = (frame.raw ? kPostRaw : 0U) | (frame.settings.srgbOutput ? kPostSrgb : 0U) |
                   (particles ? kPostParticles : 0U) | (motionBlur ? kPostMotionBlur : 0U) |
                   (bloom ? kPostBloom : 0U);
    // Unused inputs still need valid descriptors of the right kind.
    vulkan::DescriptorWrites writes;
    writes.storageImage(0, radiance.view())
        .storageImage(1, m_particles.view())
        .storageImage(2, (motionBlur ? *frame.motion : radiance).view())
        .sampledImage(3, (m_bloom.empty() ? m_particles : m_bloom[0]).view(), m_sampler.handle())
        .storageImage(4, output.view())
        .storageImage(5, m_neighborBlur.view());
    m_final.bind(commandBuffer);
    m_final.pushDescriptors(commandBuffer, writes.writes());
    m_final.pushConstants(commandBuffer, &params, sizeof(params));
    vkCmdDispatch(commandBuffer, groups(output.width()), groups(output.height()), 1);
}

}  // namespace starcore::gpu
