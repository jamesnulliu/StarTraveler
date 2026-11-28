#include "raster_renderer.hpp"
#include "starcore/vulkan/context.hpp"

namespace starcore::gpu
{

namespace
{

#include "shaders/raster.comp.spv.h"

// Bindings of raster.comp (0 unused: the TLAS of the ray-traced passes).
constexpr VkDescriptorType kBuffer = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
constexpr VkDescriptorType kImage = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
constexpr VkDescriptorType kUniform = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
constexpr VkDescriptorType kShadeBindings[] = {
    kBuffer, kUniform, kBuffer, kImage, kBuffer, kBuffer,  kBuffer,
    kImage,  kImage,   kImage,  kImage, kImage,  kBuffer,  kUniform,
    VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER};  // 13, 14: shadows.glsl.

}  // namespace

RasterRenderer::RasterRenderer(vulkan::Context& context)
    : m_context(&context), m_shade(context, raster_comp_spv, kShadeBindings), m_shadows(context)
{
}

void RasterRenderer::resize(std::uint32_t width, std::uint32_t height)
{
    m_motion = {};
    if (width == 0 || height == 0) {
        return;
    }
    m_motion = vulkan::Image(*m_context, width, height, VK_FORMAT_R32G32B32A32_SFLOAT,
                             VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
    m_context->submitAndWait([&](VkCommandBuffer commandBuffer) {
        vulkan::transitionImage(commandBuffer, m_motion.handle(), VK_IMAGE_LAYOUT_UNDEFINED,
                                VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0,
                                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
    });
}

void RasterRenderer::record(VkCommandBuffer commandBuffer, const RasterFrame& frame,
                            const GpuScene& scene, VisibilityPass& visibility,
                            const vulkan::Image& accumulation, const vulkan::Image& radiance,
                            const vulkan::Image& entityIds)
{
    const std::uint32_t width = radiance.width();
    const std::uint32_t height = radiance.height();
    const PathTracerSettings& settings = frame.settings;

    FrameUniforms uniforms{};
    cameraBasis(*frame.camera, uniforms.cameraPosition, uniforms.cameraForward,
                uniforms.cameraRight, uniforms.cameraUp);
    cameraBasis(*frame.previousCamera, uniforms.previousPosition, uniforms.previousForward,
                uniforms.previousRight, uniforms.previousUp);
    uniforms.cameraPosition.w = kNearPlane;
    splitAddress(scene.skyAddress(), uniforms.sky[0], uniforms.sky[1]);
    // A frame on its own samples pixel centers; averaged ones spread over the pixel.
    // Upscaled: every frame on its own, each at its own sample position.
    const bool alone = frame.jittered || frame.accumulated == 0;
    const Vec2 sample = frame.jittered            ? jitter(frame.frameIndex)
                        : frame.accumulated == 0 ? Vec2(0.5F)
                                                 : jitter(frame.accumulated);
    uniforms.viewport =
        Vec4(static_cast<float>(width), static_cast<float>(height), sample.x, sample.y);
    // Turns the soft shadows' taps.
    uniforms.frame[0] = frame.jittered ? frame.frameIndex : frame.accumulated;
    uniforms.options[0] = frame.writeIds ? kFlagWriteIds : 0U;
    if (settings.directOnly) {
        uniforms.options[0] |= kFlagDirectOnly;
    }
    uniforms.options[1] = settings.instanceMask & kAllInstances;
    uniforms.options[2] = scene.lightCount();
    uniforms.history[1] = alone ? kHistoryReset : 0U;
    vulkan::Buffer& uniformBuffer = m_uniforms[frame.frameSlot];
    writeUniforms(*m_context, uniformBuffer, uniforms);

    m_shadows.record(commandBuffer, frame.frameSlot, *frame.scene, scene, *frame.camera,
                     settings.instanceMask, frame.shadows);
    visibility.record(commandBuffer, uniformBuffer.handle(), scene, *frame.scene,
                      settings.instanceMask, GlassLayer::ThinTransmissive);

    vulkan::DescriptorWrites writes;
    writes.uniformBuffer(1, uniformBuffer.handle())
        .storageBuffer(2, scene.transformBuffer())
        .storageImage(3, visibility.visibility().view())
        .storageBuffer(4, scene.instanceBuffer())
        .storageBuffer(5, scene.materialBuffer())
        .storageBuffer(6, scene.lightBuffer())
        .storageImage(7, visibility.glassVisibility().view())
        .storageImage(8, accumulation.view())
        .storageImage(9, radiance.view())
        .storageImage(10, entityIds.view())
        .storageImage(11, m_motion.view())
        .storageBuffer(12, scene.previousTransformBuffer())
        .uniformBuffer(13, m_shadows.records(frame.frameSlot))
        .sampledImage(14, m_shadows.atlas(), m_shadows.sampler());
    m_shade.bind(commandBuffer);
    m_shade.pushDescriptors(commandBuffer, writes.writes());
    vkCmdDispatch(commandBuffer, (width + kWorkgroupSize - 1) / kWorkgroupSize,
                  (height + kWorkgroupSize - 1) / kWorkgroupSize, 1);
}

}  // namespace starcore::gpu
