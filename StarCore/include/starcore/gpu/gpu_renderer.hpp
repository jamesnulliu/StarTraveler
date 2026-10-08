#pragma once

#include <array>
#include <cstdint>
#include <expected>
#include <memory>
#include <string>

#include <vulkan/vulkan.h>

#include "starcore/common/enum_names.hpp"
#include "starcore/common/image.hpp"
#include "starcore/gpu/gpu_scene.hpp"
#include "starcore/render/camera.hpp"
#include "starcore/render/path_tracer.hpp"
#include "starcore/render/render_scene.hpp"
#include "starcore/vulkan/pipeline.hpp"
#include "starcore/vulkan/resources.hpp"

namespace starcore
{

namespace gpu
{
class HybridRenderer;
class RasterRenderer;
class VisibilityPass;
}  // namespace gpu

/** @brief How GpuRenderer::render() makes an image. */
enum class GpuPipeline : std::uint8_t
{
    // Camera rays and plain accumulation that restarts whenever anything
    // changes: converges to the CPU path tracer's image (tests, offline).
    PathTracing,
    // Rasterized first hits (a visibility buffer), paths traced from there
    // with the same estimator, samples reprojected across frames and
    // denoised: for real time.
    Hybrid,
    // Rasterized first hits lit like RenderMode::Preview without shadows: no
    // rays, so no ray tracing hardware needed. Anti-aliased while still.
    Raster,
};

template <>
struct EnumNames<GpuPipeline>
{
    static constexpr std::array kNames = {"Path tracing", "Hybrid", "Raster"};
};

/** @brief Whether a pipeline traces rays (needs ray queries). */
[[nodiscard]] constexpr auto tracesRays(GpuPipeline pipeline) noexcept -> bool
{
    return pipeline != GpuPipeline::Raster;
}

/** @brief What the hybrid pipeline shows: its result or an intermediate image. */
enum class HybridView : std::uint8_t
{
    Final,
    Noisy,     // This frame's samples, before any reuse.
    Albedo,    // Of the primary surfaces (lighting is filtered divided by it).
    Lighting,  // Filtered lighting of the primary surfaces.
    Glass,     // What window panes reflect (a layer of its own).
    Variance,  // Relative standard deviation of the lighting's mean.
    Motion,    // Screen motion of the primary surfaces (|x|, |y| / 8 px).
    History,   // Samples per pixel reused (white: the history cap or more).
};

template <>
struct EnumNames<HybridView>
{
    static constexpr std::array kNames = {"Final",    "Noisy",    "Albedo", "Lighting",
                                          "Glass",    "Variance", "Motion", "History"};
};

/** @brief Sample reuse of the hybrid pipeline (SVGF-style). */
struct DenoiserSettings
{
    // Reproject the samples of earlier frames (off: every frame starts over).
    bool temporal = true;
    // Edge-aware a-trous filter of the lighting; it fades out as samples
    // accumulate, so a still image converges to the unfiltered mean.
    bool spatial = true;
    // Samples per pixel kept while the camera or objects move: more is
    // smoother, fewer follows changing light faster.
    std::uint32_t historySamples = 64;
    std::uint32_t iterations = 5;  // Filter radius: 2^iterations pixels.
    float colorSigma = 4.0F;       // Luminance tolerance, in standard deviations.
    float normalPower = 128.0F;
    float depthSigma = 1.0F;

    auto operator==(const DenoiserSettings&) const -> bool = default;
};

struct GpuRenderOptions
{
    GpuPipeline pipeline = GpuPipeline::PathTracing;
    DenoiserSettings denoiser;
    HybridView view = HybridView::Final;

    auto operator==(const GpuRenderOptions&) const -> bool = default;
};

/**
 * @brief Renderer on the GPU: path tracing in compute shaders with ray
 *        queries against acceleration structures built from the RenderScene,
 *        or rasterization (see GpuPipeline).
 *
 * The PathTracing pipeline has the interface and estimator of CpuPathTracer
 * (shaders/path_trace.comp is a port of path_tracer.cpp, with the same random
 * number streams), so both converge to the same image. The Hybrid pipeline
 * rasterizes the first hits, traces paths from there with the same estimator
 * and reuses samples across frames while the view moves (see
 * GpuRenderOptions). The Raster pipeline traces nothing. Each render()
 * records and submits one frame without waiting for it; at most
 * kFramesInFlight frames are queued. The output image is read by later work
 * on the same queue (the UI pass) or copied back with readImage().
 *
 * The scene is uploaded incrementally: render scenes kept up to date by a
 * RenderSceneBuilder upload only the instances that changed.
 */
class GpuRenderer
{
public:
    /** @return An error if the device has no push descriptors (all pipelines use them). */
    [[nodiscard]] static auto create(vulkan::Context& context)
        -> std::expected<std::unique_ptr<GpuRenderer>, std::string>;

    ~GpuRenderer();
    GpuRenderer(const GpuRenderer&) = delete;
    auto operator=(const GpuRenderer&) -> GpuRenderer& = delete;

