// The camera: AgX tone mapping and automatic exposure (render/exposure.hpp).

#include <array>
#include <cmath>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <glm/geometric.hpp>

#include <starcore/math/color.hpp>
#include <starcore/render/exposure.hpp>
#include <starcore/render/path_tracer.hpp>
#include <starcore/scene/scene.hpp>

using namespace starcore;
using Catch::Approx;

namespace
{

auto luminance(Vec3 color) -> float
{
    return glm::dot(color, Vec3(0.2126F, 0.7152F, 0.0722F));
}

/** @brief A histogram of `pixels` at each luminance. */
template <std::size_t N>
auto histogramOf(const std::array<std::pair<float, std::uint32_t>, N>& pixels)
    -> std::array<std::uint32_t, kExposureBins>
{
    std::array<std::uint32_t, kExposureBins> histogram{};
    for (const auto& [value, count] : pixels) {
        histogram[exposureBin(value)] += count;
    }
    return histogram;
}

}  // namespace

TEST_CASE("AgX: film-like tone mapping", "[color]")
{
    for (const ToneMapper mapper : {ToneMapper::AgX, ToneMapper::AgXPunchy}) {
        CHECK(luminance(toneMap(Vec3(0.0F), mapper)) < 1e-3F);
        // Monotonic over its range, white at its top.
        float previous = -1.0F;
        for (float stops = -12.0F; stops <= 4.0F; stops += 0.5F) {
            const float display = luminance(toneMap(Vec3(std::exp2(stops)), mapper));
            CHECK(display >= previous);
            previous = display;
        }
        CHECK(previous > 0.9F);
        // Bright saturated light goes towards white instead of clipping to its hue.
        const Vec3 red = toneMap(Vec3(40.0F, 0.0F, 0.0F), mapper);
        CHECK(red.g > 0.3F * red.r);
    }
    // Middle gray in the middle; the Punchy look darker below it.
    const float gray = luminance(toneMap(Vec3(0.18F), ToneMapper::AgX));
    CHECK(gray > 0.12F);
    CHECK(gray < 0.3F);
    CHECK(luminance(toneMap(Vec3(0.02F), ToneMapper::AgXPunchy)) <
          luminance(toneMap(Vec3(0.02F), ToneMapper::AgX)));
}

TEST_CASE("Auto exposure: metering", "[exposure]")
{
    CHECK(exposureBin(0.0F) == 0);
    CHECK(exposureBin(std::nanf("")) == 0);
    CHECK(exposureBin(1e6F) == kExposureBins - 1);
    CHECK(meteredExposure(std::array<std::uint32_t, kExposureBins>{}) == 1.0F);

    // A view of one brightness is exposed to middle gray (within a bin, a third of a stop).
    const float bin = std::exp2((kExposureMaxLog2 - kExposureMinLog2) / (kExposureBins - 1));
    for (const float radiance : {0.02F, 0.2F, 2.0F}) {
        const auto histogram = histogramOf(std::array{std::pair{radiance, 1000U}});
        CHECK(meteredExposure(histogram) * radiance == Approx(kExposureKey).epsilon(bin - 1.0F));
    }
    // Half of it black space, highlights in a few percent: the rest is metered...
    const auto histogram =
        histogramOf(std::array{std::pair{0.0F, 500U}, std::pair{0.5F, 470U},
                               std::pair{20.0F, 30U}});
    CHECK(meteredExposure(histogram) * 0.5F == Approx(kExposureKey).epsilon(bin - 1.0F));
    // ... unless they are a sun, whose glare floods the view: 1% at 4000, its glare 2.
    const auto sun = histogramOf(std::array{std::pair{0.0F, 500U}, std::pair{0.5F, 490U},
                                            std::pair{4000.0F, 10U}});
    const float glare = kVeilingGlare * (490.0F * 0.5F + 10.0F * 4000.0F) / 1000.0F;
    CHECK(meteredExposure(sun) * glare == Approx(kExposureKey).epsilon(bin - 1.0F));
    // Within the camera's range.
    CHECK(meteredExposure(histogramOf(std::array{std::pair{1e-5F, 10U}})) == kMaxExposure);
    CHECK(meteredExposure(histogramOf(std::array{std::pair{50.0F, 10U}})) == kMinExposure);
}

TEST_CASE("Auto exposure: adaptation", "[exposure]")
{
    CHECK(adaptExposure(1.0F, 4.0F, 0.0F) == Approx(1.0F));
    CHECK(adaptExposure(1.0F, 4.0F, 100.0F) == Approx(4.0F));
    // Halfway in stops after ln 2 / rate seconds; darker faster than brighter.
    CHECK(adaptExposure(1.0F, 4.0F, std::log(2.0F) / kBrighteningRate) == Approx(2.0F));
    CHECK(adaptExposure(4.0F, 1.0F, std::log(2.0F) / kDarkeningRate) == Approx(2.0F));
    CHECK(std::log2(4.0F / adaptExposure(4.0F, 1.0F, 0.2F)) >
          std::log2(adaptExposure(1.0F, 4.0F, 0.2F)));
}

TEST_CASE("Auto exposure: the CPU path tracer meters its image", "[exposure][render]")
{
    Scene scene;
    scene.environment().skyColor = Vec3(0.5F);
    const RenderScene renderScene = buildRenderScene(scene);
    Camera camera;
    camera.setViewport(32, 24);
    CpuPathTracer tracer(1);
    tracer.resize(32, 24);
    PathTracerSettings settings{.mode = RenderMode::Preview, .autoExposure = true};
    tracer.render(renderScene, camera, settings);
    // 0.5 exposed to middle gray (sRGB 118), within a third of a stop.
    const int value = static_cast<int>(tracer.image().at(16, 12) & 0xFFU);
    CHECK(value > 108);
    CHECK(value < 128);
    // A compensation of two stops.
    settings.exposure = 4.0F;
    tracer.render(renderScene, camera, settings);
    CHECK(static_cast<int>(tracer.image().at(16, 12) & 0xFFU) > 190);
}
