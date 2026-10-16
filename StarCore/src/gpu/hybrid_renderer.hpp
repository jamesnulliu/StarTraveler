#pragma once
// The Hybrid pipeline of GpuRenderer (private). Per frame:
//   1. visibility (VisibilityPass): rasterize instance and triangle ids with a
//      jittered projection, the primary surfaces and then, over a copy of
//      their depth, the nearest smooth window panes (the glass layer);
//   2. trace (hybrid_trace.comp, direct.comp): rebuild the hits of both from
//      those ids, write their guides and motion vectors; the light the primary
//      surfaces' diffuse lobe gets from the sphere lights, unshadowed in closed
//      form and (pixels traced) shadowed (the direct layer; or every pixel
//      shadowed with shadow maps, rendered first: DirectShadows); trace paths from
//      them (every pixel, or one of each 2x2 in turn, TraceResolution) into
//      three more layers: the rest of the primary surfaces' lighting, what they
//      reflect (specular) and what the panes reflect (glass); while things move,
//      the temporal gradients of each layer (filtered by gradient.comp);
//      With the radiance cache (GpuRenderOptions::radianceCache), first the paths
//      of a few pixels teach it the light their vertices reflect (radiance_cache
//      .hpp), and the other paths end at their first vertex it knows;
//   3. temporal (temporal.comp, per layer): reproject and accumulate (the
//      pixels not traced borrow from the traced ones around them);
//   4. a-trous (atrous.comp): the SVGF spatial filter, a few iterations;
//   5. composite (composite.comp): the direct light (unshadowed x how much of it
//      arrives) + lighting x albedo + emission + specular + glass into
//      GpuRenderer's radiance image.

#include <array>
#include <cstdint>
#include <memory>

#include <vulkan/vulkan.h>

#include "radiance_cache.hpp"
#include "rasterization.hpp"
#include "shadow_maps.hpp"
#include "starcore/gpu/gpu_renderer.hpp"
#include "starcore/gpu/gpu_scene.hpp"
#include "starcore/vulkan/pipeline.hpp"
#include "starcore/vulkan/resources.hpp"

namespace starcore::gpu
{

/** @brief One frame of the hybrid pipeline. */
struct HybridFrame
{
    const RenderScene* scene = nullptr;
    const Camera* camera = nullptr;
    const Camera* previousCamera = nullptr;  // For motion vectors.
    PathTracerSettings settings;
    GpuRenderOptions options;
    std::uint32_t frameSlot = 0;
    std::uint32_t frameIndex = 0;  // Random streams and the jitter.
    bool reset = false;            // Discard earlier samples.
    bool still = false;            // Nothing moved since the previous frame.
    bool writeIds = false;
    // An upscaler takes the result (GpuRenderOptions::upscaling): each frame's own
    // albedo, emission and unshadowed light at its sample positions, not their
    // averages over the last frames (the upscaler averages them, with the detail of the
    // output's pixels).
    bool upscaled = false;
    // Timestamps around the trace pass (queries traceQuery, traceQuery + 1).
    VkQueryPool queryPool = VK_NULL_HANDLE;
    std::uint32_t traceQuery = 0;
};

class HybridRenderer
{
public:
    explicit HybridRenderer(vulkan::Context& context);
    ~HybridRenderer();
    HybridRenderer(const HybridRenderer&) = delete;
    auto operator=(const HybridRenderer&) -> HybridRenderer& = delete;

    /** @brief (Re)create the images; the next frame must reset. Waits for nothing. */
    void resize(std::uint32_t width, std::uint32_t height);

    /** @brief Record a frame writing `radiance` (RGBA32F) and, if asked, `entityIds`. */
    void record(VkCommandBuffer commandBuffer, const HybridFrame& frame, const GpuScene& scene,
                VisibilityPass& visibility, const vulkan::Image& radiance,
                const vulkan::Image& entityIds);

    /**
     * @brief RGBA32F, xy: previous minus current pixel position of the primary
     *        surfaces, w: their view depth (sampled by the vendors' upscalers).
     */
    [[nodiscard]] auto motion() const -> const vulkan::Image&;

private:
    struct Images;

    vulkan::Context* m_context;
    // hybrid_trace.comp, a pipeline per stage (TraceStage).
    std::array<vulkan::ComputePipeline, 5> m_trace;
    std::unique_ptr<RadianceCache> m_cache;  // Created when first used.
    // temporal.comp: every pixel traced, and some borrowing (TraceResolution::Half).
    std::array<vulkan::ComputePipeline, 2> m_temporal;
    // atrous.comp: reading the taps from the images, and tiled.
    std::array<vulkan::ComputePipeline, 2> m_atrous;
    // direct.comp: shadows traced, and looked up in shadow maps (DirectShadows).
    std::array<vulkan::ComputePipeline, 2> m_direct;
    ShadowMaps m_shadows;
    vulkan::ComputePipeline m_gradient;
    vulkan::ComputePipeline m_composite;
    std::array<vulkan::Buffer, kFramesInFlight> m_uniforms;
    std::unique_ptr<Images> m_images;
    std::uint32_t m_current = 0;  // Which of the ping-pong images this frame writes.
};

}  // namespace starcore::gpu
