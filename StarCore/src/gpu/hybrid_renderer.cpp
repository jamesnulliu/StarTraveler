#include <algorithm>
#include <span>

#include "hybrid_renderer.hpp"
#include "starcore/vulkan/check.hpp"
#include "starcore/vulkan/context.hpp"

namespace starcore::gpu
{

namespace
{

#include "shaders/atrous.comp.spv.h"
#include "shaders/composite.comp.spv.h"
#include "shaders/gradient.comp.spv.h"
#include "shaders/hybrid_trace.comp.spv.h"
#include "shaders/temporal.comp.spv.h"

// Bindings 0-2 and 4-6 are the same in every pass (hybrid.glsl, scene.glsl):
// TLAS, frame uniforms, transforms, instances, materials, lights.
constexpr VkDescriptorType kBuffer = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
constexpr VkDescriptorType kUniform = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
constexpr VkDescriptorType kImage = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
constexpr VkDescriptorType kTlas = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
constexpr VkDescriptorType kTraceBindings[] = {
    kTlas,  kUniform, kBuffer, kImage, kBuffer, kBuffer, kBuffer, kImage, kImage, kImage,
    kImage, kImage,   kImage,  kImage, kImage,  kImage,  kImage,  kImage, kImage, kBuffer,
    kImage, kImage,   kImage,  kImage, kImage,  kImage,  kImage,  kImage};
constexpr VkDescriptorType kTemporalBindings[] = {
    kBuffer, kUniform, kBuffer, kImage, kBuffer, kBuffer, kBuffer, kImage, kImage, kImage, kImage,
    kImage,  kImage,   kImage,  kImage, kImage,  kImage,  kImage,  kImage, kImage, kImage, kImage,
    kImage,  kImage,   kImage};
constexpr VkDescriptorType kAtrousBindings[] = {kBuffer, kUniform, kBuffer, kImage, kBuffer,
                                                kBuffer, kBuffer,  kImage,  kImage, kImage};
constexpr VkDescriptorType kGradientBindings[] = {kBuffer, kUniform, kBuffer, kImage,
                                                  kBuffer, kBuffer,  kBuffer, kImage};

/** @brief Stages of hybrid_trace.comp (push constant). */
enum class TraceStage : std::uint32_t
{
    Samples = 0,      // Per pixel: the primary layer's paths and guides.
    Reflections = 1,  // Per pixel: the reflected paths.
    Gradients = 2,    // Per stratum: the temporal gradients.
};
// Pixels per side of a temporal gradient's stratum (kGradientStratum in hybrid.glsl).
constexpr std::uint32_t kGradientStratum = 3;
// Filter iterations of the gradients (radius 3 strata): one pixel per stratum
// and frame has one, of one light sample.
constexpr std::uint32_t kGradientIterations = 2;

/** @brief The layers (kLayer* in hybrid.glsl), each reprojected and filtered on its own. */
enum class Layer : std::uint32_t
{
    Primary = 0,   // Lighting of the primary surfaces.
    Glass = 1,     // What the window panes in front reflect.
    Specular = 2,  // What the primary surfaces reflect.
};
constexpr Layer kLayers[] = {Layer::Primary, Layer::Glass, Layer::Specular};
// Spatial filter iterations of the reflections at most (radius 8 pixels).
constexpr std::uint32_t kReflectionIterations = 3;

/** @brief Push constants of atrous.comp. */
struct AtrousPass
{
    std::int32_t step = 1;
    std::uint32_t firstIteration = 0;
    Layer layer = Layer::Primary;
};
constexpr VkDescriptorType kCompositeBindings[] = {
    kBuffer, kUniform, kBuffer, kImage, kBuffer, kBuffer, kBuffer, kImage, kImage,
    kImage,  kImage,   kImage,  kImage, kImage,  kImage,  kImage,  kImage, kImage};

void computeBarrier(VkCommandBuffer commandBuffer)
{
    vulkan::memoryBarrier(commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                          VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                          VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
}

}  // namespace

struct HybridRenderer::Images
{
    // This frame (hybrid_trace.comp).
    vulkan::Image noisyPrimary;
    vulkan::Image noisyGlass;
    vulkan::Image noisySpecular;
    vulkan::Image frameAlbedo;
    vulkan::Image frameEmission;
    vulkan::Image primaryMotion;
    vulkan::Image glassMotion;
    vulkan::Image reflectionMotion;  // Of the reflected images: xy specular, zw glass.
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
    // What the primary layer's samples started from and the direct light their first path
    // found there (the gradients' previous frame).
    std::array<vulkan::Image, 2> primaryHits;
    std::array<vulkan::Image, 2> directLight;
    std::array<vulkan::Image, 2> glassColor;
    std::array<vulkan::Image, 2> glassMoments;
    std::array<vulkan::Image, 2> specularColor;
    std::array<vulkan::Image, 2> specularMoments;
    std::array<vulkan::Image, 2> reflectionGuide;  // What is reflected: xy specular, zw glass.
    // Filter input ([0], from the temporal pass) and a-trous ping-pong.
    std::array<vulkan::Image, 2> primaryFilter;
    std::array<vulkan::Image, 2> glassFilter;
    std::array<vulkan::Image, 2> specularFilter;
    // Temporal gradients, one per stratum: the trace's ([0]) and the filter's ping-pong.
    std::array<vulkan::Image, 2> gradients;

