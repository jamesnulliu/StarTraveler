#pragma once

#include <string_view>

#include "game_layer.hpp"

namespace startraveler
{

inline auto demoPresetName(int preset) -> std::string_view
{
    switch (preset) {
    case 1: return "Raster shadows + ambient";
    case 2: return "Hybrid Half + shadow maps";
    case 3: return "Hybrid Half + traced shadows";
    case 4: return "Hybrid Full";
    case 5: return "Hybrid Half + irradiance probes";
    case 6: return "DLSS Ray Reconstruction";
    case 7: return "Progressive path tracer";
    default: return "Custom";
    }
}

// Presentation profiles, not the equal-resolution formal benchmark configurations.
inline auto applyDemoPreset(RenderOptions& options, int preset) -> bool
{
    using namespace starcore;
    if (preset < 1 || preset > 7) {
        return false;
    }
    options.gpu = true;
    options.liveRender = true;
    options.rayTracing = preset != 1;
    options.adaptiveGpu = false;
    options.gpuBudgetMs = 0.0F;
    options.gpuRender = {};
    options.gpuRender.pipeline = preset == 1 ? GpuPipeline::Raster :
                                preset == 7 ? GpuPipeline::PathTracing : GpuPipeline::Hybrid;
    options.gpuRender.traceResolution =
        preset == 2 || preset == 3 || preset == 5 ? TraceResolution::Half : TraceResolution::Full;
    options.gpuRender.directShadows =
        preset == 2 ? DirectShadows::ShadowMaps : DirectShadows::Traced;
    options.gpuRender.probes.enabled = preset == 5;
    options.gpuRender.upscaling.upscaler = preset == 6 ? Upscaler::DlssRr : Upscaler::Off;
    options.gpuRender.upscaling.quality = UpscalerQuality::Quality;
    options.gpuRender.upscaling.dlssPreset = DlssPreset::K;
    options.tracer = {};
    options.tracer.seed = 29;
    options.tracer.exposure = 1.0F;
    options.tracer.toneMapper = ToneMapper::Aces;
    options.particles = false;
    options.spaceDust = false;
    return true;
}

inline auto matchesDemoPreset(const RenderOptions& options, int preset) -> bool
{
    RenderOptions expected;
    if (!applyDemoPreset(expected, preset)) {
        return false;
    }
    return options.gpu == expected.gpu && options.liveRender == expected.liveRender &&
           options.rayTracing == expected.rayTracing && !options.adaptiveGpu &&
           options.gpuBudgetMs == 0.0F && options.gpuRender == expected.gpuRender &&
           options.tracer == expected.tracer && !options.particles && !options.spaceDust;
}

}  // namespace startraveler
