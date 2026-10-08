#include <algorithm>
#include <cmath>
#include <span>

#include <glm/trigonometric.hpp>

#include "hybrid_renderer.hpp"
#include "starcore/vulkan/check.hpp"
#include "starcore/vulkan/context.hpp"

namespace starcore::gpu
{

namespace
{

#include "shaders/atrous.comp.spv.h"
#include "shaders/composite.comp.spv.h"
#include "shaders/hybrid_trace.comp.spv.h"
#include "shaders/temporal.comp.spv.h"
#include "shaders/visibility.frag.spv.h"
#include "shaders/visibility.vert.spv.h"
#include "shaders/visibility_sphere.frag.spv.h"

constexpr std::uint32_t kWorkgroupSize = 8;  // local_size of the compute passes.
constexpr float kNearPlane = 0.01F;          // Of the visibility pass (reversed depth).
constexpr VkFormat kDepthFormat = VK_FORMAT_D32_SFLOAT;
constexpr VkFormat kVisibilityFormat = VK_FORMAT_R32G32_UINT;
constexpr std::uint32_t kJitterPeriod = 4096;

// hybrid.glsl: options.x and history.y bits.
constexpr std::uint32_t kFlagPreview = 1U;
constexpr std::uint32_t kFlagNextEvent = 2U;
constexpr std::uint32_t kFlagSrgb = 4U;
constexpr std::uint32_t kFlagWriteIds = 8U;
constexpr std::uint32_t kHistoryReset = 1U;
constexpr std::uint32_t kHistoryStill = 2U;

// Bindings 0-2 and 4-6 are the same in every pass (hybrid.glsl, scene.glsl):
// TLAS, frame uniforms, transforms, instances, materials, lights.
constexpr VkDescriptorType kBuffer = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
constexpr VkDescriptorType kUniform = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
constexpr VkDescriptorType kImage = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
constexpr VkDescriptorType kTlas = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
constexpr VkDescriptorType kVisibilityBindings[] = {kBuffer, kUniform, kBuffer, kBuffer,
                                                    kBuffer, kBuffer,  kBuffer};
constexpr VkDescriptorType kTraceBindings[] = {
    kTlas,  kUniform, kBuffer, kImage, kBuffer, kBuffer, kBuffer, kImage, kImage, kImage,
    kImage, kImage,   kImage,  kImage, kImage,  kImage,  kImage,  kImage, kImage};
// Spatial filter iterations of the glass layer at most (radius 8 pixels).
constexpr std::uint32_t kGlassIterations = 3;
constexpr VkDescriptorType kTemporalBindings[] = {
    kBuffer, kUniform, kBuffer, kImage, kBuffer, kBuffer, kBuffer, kImage, kImage, kImage, kImage,
    kImage,  kImage,   kImage,  kImage, kImage,  kImage,  kImage,  kImage, kImage, kImage};
constexpr VkDescriptorType kAtrousBindings[] = {kBuffer, kUniform, kBuffer, kImage, kBuffer,
                                                kBuffer, kBuffer,  kImage,  kImage, kImage};

/** @brief Push constants of atrous.comp. */
struct AtrousPass
{
    std::int32_t step = 1;
    std::uint32_t firstIteration = 0;
};
constexpr VkDescriptorType kCompositeBindings[] = {
    kBuffer, kUniform, kBuffer, kImage, kBuffer, kBuffer, kBuffer, kImage,
    kImage,  kImage,   kImage,  kImage, kImage,  kImage,  kImage,  kImage};

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

/** @brief Sample position in the pixel of a frame (low discrepancy over frames). */
auto jitter(std::uint32_t frameIndex) -> Vec2
{
    const std::uint32_t index = frameIndex % kJitterPeriod + 1;
    return {halton(index, 2), halton(index, 3)};
}

/** @brief Glass layer: smooth window panes (isGlassLayer in hybrid.glsl). */
auto isGlassLayer(const RenderMaterial& m) -> bool
{
    return (m.flags & kMaterialThinWalled) != 0U && m.transmission * (1.0F - m.metallic) >= 0.5F &&
           m.roughness <= 0.1F;
}

void computeBarrier(VkCommandBuffer commandBuffer)
{
    vulkan::memoryBarrier(commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                          VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                          VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
}

void cameraBasis(const Camera& camera, Vec4& position, Vec4& forward, Vec4& right, Vec4& up)
{
    // Camera rays: forward + right * ndcX + up * ndcY (Camera::generateRay).
    const float tanHalfFov = std::tan(glm::radians(camera.verticalFov()) * 0.5F);
    position = Vec4(camera.position(), 0.0F);
    forward = Vec4(camera.forward(), 0.0F);
    right = Vec4(camera.right() * (camera.aspectRatio() * tanHalfFov), 0.0F);
    up = Vec4(camera.up() * tanHalfFov, 0.0F);
}

}  // namespace

struct HybridRenderer::Images
{
    vulkan::Image depth;
    vulkan::Image glassDepth;
    vulkan::Image visibility;  // RG32UI: instance + 1 (0: none), triangle.
    vulkan::Image glassVisibility;
    // This frame (hybrid_trace.comp).
    vulkan::Image noisyPrimary;
    vulkan::Image noisyGlass;
    vulkan::Image frameAlbedo;
    vulkan::Image frameEmission;
    vulkan::Image primaryMotion;
    vulkan::Image glassMotion;
    // RG32UI: compact guides for the a-trous taps.
    vulkan::Image primaryFilterGuide;
    vulkan::Image glassFilterGuide;
    // Ping-pong: written this frame / read as the previous one.
    std::array<vulkan::Image, 2> primaryGuide;
    std::array<vulkan::Image, 2> glassGuide;
    std::array<vulkan::Image, 2> primaryColor;
    std::array<vulkan::Image, 2> primaryAlbedo;
    std::array<vulkan::Image, 2> primaryEmission;
    std::array<vulkan::Image, 2> primaryMoments;
    std::array<vulkan::Image, 2> glassColor;
    std::array<vulkan::Image, 2> glassMoments;
    // Filter input ([0], from the temporal pass) and a-trous ping-pong.
    std::array<vulkan::Image, 2> primaryFilter;
    std::array<vulkan::Image, 2> glassFilter;

