#include <catch2/catch_test_macros.hpp>

#include "demo_presets.hpp"

using namespace startraveler;
using namespace starcore;

TEST_CASE("Presentation profiles clear previous rendering experiments", "[demo]")
{
    for (int preset = 1; preset <= 7; ++preset) {
        RenderOptions options;
        options.gpuRender.radianceCache = true;
        options.gpuRender.probes.enabled = true;
        options.gpuRender.lightCandidates = 4;
        options.gpuRender.denoiser.temporal = false;
        options.gpuRender.denoiser.spatial = false;
        options.gpuRender.upscaling.dlssExposure = 4.0F;
        options.tracer.autoExposure = true;
        REQUIRE(applyDemoPreset(options, preset));
        CHECK(matchesDemoPreset(options, preset));
        CHECK(options.gpu);
        CHECK(options.liveRender);
        CHECK(options.gpuBudgetMs == 0.0F);
        CHECK_FALSE(options.adaptiveGpu);
        CHECK_FALSE(options.tracer.autoExposure);
        CHECK(options.tracer.exposure == 1.0F);
        CHECK(options.tracer.toneMapper == ToneMapper::Aces);
        CHECK(options.tracer.samplesPerPixel == 1);
        CHECK(options.tracer.accumulate);
        CHECK_FALSE(options.gpuRender.radianceCache);
        CHECK(options.gpuRender.lightCandidates == 1);
        CHECK(options.gpuRender.effects == EffectSettings{});
        CHECK(options.gpuRender.denoiser.temporal);
        CHECK(options.gpuRender.denoiser.spatial);
        CHECK_FALSE(options.particles);
        CHECK_FALSE(options.spaceDust);
        CHECK(options.gpuRender.probes.enabled == (preset == 5));
        CHECK(options.gpuRender.upscaling.upscaler ==
              (preset == 6 ? Upscaler::DlssRr : Upscaler::Off));
        CHECK(options.rayTracing == (preset != 1));
    }
}

TEST_CASE("Presentation rungs select their specified pipelines and ray budgets", "[demo]")
{
    RenderOptions options;
    REQUIRE(applyDemoPreset(options, 1));
    CHECK(options.gpuRender.pipeline == GpuPipeline::Raster);
    REQUIRE(applyDemoPreset(options, 2));
    CHECK(options.gpuRender.pipeline == GpuPipeline::Hybrid);
    CHECK(options.gpuRender.traceResolution == TraceResolution::Half);
    CHECK(options.gpuRender.directShadows == DirectShadows::ShadowMaps);
    REQUIRE(applyDemoPreset(options, 3));
    CHECK(options.gpuRender.directShadows == DirectShadows::Traced);
    REQUIRE(applyDemoPreset(options, 4));
    CHECK(options.gpuRender.traceResolution == TraceResolution::Full);
    REQUIRE(applyDemoPreset(options, 7));
    CHECK(options.gpuRender.pipeline == GpuPipeline::PathTracing);
    CHECK(options.tracer.accumulate);
}

TEST_CASE("Presentation label becomes custom after manual render changes", "[demo]")
{
    RenderOptions options;
    REQUIRE(applyDemoPreset(options, 4));
    options.tracer.exposure = 2.0F;
    CHECK_FALSE(matchesDemoPreset(options, 4));
    REQUIRE(applyDemoPreset(options, 6));
    options.gpuRender.upscaling.upscaler = Upscaler::Off;
    CHECK_FALSE(matchesDemoPreset(options, 6));
    const auto before = options.gpuRender;
    CHECK_FALSE(applyDemoPreset(options, 0));
    CHECK_FALSE(applyDemoPreset(options, 8));
    CHECK(options.gpuRender == before);
}