    template <typename F>
    void forEach(F&& function)
    {
        for (vulkan::Image* image : {&noisyPrimary, &noisyGlass, &noisySpecular, &frameAlbedo,
                                     &frameEmission, &primaryMotion, &glassMotion,
                                     &reflectionMotion, &primaryFilterGuide, &glassFilterGuide}) {
            function(*image);
        }
        for (auto* pair :
             {&primaryGuide, &glassGuide, &primaryColor, &primaryAlbedo, &primaryEmission,
              &primaryMoments, &primaryHits, &directLight, &glassColor, &glassMoments,
              &specularColor, &specularMoments, &reflectionGuide, &primaryFilter, &glassFilter,
              &specularFilter, &gradients}) {
            function((*pair)[0]);
            function((*pair)[1]);
        }
    }

    /** @brief The images a layer is reprojected and filtered with. */
    struct LayerImages
    {
        std::array<vulkan::Image, 2>* guide;  // The surface's.
        vulkan::Image* filterGuide;
        vulkan::Image* motion;  // The surface's.
        vulkan::Image* noisy;
        std::array<vulkan::Image, 2>* color;
        std::array<vulkan::Image, 2>* moments;
        std::array<vulkan::Image, 2>* filter;
    };
    auto layer(Layer layer) -> LayerImages
    {
        switch (layer) {
        case Layer::Glass:
            return {&glassGuide, &glassFilterGuide, &glassMotion, &noisyGlass,
                    &glassColor, &glassMoments,     &glassFilter};
        case Layer::Specular:
            return {&primaryGuide,  &primaryFilterGuide, &primaryMotion, &noisySpecular,
                    &specularColor, &specularMoments,    &specularFilter};
        case Layer::Primary:
            break;
        }
        return {&primaryGuide, &primaryFilterGuide, &primaryMotion, &noisyPrimary,
                &primaryColor, &primaryMoments,     &primaryFilter};
    }
};

HybridRenderer::HybridRenderer(vulkan::Context& context)
    : m_context(&context),
      m_trace(context, hybrid_trace_comp_spv, kTraceBindings, sizeof(std::uint32_t)),
      m_temporal(context, temporal_comp_spv, kTemporalBindings, sizeof(std::uint32_t)),
      m_atrous(context, atrous_comp_spv, kAtrousBindings, sizeof(AtrousPass)),
      m_gradient(context, gradient_comp_spv, kGradientBindings, sizeof(std::int32_t)),
      m_composite(context, composite_comp_spv, kCompositeBindings)
{
}

HybridRenderer::~HybridRenderer() = default;

auto HybridRenderer::motion() const -> const vulkan::Image&
{
    return m_images->primaryMotion;
}

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
    images->noisyPrimary = storage(VK_FORMAT_R32G32B32A32_SFLOAT);
    images->noisyGlass = storage(VK_FORMAT_R32G32B32A32_SFLOAT);
    images->noisySpecular = storage(VK_FORMAT_R32G32B32A32_SFLOAT);
    images->frameAlbedo = storage(VK_FORMAT_R16G16B16A16_SFLOAT);
    images->frameEmission = storage(VK_FORMAT_R16G16B16A16_SFLOAT);
    images->primaryMotion = storage(VK_FORMAT_R32G32B32A32_SFLOAT);
    images->glassMotion = storage(VK_FORMAT_R32G32B32A32_SFLOAT);
    images->reflectionMotion = storage(VK_FORMAT_R32G32B32A32_SFLOAT);
    images->primaryFilterGuide = storage(VK_FORMAT_R32G32_UINT);
    images->glassFilterGuide = storage(VK_FORMAT_R32G32_UINT);
    for (std::size_t i = 0; i < 2; ++i) {
        images->primaryGuide[i] = storage(VK_FORMAT_R32G32B32A32_UINT);
        images->glassGuide[i] = storage(VK_FORMAT_R32G32B32A32_UINT);
        images->primaryColor[i] = storage(VK_FORMAT_R32G32B32A32_SFLOAT);
        images->primaryAlbedo[i] = storage(VK_FORMAT_R32G32B32A32_SFLOAT);
        images->primaryEmission[i] = storage(VK_FORMAT_R16G16B16A16_SFLOAT);
        images->primaryMoments[i] = storage(VK_FORMAT_R32G32_SFLOAT);
        images->primaryHits[i] = storage(VK_FORMAT_R32G32_UINT);
        images->directLight[i] = storage(VK_FORMAT_R32_SFLOAT);
        images->glassColor[i] = storage(VK_FORMAT_R32G32B32A32_SFLOAT);
        images->glassMoments[i] = storage(VK_FORMAT_R32G32_SFLOAT);
        images->specularColor[i] = storage(VK_FORMAT_R32G32B32A32_SFLOAT);
        images->specularMoments[i] = storage(VK_FORMAT_R32G32_SFLOAT);
        images->reflectionGuide[i] = storage(VK_FORMAT_R32G32B32A32_UINT);
        images->primaryFilter[i] = storage(VK_FORMAT_R16G16B16A16_SFLOAT);
        images->glassFilter[i] = storage(VK_FORMAT_R16G16B16A16_SFLOAT);
        images->specularFilter[i] = storage(VK_FORMAT_R16G16B16A16_SFLOAT);
        images->gradients[i] = vulkan::Image(
            *m_context, (width + kGradientStratum - 1) / kGradientStratum,
            (height + kGradientStratum - 1) / kGradientStratum, VK_FORMAT_R32G32B32A32_SFLOAT,
            VK_IMAGE_USAGE_STORAGE_BIT);
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

void HybridRenderer::record(VkCommandBuffer commandBuffer, const HybridFrame& frame,
                            const GpuScene& scene, VisibilityPass& visibility,
                            const vulkan::Image& radiance, const vulkan::Image& entityIds)
{
    Images& images = *m_images;
    const std::uint32_t current = m_current;
    const std::uint32_t previous = 1 - current;
    const std::uint32_t width = radiance.width();
    const std::uint32_t height = radiance.height();
    const PathTracerSettings& settings = frame.settings;
    const DenoiserSettings& denoiser = frame.options.denoiser;
    const RenderScene& renderScene = *frame.scene;

    // Uniforms ------------------------------------------------------------------------------------
    FrameUniforms uniforms{};
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
    uniforms.history[0] = frame.still ? 0U : std::max(1U, denoiser.historyFrames);
    uniforms.history[1] = (frame.reset ? kHistoryReset : 0U) | (frame.still ? kHistoryStill : 0U);
    uniforms.history[2] = static_cast<std::uint32_t>(frame.options.view);
    uniforms.denoiser = Vec4(denoiser.colorSigma, denoiser.normalPower, denoiser.depthSigma, 0.0F);
    vulkan::Buffer& uniformBuffer = m_uniforms[frame.frameSlot];
    writeUniforms(*m_context, uniformBuffer, uniforms);

    // 1. Visibility -------------------------------------------------------------------------------
    visibility.record(commandBuffer, uniformBuffer.handle(), scene, renderScene,
                      settings.instanceMask, GlassLayer::SmoothPanes);

    const std::uint32_t groupsX = (width + kWorkgroupSize - 1) / kWorkgroupSize;
    const std::uint32_t groupsY = (height + kWorkgroupSize - 1) / kWorkgroupSize;
    // The previous frame's samples are reused, and the lighting may have changed.
    const bool moving = !frame.reset && !frame.still;

    // 2. Trace ------------------------------------------------------------------------------------
    if (frame.queryPool != VK_NULL_HANDLE) {
        vkCmdWriteTimestamp(commandBuffer, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, frame.queryPool,
                            frame.traceQuery);
    }
    {
        vulkan::DescriptorWrites writes;
        writes.accelerationStructure(0, scene.tlas())
            .uniformBuffer(1, uniformBuffer.handle())
            .storageBuffer(2, scene.transformBuffer())
            .storageImage(3, visibility.visibility().view())
            .storageBuffer(4, scene.instanceBuffer())
            .storageBuffer(5, scene.materialBuffer())
            .storageBuffer(6, scene.lightBuffer())
            .storageImage(7, visibility.glassVisibility().view())
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
            .storageImage(18, images.glassFilterGuide.view())
            .storageBuffer(19, scene.previousTransformBuffer())
            .storageImage(20, images.noisySpecular.view())
            .storageImage(21, images.reflectionMotion.view())
            .storageImage(22, images.reflectionGuide[current].view())
            .storageImage(23, images.primaryHits[previous].view())
            .storageImage(24, images.primaryHits[current].view())
            .storageImage(25, images.directLight[previous].view())
            .storageImage(26, images.directLight[current].view())
            .storageImage(27, images.gradients[0].view());
        m_trace.bind(commandBuffer);
        m_trace.pushDescriptors(commandBuffer, writes.writes());
        // The samples per pixel (timed), then the reflected paths: one per pixel and
        // layer, a fixed cost like the other passes.
        for (const TraceStage stage : {TraceStage::Samples, TraceStage::Reflections}) {
            m_trace.pushConstants(commandBuffer, &stage, sizeof(stage));
            vkCmdDispatch(commandBuffer, groupsX, groupsY, 1);
            if (stage == TraceStage::Samples && frame.queryPool != VK_NULL_HANDLE) {
                vkCmdWriteTimestamp(commandBuffer, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                                    frame.queryPool, frame.traceQuery + 1);
            }
        }
        // The temporal gradients, while things move (still, the lighting is what it was).
        if (moving) {
            const TraceStage stage = TraceStage::Gradients;
            m_trace.pushConstants(commandBuffer, &stage, sizeof(stage));
            vkCmdDispatch(commandBuffer, (images.gradients[0].width() + kWorkgroupSize - 1) /
                                             kWorkgroupSize,
                          (images.gradients[0].height() + kWorkgroupSize - 1) / kWorkgroupSize, 1);
        }
    }
    computeBarrier(commandBuffer);

    // Index of the filtered gradients.
    std::uint32_t gradients = 0;
    if (moving) {
        m_gradient.bind(commandBuffer);
        for (std::uint32_t i = 0; i < kGradientIterations; ++i) {
            const auto step = static_cast<std::int32_t>(1U << i);
            vulkan::DescriptorWrites writes;
            writes.uniformBuffer(1, uniformBuffer.handle())
                .storageImage(3, images.gradients[gradients].view())
                .storageImage(7, images.gradients[1 - gradients].view());
            m_gradient.pushDescriptors(commandBuffer, writes.writes());
            m_gradient.pushConstants(commandBuffer, &step, sizeof(step));
            vkCmdDispatch(commandBuffer,
                          (images.gradients[0].width() + kWorkgroupSize - 1) / kWorkgroupSize,
                          (images.gradients[0].height() + kWorkgroupSize - 1) / kWorkgroupSize,
                          1);
            computeBarrier(commandBuffer);
            gradients = 1 - gradients;
        }
    }

    // 3. Temporal ---------------------------------------------------------------------------------
    m_temporal.bind(commandBuffer);
    for (const Layer layer : kLayers) {
        const Images::LayerImages layerImages = images.layer(layer);
        // Only the primary layer has albedo, emission and gradients: their images are bound,
        // unused, for the others (a barrier between the layers all the same: they count as
        // used). The gradients only while moving (unused still).
        vulkan::DescriptorWrites writes;
        writes.uniformBuffer(1, uniformBuffer.handle())
            .storageImage(3, (*layerImages.guide)[current].view())
            .storageImage(7, layerImages.motion->view())
            .storageImage(8, (*layerImages.guide)[previous].view())
            .storageImage(9, layerImages.noisy->view())
            .storageImage(10, images.frameAlbedo.view())
            .storageImage(11, images.frameEmission.view())
            .storageImage(12, (*layerImages.color)[previous].view())
            .storageImage(13, images.primaryAlbedo[previous].view())
            .storageImage(14, images.primaryEmission[previous].view())
            .storageImage(15, (*layerImages.moments)[previous].view())
            .storageImage(16, (*layerImages.color)[current].view())
            .storageImage(17, images.primaryAlbedo[current].view())
            .storageImage(18, images.primaryEmission[current].view())
            .storageImage(19, (*layerImages.moments)[current].view())
            .storageImage(20, (*layerImages.filter)[0].view())
            .storageImage(21, images.reflectionMotion.view())
            .storageImage(22, images.reflectionGuide[previous].view())
            .storageImage(23, images.reflectionGuide[current].view())
            .storageImage(24, images.gradients[gradients].view());
        m_temporal.pushDescriptors(commandBuffer, writes.writes());
        m_temporal.pushConstants(commandBuffer, &layer, sizeof(layer));
        vkCmdDispatch(commandBuffer, groupsX, groupsY, 1);
        computeBarrier(commandBuffer);
    }

    // 4. A-trous ----------------------------------------------------------------------------------
    // Index of each layer's latest filter image.
    std::array<std::uint32_t, std::size(kLayers)> filtered{};
    const std::uint32_t iterations = denoiser.spatial ? denoiser.iterations : 0;
    if (iterations > 0) {
        m_atrous.bind(commandBuffer);
    }
    for (std::uint32_t i = 0; i < iterations; ++i) {
        for (const Layer layer : kLayers) {
            if (layer != Layer::Primary && i >= kReflectionIterations) {
                continue;
            }
            const AtrousPass pass{.step = static_cast<std::int32_t>(1U << i),
                                  .firstIteration = i == 0 ? 1U : 0U,
                                  .layer = layer};
            const Images::LayerImages layerImages = images.layer(layer);
            std::uint32_t& latest = filtered[static_cast<std::size_t>(layer)];
            vulkan::DescriptorWrites writes;
            writes.uniformBuffer(1, uniformBuffer.handle())
                .storageImage(3, (*layerImages.guide)[current].view())
                .storageImage(7, (*layerImages.filter)[latest].view())
                .storageImage(8, (*layerImages.filter)[1 - latest].view())
                .storageImage(9, layerImages.filterGuide->view());
            m_atrous.pushDescriptors(commandBuffer, writes.writes());
            m_atrous.pushConstants(commandBuffer, &pass, sizeof(pass));
            vkCmdDispatch(commandBuffer, groupsX, groupsY, 1);
            latest = 1 - latest;
        }
        computeBarrier(commandBuffer);
    }

    // 5. Composite --------------------------------------------------------------------------------
    {
        auto latest = [&](Layer layer) -> const vulkan::Image& {
            return (*images.layer(layer).filter)[filtered[static_cast<std::size_t>(layer)]];
        };
        vulkan::DescriptorWrites writes;
        writes.uniformBuffer(1, uniformBuffer.handle())
            .storageImage(3, latest(Layer::Primary).view())
            .storageImage(7, latest(Layer::Glass).view())
            .storageImage(8, images.primaryAlbedo[current].view())
            .storageImage(9, images.primaryEmission[current].view())
            .storageImage(10, images.primaryColor[current].view())
            .storageImage(11, images.noisyPrimary.view())
            .storageImage(12, images.noisyGlass.view())
            .storageImage(13, images.frameEmission.view())
            .storageImage(14, images.primaryMotion.view())
            .storageImage(15, radiance.view())
            .storageImage(16, latest(Layer::Specular).view())
            .storageImage(17, images.noisySpecular.view());
        m_composite.bind(commandBuffer);
        m_composite.pushDescriptors(commandBuffer, writes.writes());
        vkCmdDispatch(commandBuffer, groupsX, groupsY, 1);
    }

    m_current = previous;
}

}  // namespace starcore::gpu