    template <typename F>
    void forEach(F&& function)
    {
        for (vulkan::Image* image :
             {&depth, &glassDepth, &visibility, &glassVisibility, &noisyPrimary, &noisyGlass,
              &frameAlbedo, &frameEmission, &primaryMotion, &glassMotion, &primaryFilterGuide,
              &glassFilterGuide}) {
            function(*image);
        }
        for (auto* pair : {&primaryGuide, &glassGuide, &primaryColor, &primaryAlbedo,
                           &primaryEmission, &primaryMoments, &glassColor, &glassMoments,
                           &primaryFilter, &glassFilter}) {
            function((*pair)[0]);
            function((*pair)[1]);
        }
    }
};

HybridRenderer::HybridRenderer(vulkan::Context& context)
    : m_context(&context),
      m_visibility(context, {.vertexSpirv = visibility_vert_spv,
                             .fragmentSpirv = visibility_frag_spv,
                             .bindings = kVisibilityBindings,
                             .pushConstantSize = sizeof(std::uint32_t),
                             .colorFormats = std::span(&kVisibilityFormat, 1),
                             .depthFormat = kDepthFormat}),
      m_visibilitySphere(context, {.vertexSpirv = visibility_vert_spv,
                                   .fragmentSpirv = visibility_sphere_frag_spv,
                                   .bindings = kVisibilityBindings,
                                   .pushConstantSize = sizeof(std::uint32_t),
                                   .colorFormats = std::span(&kVisibilityFormat, 1),
                                   .depthFormat = kDepthFormat}),
      m_trace(context, hybrid_trace_comp_spv, kTraceBindings, sizeof(std::uint32_t)),
      m_temporal(context, temporal_comp_spv, kTemporalBindings, sizeof(std::uint32_t)),
      m_atrous(context, atrous_comp_spv, kAtrousBindings, sizeof(AtrousPass)),
      m_composite(context, composite_comp_spv, kCompositeBindings)
{
}

HybridRenderer::~HybridRenderer() = default;

void HybridRenderer::resize(std::uint32_t width, std::uint32_t height)
{
    m_images.reset();
    if (width == 0 || height == 0) {
        return;
    }
    auto images = std::make_unique<Images>();
    auto storage = [&](VkFormat format) {
        return vulkan::Image(*m_context, width, height, format, VK_IMAGE_USAGE_STORAGE_BIT);
    };
    images->depth = vulkan::Image(*m_context, width, height, kDepthFormat,
                                  VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                                      VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    images->glassDepth = vulkan::Image(*m_context, width, height, kDepthFormat,
                                       VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                                           VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    for (vulkan::Image* visibility : {&images->visibility, &images->glassVisibility}) {
        *visibility = vulkan::Image(*m_context, width, height, kVisibilityFormat,
                                    VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                        VK_IMAGE_USAGE_STORAGE_BIT);
    }
    images->noisyPrimary = storage(VK_FORMAT_R32G32B32A32_SFLOAT);
    images->noisyGlass = storage(VK_FORMAT_R32G32B32A32_SFLOAT);
    images->frameAlbedo = storage(VK_FORMAT_R16G16B16A16_SFLOAT);
    images->frameEmission = storage(VK_FORMAT_R16G16B16A16_SFLOAT);
    images->primaryMotion = storage(VK_FORMAT_R32G32B32A32_SFLOAT);
    images->glassMotion = storage(VK_FORMAT_R32G32B32A32_SFLOAT);
    images->primaryFilterGuide = storage(VK_FORMAT_R32G32_UINT);
    images->glassFilterGuide = storage(VK_FORMAT_R32G32_UINT);
    for (std::size_t i = 0; i < 2; ++i) {
        images->primaryGuide[i] = storage(VK_FORMAT_R32G32B32A32_UINT);
        images->glassGuide[i] = storage(VK_FORMAT_R32G32B32A32_UINT);
        images->primaryColor[i] = storage(VK_FORMAT_R32G32B32A32_SFLOAT);
        images->primaryAlbedo[i] = storage(VK_FORMAT_R32G32B32A32_SFLOAT);
        images->primaryEmission[i] = storage(VK_FORMAT_R16G16B16A16_SFLOAT);
        images->primaryMoments[i] = storage(VK_FORMAT_R32G32_SFLOAT);
        images->glassColor[i] = storage(VK_FORMAT_R32G32B32A32_SFLOAT);
        images->glassMoments[i] = storage(VK_FORMAT_R32G32_SFLOAT);
        images->primaryFilter[i] = storage(VK_FORMAT_R16G16B16A16_SFLOAT);
        images->glassFilter[i] = storage(VK_FORMAT_R16G16B16A16_SFLOAT);
    }

    // GENERAL from now on. The contents are undefined until the first frame,
    // which resets (no history is read).
    m_context->submitAndWait([&](VkCommandBuffer commandBuffer) {
        images->forEach([&](const vulkan::Image& image) {
            vulkan::transitionImage(commandBuffer, image.handle(), VK_IMAGE_LAYOUT_UNDEFINED,
                                    VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0,
                                    VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                    VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
                                    image.aspect());
        });
    });
    m_images = std::move(images);
}

void HybridRenderer::renderVisibility(VkCommandBuffer commandBuffer, const HybridFrame& frame,
                                      const GpuScene& scene, bool glass)
{
    Images& images = *m_images;
    const std::uint32_t width = images.visibility.width();
    const std::uint32_t height = images.visibility.height();

    VkRenderingAttachmentInfo color{};
    color.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    color.imageView = (glass ? images.glassVisibility : images.visibility).view();
    color.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingAttachmentInfo depth{};
    depth.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    depth.imageView = (glass ? images.glassDepth : images.depth).view();
    depth.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    // The glass layer is tested against (a copy of) the primary surfaces' depth.
    depth.loadOp = glass ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_CLEAR;
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
    writes.uniformBuffer(1, m_uniforms[frame.frameSlot].handle())
        .storageBuffer(2, frame.transforms)
        .storageBuffer(4, scene.instanceBuffer());
    const RenderScene& renderScene = *frame.scene;
    for (const vulkan::GraphicsPipeline* pipeline : {&m_visibility, &m_visibilitySphere}) {
        const bool spheres = pipeline == &m_visibilitySphere;
        bool bound = false;
        for (std::uint32_t i = 0; i < renderScene.instances.size(); ++i) {
            const RenderInstance& instance = renderScene.instances[i];
            const RenderGeometry& geometry = renderScene.geometries[instance.geometryIndex];
            if ((instance.mask & frame.settings.instanceMask) == 0 ||
                (geometry.kind == GeometryKind::Sphere) != spheres ||
                isGlassLayer(renderScene.materials[instance.materialIndex]) != glass) {
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

void HybridRenderer::record(VkCommandBuffer commandBuffer, const HybridFrame& frame,
                            const GpuScene& scene, const vulkan::Image& output,
                            const vulkan::Image& entityIds)
{
    Images& images = *m_images;
    const std::uint32_t current = m_current;
    const std::uint32_t previous = 1 - current;
    const std::uint32_t width = output.width();
    const std::uint32_t height = output.height();
    const PathTracerSettings& settings = frame.settings;
    const DenoiserSettings& denoiser = frame.options.denoiser;
    const RenderScene& renderScene = *frame.scene;

    // Uniforms ------------------------------------------------------------------------------------
    HybridFrameUniforms uniforms{};
    cameraBasis(*frame.camera, uniforms.cameraPosition, uniforms.cameraForward,
                uniforms.cameraRight, uniforms.cameraUp);
    cameraBasis(*frame.previousCamera, uniforms.previousPosition, uniforms.previousForward,
                uniforms.previousRight, uniforms.previousUp);
    uniforms.cameraPosition.w = kNearPlane;
    uniforms.cameraForward.w = settings.fireflyClamp;
    uniforms.cameraRight.w = settings.exposure;
    uniforms.skyColor = Vec4(renderScene.skyColor, 0.0F);
    uniforms.viewport = Vec4(static_cast<float>(width), static_cast<float>(height),
                             jitter(frame.frameIndex));
    uniforms.previousJitter = Vec4(jitter(frame.frameIndex - 1), 0.0F, 0.0F);
    std::uint32_t flags = 0;
    if (settings.mode == RenderMode::Preview) {
        flags |= kFlagPreview;
    }
    if (settings.nextEventEstimation) {
        flags |= kFlagNextEvent;
    }
    if (settings.srgbOutput) {
        flags |= kFlagSrgb;
    }
    if (frame.writeIds) {
        flags |= kFlagWriteIds;
    }
    const std::uint32_t samples =
        settings.mode == RenderMode::Preview ? 1U : std::max(1U, settings.samplesPerPixel);
    uniforms.frame[0] = frame.frameIndex;
    uniforms.frame[1] = settings.seed;
    uniforms.frame[2] = samples;
    uniforms.frame[3] = settings.maxBounces;
    uniforms.options[0] = flags;
    uniforms.options[1] = settings.instanceMask & kAllInstances;
    uniforms.options[2] = scene.lightCount();
    uniforms.options[3] = static_cast<std::uint32_t>(settings.toneMapper);
    uniforms.history[0] = frame.still ? 0U : std::max(1U, denoiser.historySamples);
    uniforms.history[1] = (frame.reset ? kHistoryReset : 0U) | (frame.still ? kHistoryStill : 0U);
    uniforms.history[2] = static_cast<std::uint32_t>(frame.options.view);
    uniforms.denoiser = Vec4(denoiser.colorSigma, denoiser.normalPower, denoiser.depthSigma, 0.0F);
    vulkan::Buffer& uniformBuffer = m_uniforms[frame.frameSlot];
    if (!uniformBuffer.isValid()) {
        // The slot's previous submission completed: its buffer may be rewritten.
        uniformBuffer = vulkan::Buffer(*m_context, sizeof(uniforms),
                                       VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                                       vulkan::MemoryUsage::Upload);
    }
    uniformBuffer.write(std::as_bytes(std::span(&uniforms, 1)));

    // 1. Visibility -------------------------------------------------------------------------------
    renderVisibility(commandBuffer, frame, scene, false);
    vulkan::memoryBarrier(commandBuffer, VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                          VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                          VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    VkImageCopy copy{};
    copy.srcSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
    copy.dstSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
    copy.extent = {width, height, 1};
    vkCmdCopyImage(commandBuffer, images.depth.handle(), VK_IMAGE_LAYOUT_GENERAL,
                   images.glassDepth.handle(), VK_IMAGE_LAYOUT_GENERAL, 1, &copy);
    vulkan::memoryBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                          VK_ACCESS_TRANSFER_WRITE_BIT,
                          VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                              VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                          VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                              VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
    renderVisibility(commandBuffer, frame, scene, true);
    vulkan::memoryBarrier(commandBuffer, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                          VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                          VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);

    const std::uint32_t groupsX = (width + kWorkgroupSize - 1) / kWorkgroupSize;
    const std::uint32_t groupsY = (height + kWorkgroupSize - 1) / kWorkgroupSize;

    // 2. Trace ------------------------------------------------------------------------------------
    if (frame.queryPool != VK_NULL_HANDLE) {
        vkCmdWriteTimestamp(commandBuffer, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, frame.queryPool,
                            frame.traceQuery);
    }
    {
        vulkan::DescriptorWrites writes;
        writes.accelerationStructure(0, scene.tlas())
            .uniformBuffer(1, uniformBuffer.handle())
            .storageBuffer(2, frame.transforms)
            .storageImage(3, images.visibility.view())
            .storageBuffer(4, scene.instanceBuffer())
            .storageBuffer(5, scene.materialBuffer())
            .storageBuffer(6, scene.lightBuffer())
            .storageImage(7, images.glassVisibility.view())
            .storageImage(8, images.noisyPrimary.view())
            .storageImage(9, images.noisyGlass.view())
            .storageImage(10, images.frameAlbedo.view())
            .storageImage(11, images.frameEmission.view())
            .storageImage(12, images.primaryGuide[current].view())
            .storageImage(13, images.primaryMotion.view())
            .storageImage(14, images.glassGuide[current].view())
            .storageImage(15, images.glassMotion.view())
            .storageImage(16, entityIds.view())
            .storageImage(17, images.primaryFilterGuide.view())
            .storageImage(18, images.glassFilterGuide.view());
        m_trace.bind(commandBuffer);
        m_trace.pushDescriptors(commandBuffer, writes.writes());
        // The samples per pixel (timed), then the paths reflected by panes: one per
        // pixel, a fixed cost like the other passes.
        for (const std::uint32_t reflections : {0U, 1U}) {
            m_trace.pushConstants(commandBuffer, &reflections, sizeof(reflections));
            vkCmdDispatch(commandBuffer, groupsX, groupsY, 1);
            if (reflections == 0 && frame.queryPool != VK_NULL_HANDLE) {
                vkCmdWriteTimestamp(commandBuffer, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                                    frame.queryPool, frame.traceQuery + 1);
            }
        }
    }
    computeBarrier(commandBuffer);

    // 3. Temporal ---------------------------------------------------------------------------------
    m_temporal.bind(commandBuffer);
    for (const std::uint32_t primary : {1U, 0U}) {
        auto& guide = primary != 0 ? images.primaryGuide : images.glassGuide;
        auto& color = primary != 0 ? images.primaryColor : images.glassColor;
        auto& moments = primary != 0 ? images.primaryMoments : images.glassMoments;
        // The glass layer has no albedo or emission: the primary's are bound, unused.
        vulkan::DescriptorWrites writes;
        writes.uniformBuffer(1, uniformBuffer.handle())
            .storageImage(3, guide[current].view())
            .storageImage(7, (primary != 0 ? images.primaryMotion : images.glassMotion).view())
            .storageImage(8, guide[previous].view())
            .storageImage(9, (primary != 0 ? images.noisyPrimary : images.noisyGlass).view())
            .storageImage(10, images.frameAlbedo.view())
            .storageImage(11, images.frameEmission.view())
            .storageImage(12, color[previous].view())
            .storageImage(13, images.primaryAlbedo[previous].view())
            .storageImage(14, images.primaryEmission[previous].view())
            .storageImage(15, moments[previous].view())
            .storageImage(16, color[current].view())
            .storageImage(17, images.primaryAlbedo[current].view())
            .storageImage(18, images.primaryEmission[current].view())
            .storageImage(19, moments[current].view())
            .storageImage(20,
                          (primary != 0 ? images.primaryFilter : images.glassFilter)[0].view());
        m_temporal.pushDescriptors(commandBuffer, writes.writes());
        m_temporal.pushConstants(commandBuffer, &primary, sizeof(primary));
        vkCmdDispatch(commandBuffer, groupsX, groupsY, 1);
        computeBarrier(commandBuffer);
    }

    // 4. A-trous ----------------------------------------------------------------------------------
    // Index of each layer's latest filter image.
    std::uint32_t primaryFiltered = 0;
    std::uint32_t glassFiltered = 0;
    const std::uint32_t iterations = denoiser.spatial ? denoiser.iterations : 0;
    if (iterations > 0) {
        m_atrous.bind(commandBuffer);
    }
    for (std::uint32_t i = 0; i < iterations; ++i) {
        const AtrousPass pass{.step = static_cast<std::int32_t>(1U << i),
                              .firstIteration = i == 0 ? 1U : 0U};
        for (const bool primary : {true, false}) {
            if (!primary && i >= kGlassIterations) {
                continue;
            }
            auto& filter = primary ? images.primaryFilter : images.glassFilter;
            std::uint32_t& filtered = primary ? primaryFiltered : glassFiltered;
            vulkan::DescriptorWrites writes;
            writes.uniformBuffer(1, uniformBuffer.handle())
                .storageImage(3, (primary ? images.primaryGuide : images.glassGuide)[current].view())
                .storageImage(7, filter[filtered].view())
                .storageImage(8, filter[1 - filtered].view())
                .storageImage(9, (primary ? images.primaryFilterGuide : images.glassFilterGuide)
                                     .view());
            m_atrous.pushDescriptors(commandBuffer, writes.writes());
            m_atrous.pushConstants(commandBuffer, &pass, sizeof(pass));
            vkCmdDispatch(commandBuffer, groupsX, groupsY, 1);
            filtered = 1 - filtered;
        }
        computeBarrier(commandBuffer);
    }

    // 5. Composite --------------------------------------------------------------------------------
    {
        vulkan::DescriptorWrites writes;
        writes.uniformBuffer(1, uniformBuffer.handle())
            .storageImage(3, images.primaryFilter[primaryFiltered].view())
            .storageImage(7, images.glassFilter[glassFiltered].view())
            .storageImage(8, images.primaryAlbedo[current].view())
            .storageImage(9, images.primaryEmission[current].view())
            .storageImage(10, images.primaryColor[current].view())
            .storageImage(11, images.noisyPrimary.view())
            .storageImage(12, images.noisyGlass.view())
            .storageImage(13, images.frameEmission.view())
            .storageImage(14, images.primaryMotion.view())
            .storageImage(15, output.view());
        m_composite.bind(commandBuffer);
        m_composite.pushDescriptors(commandBuffer, writes.writes());
        vkCmdDispatch(commandBuffer, groupsX, groupsY, 1);
    }

    m_current = previous;
}

}  // namespace starcore::gpu
