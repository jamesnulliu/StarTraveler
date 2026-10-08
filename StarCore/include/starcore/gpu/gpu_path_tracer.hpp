#pragma once

#include <array>
#include <cstdint>
#include <expected>
#include <memory>
#include <string>

#include <vulkan/vulkan.h>

#include "starcore/common/image.hpp"
#include "starcore/gpu/gpu_scene.hpp"
#include "starcore/render/camera.hpp"
#include "starcore/render/path_tracer.hpp"
#include "starcore/render/render_scene.hpp"
#include "starcore/vulkan/pipeline.hpp"
#include "starcore/vulkan/resources.hpp"

namespace starcore
{

/**
 * @brief Progressive path tracer on the GPU: a compute shader with ray
 *        queries against acceleration structures built from the RenderScene.
 *
 * Same interface and estimator as CpuPathTracer (shaders/path_trace.comp is a
 * port of path_tracer.cpp, with the same random number streams), so both
 * converge to the same image. Each render() records and submits one frame of
 * samples without waiting for it; at most kFramesInFlight frames are queued.
 * The output image is read by later work on the same queue (the UI pass) or
 * copied back with readImage().
 */
class GpuPathTracer
{
public:
    /** @return An error if the device has no ray query support. */
    [[nodiscard]] static auto create(vulkan::Context& context)
        -> std::expected<std::unique_ptr<GpuPathTracer>, std::string>;

    ~GpuPathTracer();
    GpuPathTracer(const GpuPathTracer&) = delete;
    auto operator=(const GpuPathTracer&) -> GpuPathTracer& = delete;

    /** @brief Recreates the images (waits for the device when they exist). */
    void resize(std::uint32_t width, std::uint32_t height);
    void resetAccumulation() noexcept
    {
        m_frameIndex = 0;
    }

    /**
     * @brief Trace settings.samplesPerPixel samples per pixel. Accumulation
     *        restarts when the scene, camera, settings or size change, except
     *        for samplesPerPixel: frames are weighted by their sample count,
     *        so it may follow a time budget (see samplesForBudget).
     */
    void render(const RenderScene& scene, const Camera& camera,
                const PathTracerSettings& settings);

    [[nodiscard]] auto width() const noexcept -> std::uint32_t
    {
        return m_output.width();
    }
    [[nodiscard]] auto height() const noexcept -> std::uint32_t
    {
        return m_output.height();
    }

    /** @brief Layout the output image stays in (storage writes and sampling). */
    static constexpr VkImageLayout kOutputLayout = VK_IMAGE_LAYOUT_GENERAL;
    /** @brief Tone-mapped RGBA8 result; recreated by resize(). */
    [[nodiscard]] auto outputView() const noexcept -> VkImageView
    {
        return m_output.view();
    }

    [[nodiscard]] auto accumulatedFrames() const noexcept -> std::uint32_t
    {
        return m_frameIndex;
    }
    /** @brief GPU time of the last completed frame in milliseconds (0 if unknown). */
    [[nodiscard]] auto gpuTimeMs() const noexcept -> float
    {
        return m_gpuTimeMs;
    }

    /**
     * @brief Samples per pixel that should take about `budgetMs` of GPU time
     *        per frame, from the cost of recent path-traced frames; grows at
     *        most 2x per frame. 1 until a frame was measured.
     */
    [[nodiscard]] auto samplesForBudget(float budgetMs, std::uint32_t maxSamples = 64) const
        -> std::uint32_t;

    /** @brief Wait for the GPU and copy the output image to the CPU. */
    [[nodiscard]] auto readImage() -> ColorImage;
    /** @brief Entity ids of the first frame since the last reset (kNoEntityId = sky). */
    [[nodiscard]] auto readEntityIds() -> IdImage;

    /** @brief Wait until all submitted frames have completed. */
    void waitIdle();

private:
    struct FrameSlot
    {
        VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
        VkFence fence = VK_NULL_HANDLE;
        bool submitted = false;
        bool timed = false;
        // Samples per pixel of a path-traced frame (0: preview, not a cost sample).
        std::uint32_t tracedSamples = 0;
    };

    explicit GpuPathTracer(vulkan::Context& context);
    void waitForSlot(std::uint32_t slot);
    [[nodiscard]] auto needsReset(std::uint64_t sceneHash, const Camera& camera,
                                  const PathTracerSettings& settings) const -> bool;
    void readBack(const vulkan::Image& image, std::span<std::byte> destination);

    vulkan::Context* m_context;
    gpu::GpuScene m_scene;
    vulkan::ComputePipeline m_pipeline;

    VkCommandPool m_commandPool = VK_NULL_HANDLE;
    VkQueryPool m_queryPool = VK_NULL_HANDLE;
    std::array<FrameSlot, gpu::kFramesInFlight> m_slots{};
    std::uint64_t m_frameNumber = 0;  // Submitted frames.

    vulkan::Image m_accumulation;  // RGBA32F: rgb = sum of samples, a = count.
    vulkan::Image m_output;        // RGBA8, tone mapped.
    vulkan::Image m_entityIds;     // R32_UINT.

    std::uint32_t m_frameIndex = 0;  // Accumulated frames since the last reset.
    float m_gpuTimeMs = 0.0F;
    float m_msPerSample = 0.0F;          // Of path-traced frames (0 = unknown).
    std::uint32_t m_lastSamples = 1;     // Samples per pixel of the last frame.

    // State of the previous frame, for automatic accumulation resets.
    std::uint64_t m_uploadedSceneHash = 0;
    bool m_sceneUploaded = false;
    std::uint64_t m_lastSceneVersion = ~0ULL;
    Mat4 m_lastView{0.0F};
    Mat4 m_lastProjection{0.0F};
    PathTracerSettings m_lastSettings{};
};

}  // namespace starcore
