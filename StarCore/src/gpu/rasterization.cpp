#include <cmath>
#include <span>

#include <glm/trigonometric.hpp>

#include "rasterization.hpp"
#include "starcore/vulkan/context.hpp"

namespace starcore::gpu
{

namespace
{

#include "shaders/visibility.frag.spv.h"
#include "shaders/visibility.vert.spv.h"
#include "shaders/visibility_sphere.frag.spv.h"

constexpr VkFormat kDepthFormat = VK_FORMAT_D32_SFLOAT;
constexpr VkFormat kVisibilityFormat = VK_FORMAT_R32G32_UINT;
constexpr std::uint32_t kJitterPeriod = 4096;

// Bindings of the visibility shaders (0 unused: the TLAS of the compute passes).
constexpr VkDescriptorType kBuffer = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
constexpr VkDescriptorType kVisibilityBindings[] = {
    kBuffer, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, kBuffer, kBuffer, kBuffer, kBuffer, kBuffer};

/** @brief Radical inverse of `index` in `base` (Halton sequence), in [0, 1). */
auto halton(std::uint32_t index, std::uint32_t base) -> float
{
    float scale = 1.0F;
    float result = 0.0F;
    while (index > 0) {
        scale /= static_cast<float>(base);
        result += scale * static_cast<float>(index % base);
        index /= base;
    }
    return result;
}

auto inGlassLayer(const RenderMaterial& m, GlassLayer glass) -> bool
{
    if ((m.flags & kMaterialThinWalled) == 0U) {
        return false;
    }
    if (glass == GlassLayer::ThinTransmissive) {
        return m.transmission > 0.0F;  // isThinTransmissive in scene.glsl.
    }
    // isGlassLayer in hybrid.glsl.
    return m.transmission * (1.0F - m.metallic) >= 0.5F && m.roughness <= 0.1F;
}

auto pipelineConfig(std::span<const std::uint32_t> fragmentSpirv)
    -> vulkan::GraphicsPipelineConfig
{
    return {.vertexSpirv = visibility_vert_spv,
            .fragmentSpirv = fragmentSpirv,
            .bindings = kVisibilityBindings,
            .pushConstantSize = sizeof(std::uint32_t),
            .colorFormats = std::span(&kVisibilityFormat, 1),
            .depthFormat = kDepthFormat};
}

}  // namespace

void cameraBasis(const Camera& camera, Vec4& position, Vec4& forward, Vec4& right, Vec4& up)
{
    const float tanHalfFov = std::tan(glm::radians(camera.verticalFov()) * 0.5F);
    position = Vec4(camera.position(), 0.0F);
    forward = Vec4(camera.forward(), 0.0F);
    right = Vec4(camera.right() * (camera.aspectRatio() * tanHalfFov), 0.0F);
    up = Vec4(camera.up() * tanHalfFov, 0.0F);
}

auto jitter(std::uint32_t frameIndex) -> Vec2
{
    const std::uint32_t index = frameIndex % kJitterPeriod + 1;
    return {halton(index, 2), halton(index, 3)};
}

void writeUniforms(vulkan::Context& context, vulkan::Buffer& buffer,
                   const FrameUniforms& uniforms)
{
    if (!buffer.isValid()) {
        // The slot's previous submission completed: its buffer may be rewritten.
        buffer = vulkan::Buffer(context, sizeof(uniforms), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                                vulkan::MemoryUsage::Upload);
    }
    buffer.write(std::as_bytes(std::span(&uniforms, 1)));
}

VisibilityPass::VisibilityPass(vulkan::Context& context)
    : m_context(&context), m_triangles(context, pipelineConfig(visibility_frag_spv)),
      m_spheres(context, pipelineConfig(visibility_sphere_frag_spv))
{
}

void VisibilityPass::resize(std::uint32_t width, std::uint32_t height)
{
    m_depth = {};
    m_glassDepth = {};
    m_visibility = {};
    m_glassVisibility = {};
    if (width == 0 || height == 0) {
        return;
    }
    m_depth = vulkan::Image(*m_context, width, height, kDepthFormat,
                            VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                                VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    m_glassDepth = vulkan::Image(*m_context, width, height, kDepthFormat,
                                 VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                                     VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    for (vulkan::Image* visibility : {&m_visibility, &m_glassVisibility}) {
        *visibility = vulkan::Image(*m_context, width, height, kVisibilityFormat,
                                    VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                        VK_IMAGE_USAGE_STORAGE_BIT);
    }
    // GENERAL from now on.
    m_context->submitAndWait([&](VkCommandBuffer commandBuffer) {
        for (const vulkan::Image* image :
             {&m_depth, &m_glassDepth, &m_visibility, &m_glassVisibility}) {
            vulkan::transitionImage(commandBuffer, image->handle(), VK_IMAGE_LAYOUT_UNDEFINED,
                                    VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0,
                                    VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                    VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
                                    image->aspect());
        }
    });
}

void VisibilityPass::record(VkCommandBuffer commandBuffer, VkBuffer uniforms,
                            const GpuScene& scene, const RenderScene& renderScene,
                            std::uint32_t instanceMask, GlassLayer glass)
{
    draw(commandBuffer, uniforms, scene, renderScene, instanceMask, glass, false);
    vulkan::memoryBarrier(commandBuffer, VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                          VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                          VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    VkImageCopy copy{};
    copy.srcSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
    copy.dstSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
    copy.extent = {m_depth.width(), m_depth.height(), 1};
    vkCmdCopyImage(commandBuffer, m_depth.handle(), VK_IMAGE_LAYOUT_GENERAL,
                   m_glassDepth.handle(), VK_IMAGE_LAYOUT_GENERAL, 1, &copy);
    vulkan::memoryBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                          VK_ACCESS_TRANSFER_WRITE_BIT,
                          VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                              VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                          VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                              VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
    draw(commandBuffer, uniforms, scene, renderScene, instanceMask, glass, true);
    vulkan::memoryBarrier(commandBuffer, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                          VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                          VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
}

void VisibilityPass::draw(VkCommandBuffer commandBuffer, VkBuffer uniforms,
                          const GpuScene& scene, const RenderScene& renderScene,
                          std::uint32_t instanceMask, GlassLayer glass, bool glassLayer)
{
    const std::uint32_t width = m_visibility.width();
    const std::uint32_t height = m_visibility.height();

    VkRenderingAttachmentInfo color{};
    color.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    color.imageView = (glassLayer ? m_glassVisibility : m_visibility).view();
    color.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingAttachmentInfo depth{};
    depth.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    depth.imageView = (glassLayer ? m_glassDepth : m_depth).view();
    depth.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    // The glass layer is tested against (a copy of) the primary surfaces' depth.
    depth.loadOp = glassLayer ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_CLEAR;
    depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    depth.clearValue.depthStencil = {0.0F, 0};  // Reversed depth: 0 is infinitely far.
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
    writes.uniformBuffer(1, uniforms)
        .storageBuffer(2, scene.transformBuffer())
        .storageBuffer(4, scene.instanceBuffer());
    for (const vulkan::GraphicsPipeline* pipeline : {&m_triangles, &m_spheres}) {
        const bool spheres = pipeline == &m_spheres;
        bool bound = false;
        for (std::uint32_t i = 0; i < renderScene.instances.size(); ++i) {
            const RenderInstance& instance = renderScene.instances[i];
            const RenderGeometry& geometry = renderScene.geometries[instance.geometryIndex];
            if ((instance.mask & instanceMask) == 0 ||
                (geometry.kind == GeometryKind::Sphere) != spheres ||
                inGlassLayer(renderScene.materials[instance.materialIndex], glass) != glassLayer) {
                continue;
            }
            const auto vertexCount =
                spheres ? 36U
                        : static_cast<std::uint32_t>(geometry.mesh ? 3 * geometry.mesh->triangleCount()
                                                                   : 0);
            if (vertexCount == 0) {
                continue;
            }
            if (!bound) {
                pipeline->bind(commandBuffer);
                pipeline->pushDescriptors(commandBuffer, writes.writes());
                bound = true;
            }
            pipeline->pushConstants(commandBuffer, &i, sizeof(i));
            vkCmdDraw(commandBuffer, vertexCount, 1, 0, 0);
        }
    }
    vkCmdEndRendering(commandBuffer);
}

}  // namespace starcore::gpu
