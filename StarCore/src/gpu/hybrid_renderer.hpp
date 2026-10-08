#pragma once
// The Hybrid pipeline of GpuPathTracer (private). Per frame:
//   1. visibility: rasterize instance and triangle ids with a jittered
//      projection, the primary surfaces and then, over a copy of their depth,
//      the nearest smooth window panes (the glass layer);
//   2. trace (hybrid_trace.comp): rebuild the hits of both layers from those
//      ids, write their guides and motion vectors, trace paths from them;
//   3. temporal (temporal.comp, per layer): reproject and accumulate;
//   4. a-trous (atrous.comp): the SVGF spatial filter, a few iterations;
//   5. composite (composite.comp): lighting x albedo + emission + glass, tone
//      mapped into GpuPathTracer's output image.

#include <array>
#include <cstdint>
#include <memory>

#include <vulkan/vulkan.h>

#include "starcore/gpu/gpu_path_tracer.hpp"
#include "starcore/gpu/gpu_scene.hpp"
#include "starcore/vulkan/pipeline.hpp"
#include "starcore/vulkan/resources.hpp"

namespace starcore::gpu
{

/** @brief Uniforms of every hybrid pass (`FrameUniforms` in hybrid.glsl, std140). */
struct HybridFrameUniforms
{
    Vec4 cameraPosition;  // w: near plane of the visibility pass
    Vec4 cameraForward;   // w: firefly clamp (0 = off)
    Vec4 cameraRight;     // right * aspect * tan(fov / 2); w: exposure
    Vec4 cameraUp;        // up * tan(fov / 2)
    Vec4 previousPosition;
    Vec4 previousForward;
    Vec4 previousRight;
    Vec4 previousUp;
    Vec4 skyColor;
    Vec4 viewport;               // xy: size, zw: sample position in the pixel (jitter)
    Vec4 previousJitter;         // xy: the previous frame's
    std::uint32_t frame[4];      // random stream index, seed, samples per pixel, max bounces
    std::uint32_t options[4];    // flags, instance mask, light count, tone mapper
    std::uint32_t history[4];    // cap (0 = none), reset/still bits, view, -
    Vec4 denoiser;               // color sigma, normal power, depth sigma, -
};
static_assert(sizeof(HybridFrameUniforms) == 240, "std140 layout of FrameUniforms");

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
    VkBuffer transforms = VK_NULL_HANDLE;  // GpuScene::writeTransforms().
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
                const vulkan::Image& output, const vulkan::Image& entityIds);

private:
    struct Images;

    void renderVisibility(VkCommandBuffer commandBuffer, const HybridFrame& frame,
                          const GpuScene& scene, bool glass);

    vulkan::Context* m_context;
    vulkan::GraphicsPipeline m_visibility;        // Triangles.
    vulkan::GraphicsPipeline m_visibilitySphere;  // Sphere impostors.
    vulkan::ComputePipeline m_trace;
    vulkan::ComputePipeline m_temporal;
    vulkan::ComputePipeline m_atrous;
    vulkan::ComputePipeline m_composite;
    std::array<vulkan::Buffer, kFramesInFlight> m_uniforms;
    std::unique_ptr<Images> m_images;
    std::uint32_t m_current = 0;  // Which of the ping-pong images this frame writes.
};

}  // namespace starcore::gpu
