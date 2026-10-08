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
constexpr VkDescriptorType kShadeBindings[] = {
    kBuffer, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, kBuffer, kImage, kBuffer, kBuffer,
    kBuffer, kImage,                            kImage,  kImage, kImage};

}  // namespace

RasterRenderer::RasterRenderer(vulkan::Context& context)
    : m_context(&context), m_shade(context, raster_comp_spv, kShadeBindings)
{
}

void RasterRenderer::record(VkCommandBuffer commandBuffer, const RasterFrame& frame,
                            const GpuScene& scene, VisibilityPass& visibility,
                            const vulkan::Image& accumulation, const vulkan::Image& output,
                            const vulkan::Image& entityIds)
{
    const std::uint32_t width = output.width();
    const std::uint32_t height = output.height();
    const PathTracerSettings& settings = frame.settings;

    FrameUniforms uniforms{};
    cameraBasis(*frame.camera, uniforms.cameraPosition, uniforms.cameraForward,
                uniforms.cameraRight, uniforms.cameraUp);
    uniforms.cameraPosition.w = kNearPlane;
    uniforms.cameraRight.w = settings.exposure;
    uniforms.skyColor = Vec4(frame.scene->skyColor, 0.0F);
    // A frame on its own samples pixel centers; averaged ones spread over the pixel.
    const Vec2 sample = frame.accumulated == 0 ? Vec2(0.5F) : jitter(frame.accumulated);
    uniforms.viewport =
        Vec4(static_cast<float>(width), static_cast<float>(height), sample.x, sample.y);
    uniforms.options[0] =
        (settings.srgbOutput ? kFlagSrgb : 0U) | (frame.writeIds ? kFlagWriteIds : 0U);
    uniforms.options[1] = settings.instanceMask & kAllInstances;
    uniforms.options[2] = scene.lightCount();
    uniforms.options[3] = static_cast<std::uint32_t>(settings.toneMapper);
    uniforms.history[1] = frame.accumulated == 0 ? kHistoryReset : 0U;
    vulkan::Buffer& uniformBuffer = m_uniforms[frame.frameSlot];
    writeUniforms(*m_context, uniformBuffer, uniforms);

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
        .storageImage(9, output.view())
        .storageImage(10, entityIds.view());
    m_shade.bind(commandBuffer);
    m_shade.pushDescriptors(commandBuffer, writes.writes());
    vkCmdDispatch(commandBuffer, (width + kWorkgroupSize - 1) / kWorkgroupSize,
                  (height + kWorkgroupSize - 1) / kWorkgroupSize, 1);
}

}  // namespace starcore::gpu