    /** @brief Whether the pipelines that trace rays can run (the device has ray queries). */
    [[nodiscard]] auto rayTracing() const noexcept -> bool
    {
        return m_scene.rayTracing();
    }

    /** @brief Recreates the images (waits for the device when they exist). */
    void resize(std::uint32_t width, std::uint32_t height);
    void resetAccumulation() noexcept
    {
        m_frameIndex = 0;
    }

    /**
     * @brief Render a frame; tracing pipelines trace settings.samplesPerPixel
     *        samples per pixel. Frames are weighted by their sample count, so
     *        it may follow a time budget (see samplesForBudget).
     *
     * PathTracing and Raster: accumulation restarts when the scene, camera,
     * settings that change what is sampled, or size change. Hybrid: it
     * restarts when the scene's content or the estimator's settings change;
     * when only the camera or instance transforms change, earlier samples are
     * reprojected.
     *
     * @pre rayTracing() || !tracesRays(options.pipeline)
     */
    void render(const RenderScene& scene, const Camera& camera,
                const PathTracerSettings& settings, const GpuRenderOptions& options = {});

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

    /** @brief Frames since the accumulation last restarted. */
    [[nodiscard]] auto accumulatedFrames() const noexcept -> std::uint32_t
    {
        return m_frameIndex;
    }
    /** @brief GPU time of the last completed frame in milliseconds (0 if unknown). */
    [[nodiscard]] auto gpuTimeMs() const noexcept -> float
    {
        return m_gpuTimeMs;
    }
    /** @brief Of that, the time spent tracing paths (the rest: rasterizing, filtering). */
    [[nodiscard]] auto traceTimeMs() const noexcept -> float
    {
        return m_traceTimeMs;
    }
    /** @brief Instances uploaded for the last frame (incremental scene updates). */
    [[nodiscard]] auto uploadedInstances() const noexcept -> std::uint32_t
    {
        return m_scene.uploadedInstances();
    }

    /**
     * @brief Samples per pixel that should take about `budgetMs` of GPU time
     *        per frame, from the cost of recent path-traced frames (the
     *        time spent besides tracing comes off the budget); grows at most
     *        2x per frame. 1 until a frame was measured.
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
        // Samples per pixel of a path-traced frame (0: preview or raster, not a cost sample).
        std::uint32_t tracedSamples = 0;
    };

    explicit GpuRenderer(vulkan::Context& context);
    void waitForSlot(std::uint32_t slot);
    /** @brief Whether the frame starts over; `still`: nothing moved since the last one. */
    [[nodiscard]] auto needsReset(const RenderScene& scene, const Camera& camera,
                                  const PathTracerSettings& settings,
                                  const GpuRenderOptions& options, bool& still) const -> bool;
    void recordPathTracing(VkCommandBuffer commandBuffer, std::uint32_t slotIndex,
                           const RenderScene& scene, const Camera& camera,
                           const PathTracerSettings& settings);
    void readBack(const vulkan::Image& image, std::span<std::byte> destination);

    vulkan::Context* m_context;
    gpu::GpuScene m_scene;
    // Created when first used.
    std::unique_ptr<vulkan::ComputePipeline> m_pathTracing;
    std::unique_ptr<gpu::VisibilityPass> m_visibility;
    std::unique_ptr<gpu::HybridRenderer> m_hybrid;
    std::unique_ptr<gpu::RasterRenderer> m_raster;

    VkCommandPool m_commandPool = VK_NULL_HANDLE;
    // Per frame slot: frame start, tracing start and end, frame end.
    VkQueryPool m_queryPool = VK_NULL_HANDLE;
    std::array<FrameSlot, gpu::kFramesInFlight> m_slots{};
    std::uint64_t m_frameNumber = 0;  // Submitted frames.

    vulkan::Image m_accumulation;  // RGBA32F: rgb = sum of samples, a = count.
    vulkan::Image m_output;        // RGBA8, tone mapped.
    vulkan::Image m_entityIds;     // R32_UINT.

    std::uint32_t m_frameIndex = 0;  // Accumulated frames since the last reset.
    float m_gpuTimeMs = 0.0F;
    float m_traceTimeMs = 0.0F;
    float m_msPerSample = 0.0F;          // Of path-traced frames (0 = unknown).
    float m_overheadMs = 0.0F;           // GPU time besides tracing.
    std::uint32_t m_lastSamples = 1;     // Samples per pixel of the last frame.

    // State of the previous frame, for automatic accumulation resets.
    std::uint64_t m_lastVersion = 0;         // RenderScene::version.
    std::uint64_t m_lastContentVersion = 0;  // RenderScene::contentVersion.
    Mat4 m_lastView{0.0F};
    Mat4 m_lastProjection{0.0F};
    PathTracerSettings m_lastSettings{};
    GpuPipeline m_lastPipeline = GpuPipeline::PathTracing;
    bool m_lastTemporal = true;
    Camera m_lastCamera;  // For the hybrid pipeline's motion vectors.
};

}  // namespace starcore
