#include "demo_presets.hpp"

#include <array>

namespace startraveler
{

using namespace starcore;

namespace
{

constexpr std::array<std::string_view, kDemoPresetCount> kNames = {
    "Raster, no rays",
    "Hybrid, half, shadow maps",
    "Hybrid, half",
    "Hybrid, full",
    "Hybrid, half, probes",
    "DLSS-RR",
    "Path tracing",
};

/** @brief What presets 2-6 share: the hybrid rung, its denoiser, nothing experimental. */
auto hybridRung(const RenderOptions& options) -> bool
{
    const GpuRenderOptions& gpu = options.gpuRender;
    return options.rayTracing && gpu.pipeline == GpuPipeline::Hybrid && !gpu.radianceCache &&
           gpu.lightCandidates == 1 && gpu.denoiser.temporal && gpu.denoiser.spatial &&
           gpu.view == HybridView::Final;
}

}  // namespace

auto demoPresetName(int key) -> std::string_view
{
    if (key < 1 || key > kDemoPresetCount) {
        return {};
    }
    return kNames[static_cast<std::size_t>(key - 1)];
}

auto applyDemoPreset(RenderOptions& options, int key, Upscaler baseUpscaler) -> bool
{
    if (key < 1 || key > kDemoPresetCount) {
        return false;
    }
    GpuRenderOptions& gpu = options.gpuRender;
    // The rung, not whatever the panel last mixed in. Quality and the effects stay: the
    // demo compares the rungs, not the post-processing.
    gpu.radianceCache = false;
    gpu.lightCandidates = 1;
    gpu.probes.enabled = false;
    gpu.denoiser.temporal = true;
    gpu.denoiser.spatial = true;
    gpu.view = HybridView::Final;
    gpu.directShadows = DirectShadows::Traced;
    gpu.traceResolution = TraceResolution::Half;
    gpu.pipeline = GpuPipeline::Hybrid;
    gpu.upscaling.upscaler = baseUpscaler;
    options.rayTracing = true;

    switch (key) {
    case 1:
        options.rayTracing = false;
        break;
    case 2:
        gpu.directShadows = DirectShadows::ShadowMaps;
        break;
    case 3:
        break;
    case 4:
        gpu.traceResolution = TraceResolution::Full;
        break;
    case 5:
        gpu.probes.enabled = true;
        break;
    case 6:
        // DLSS-RR traces every pixel (TraceResolution::Full).
        gpu.traceResolution = TraceResolution::Full;
        gpu.upscaling.upscaler = Upscaler::DlssRr;
        break;
    case 7:
        gpu.pipeline = GpuPipeline::PathTracing;
        break;
    default:
        break;
    }
    return true;
}

auto matchDemoPreset(const RenderOptions& options) -> std::optional<int>
{
    if (!options.rayTracing) {
        return 1;
    }
    if (options.gpuRender.pipeline == GpuPipeline::PathTracing) {
        return 7;
    }
    if (!hybridRung(options)) {
        return std::nullopt;
    }
    const GpuRenderOptions& gpu = options.gpuRender;
    if (gpu.upscaling.upscaler == Upscaler::DlssRr) {
        // The pass traces every pixel whichever trace resolution the panel shows.
        if (gpu.directShadows == DirectShadows::Traced && !gpu.probes.enabled) {
            return 6;
        }
        return std::nullopt;
    }
    if (gpu.probes.enabled) {
        if (gpu.traceResolution == TraceResolution::Half &&
            gpu.directShadows == DirectShadows::Traced) {
            return 5;
        }
        return std::nullopt;
    }
    if (gpu.traceResolution == TraceResolution::Full &&
        gpu.directShadows == DirectShadows::Traced) {
        return 4;
    }
    if (gpu.traceResolution == TraceResolution::Half &&
        gpu.directShadows == DirectShadows::ShadowMaps) {
        return 2;
    }
    if (gpu.traceResolution == TraceResolution::Half &&
        gpu.directShadows == DirectShadows::Traced) {
        return 3;
    }
    return std::nullopt;
}

}  // namespace startraveler
