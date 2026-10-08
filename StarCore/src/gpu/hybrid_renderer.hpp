#pragma once
// The Hybrid pipeline of GpuRenderer (private). Per frame:
//   1. visibility (VisibilityPass): rasterize instance and triangle ids with a
//      jittered projection, the primary surfaces and then, over a copy of
//      their depth, the nearest smooth window panes (the glass layer);
//   2. trace (hybrid_trace.comp): rebuild the hits of both layers from those
//      ids, write their guides and motion vectors, trace paths from them;
//   3. temporal (temporal.comp, per layer): reproject and accumulate;
//   4. a-trous (atrous.comp): the SVGF spatial filter, a few iterations;
//   5. composite (composite.comp): lighting x albedo + emission + glass, tone
//      mapped into GpuRenderer's output image.

#include <array>
#include <cstdint>
#include <memory>

#include <vulkan/vulkan.h>

#include "rasterization.hpp"
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

    /** @brief Record a frame writing `output` (RGBA8) and, if asked, `entityIds`. */
    void record(VkCommandBuffer commandBuffer, const HybridFrame& frame, const GpuScene& scene,
                VisibilityPass& visibility, const vulkan::Image& output,
                const vulkan::Image& entityIds);

private:
    struct Images;

    vulkan::Context* m_context;
    vulkan::ComputePipeline m_trace;
    vulkan::ComputePipeline m_temporal;
    vulkan::ComputePipeline m_atrous;
    vulkan::ComputePipeline m_composite;
    std::array<vulkan::Buffer, kFramesInFlight> m_uniforms;
    std::unique_ptr<Images> m_images;
    std::uint32_t m_current = 0;  // Which of the ping-pong images this frame writes.
};

}  // namespace starcore::gpu
