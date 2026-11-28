// The demo's keys set one rung each, and the overlay names only an unmixed rung.

#include <catch2/catch_test_macros.hpp>

#include <starcore/gpu/gpu_renderer.hpp>

#include "demo_presets.hpp"

using namespace starcore;
using namespace startraveler;

namespace
{

auto optionsAt(int key, Upscaler base = Upscaler::Dlss) -> RenderOptions
{
    RenderOptions options;
    REQUIRE(applyDemoPreset(options, key, base));
    return options;
}

}  // namespace

TEST_CASE("Each demo key sets its rung and the overlay can name it", "[demo]")
{
    for (int key = 1; key <= kDemoPresetCount; ++key) {
        const RenderOptions options = optionsAt(key);
        REQUIRE(matchDemoPreset(options) == key);
        REQUIRE_FALSE(demoPresetName(key).empty());
    }
    REQUIRE(demoPresetName(0).empty());
    RenderOptions untouched = optionsAt(3);
    REQUIRE_FALSE(applyDemoPreset(untouched, 0, Upscaler::Dlss));
    REQUIRE_FALSE(applyDemoPreset(untouched, 8, Upscaler::Dlss));
    REQUIRE(matchDemoPreset(untouched) == 3);
}

TEST_CASE("Demo presets leave quality and effects, and restore the upscaler", "[demo]")
{
    RenderOptions options;
    options.gpuRender.upscaling.quality = UpscalerQuality::Performance;
    options.gpuRender.upscaling.sharpness = 0.4F;
    options.gpuRender.effects.bloom = true;
    options.gpuRender.effects.bloomStrength = 0.2F;
    options.gpuRender.denoiser.historyFrames = 8;
    options.gpuRender.probes.spacing = 2.5F;
    options.tracer.samplesPerPixel = 3;

    REQUIRE(applyDemoPreset(options, 6, Upscaler::Fsr));
    REQUIRE(options.gpuRender.upscaling.upscaler == Upscaler::DlssRr);
    REQUIRE(options.gpuRender.traceResolution == TraceResolution::Full);
    REQUIRE(options.gpuRender.upscaling.quality == UpscalerQuality::Performance);
    REQUIRE(options.gpuRender.upscaling.sharpness == 0.4F);
    REQUIRE(options.gpuRender.effects.bloom);
    REQUIRE(options.gpuRender.effects.bloomStrength == 0.2F);
    REQUIRE(options.gpuRender.denoiser.historyFrames == 8);
    REQUIRE(options.gpuRender.probes.spacing == 2.5F);
    REQUIRE_FALSE(options.gpuRender.probes.enabled);
    REQUIRE(options.tracer.samplesPerPixel == 3);

    REQUIRE(applyDemoPreset(options, 5, Upscaler::Fsr));
    REQUIRE(options.gpuRender.upscaling.upscaler == Upscaler::Fsr);
    REQUIRE(options.gpuRender.probes.enabled);
    REQUIRE(options.gpuRender.traceResolution == TraceResolution::Half);
    REQUIRE(matchDemoPreset(options) == 5);

    REQUIRE(applyDemoPreset(options, 2, Upscaler::Fsr));
    REQUIRE(options.gpuRender.directShadows == DirectShadows::ShadowMaps);
    REQUIRE_FALSE(options.gpuRender.probes.enabled);
    REQUIRE(options.rayTracing);
    REQUIRE(matchDemoPreset(options) == 2);
}

TEST_CASE("A mixed render panel is not named as a demo preset", "[demo]")
{
    RenderOptions hybrid = optionsAt(3);
    hybrid.gpuRender.radianceCache = true;
    REQUIRE_FALSE(matchDemoPreset(hybrid).has_value());

    RenderOptions probes = optionsAt(5);
    probes.gpuRender.traceResolution = TraceResolution::Full;
    REQUIRE_FALSE(matchDemoPreset(probes).has_value());

    RenderOptions view = optionsAt(4);
    view.gpuRender.view = HybridView::Albedo;
    REQUIRE_FALSE(matchDemoPreset(view).has_value());

    // The game starts on preset 3 (hybrid, half, traced shadows).
    RenderOptions fresh;
    fresh.gpuRender.upscaling.upscaler = Upscaler::Dlss;
    REQUIRE(matchDemoPreset(fresh) == 3);

    RenderOptions raster;
    raster.rayTracing = false;
    REQUIRE(matchDemoPreset(raster) == 1);

    // `--upscaler DLSS-RR` leaves the game's Half setting; the pass still traces every pixel.
    RenderOptions rayReconstruction = fresh;
    rayReconstruction.gpuRender.upscaling.upscaler = Upscaler::DlssRr;
    REQUIRE(matchDemoPreset(rayReconstruction) == 6);
}
