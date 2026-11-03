// The space sky (starcore/render/sky.hpp): its stars' counts and colors, the Milky Way's
// shape, the map's lookup, and the sky of a render scene.

#include <algorithm>
#include <cmath>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <glm/geometric.hpp>
#include <glm/matrix.hpp>

#include <starcore/render/render_scene.hpp>
#include <starcore/render/sky.hpp>
#include <starcore/scene/scene.hpp>

using namespace starcore;
using Catch::Approx;

namespace
{

constexpr float kDegree = kPi / 180.0F;

auto luminance(Vec3 color) -> float
{
    return glm::dot(color, Vec3(0.2126F, 0.7152F, 0.0722F));
}

auto galactic(float longitude, float latitude) -> Vec3
{
    const float l = longitude * kDegree;
    const float b = latitude * kDegree;
    return {std::cos(b) * std::cos(l), std::sin(b), std::cos(b) * std::sin(l)};
}

/** @brief Mean luminance of a map over a patch of galactic coordinates (degrees). */
auto patchLuminance(const SkyMap& map, float l0, float l1, float b0, float b1) -> float
{
    constexpr int kSteps = 24;
    float sum = 0.0F;
    for (int i = 0; i < kSteps; ++i) {
        for (int j = 0; j < kSteps; ++j) {
            const float l = std::lerp(l0, l1, (static_cast<float>(i) + 0.5F) / kSteps);
            const float b = std::lerp(b0, b1, (static_cast<float>(j) + 0.5F) / kSteps);
            sum += luminance(map.radiance(galactic(l, b)));
        }
    }
    return sum / (kSteps * kSteps);
}

}  // namespace

TEST_CASE("Sky: shared-exponent colors", "[sky]")
{
    for (const Vec3 color : {Vec3(0.0F), Vec3(1.0F, 0.5F, 0.25F), Vec3(1e-4F, 2e-4F, 3e-5F),
                             Vec3(300.0F, 20.0F, 1.0F)}) {
        const Vec3 decoded = decodeRgb9e5(encodeRgb9e5(color));
        const float largest = std::max({color.r, color.g, color.b});
        for (int c = 0; c < 3; ++c) {
            CHECK(std::abs(decoded[c] - color[c]) <= largest / 256.0F);
        }
    }
}

TEST_CASE("Sky: star colors from temperatures", "[sky]")
{
    const Vec3 cool = blackbodyColor(3000.0F);
    const Vec3 sun = blackbodyColor(5800.0F);
    const Vec3 hot = blackbodyColor(15000.0F);
    CHECK(cool.r > 1.5F * cool.b);
    CHECK(hot.b > 1.2F * hot.r);
    CHECK(luminance(sun) == Approx(1.0F).margin(1e-4F));
    // The sun is near white.
    CHECK(std::min({sun.r, sun.g, sun.b}) > 0.75F * std::max({sun.r, sun.g, sun.b}));
}

TEST_CASE("Sky: stars counted as in the real sky", "[sky]")
{
    CHECK(starsBrighterThan(6.0F) == Approx(5000.0).epsilon(0.1));
    CHECK(starsBrighterThan(8.0F) / starsBrighterThan(7.0F) == Approx(2.8).epsilon(0.03));
    CHECK(starFlux(0.0F) / starFlux(5.0F) == Approx(100.0F).epsilon(1e-3));

    const auto map =
        generateSkyMap(SpaceSky{.enabled = true, .seed = 7, .starMagnitude = 6.0F}, 512);
    // Drawn: the stars down to magnitude 6 (some dimmed past it by dust), brightest first.
    const auto count = static_cast<double>(map->stars.size());
    CHECK(count > 0.85 * starsBrighterThan(6.0F));
    CHECK(count < 1.05 * starsBrighterThan(6.0F));
    CHECK(std::ranges::is_sorted(map->stars, {}, &SkyStar::magnitude));
    CHECK(map->stars.back().magnitude <= 6.0F);
    CHECK(std::ranges::all_of(map->stars, [](const SkyStar& star) {
        return std::abs(luminance(star.color) - 1.0F) < 1e-3F &&
               std::abs(glm::length(star.direction) - 1.0F) < 1e-4F;
    }));

    // The faint ones crowd towards the band, the bright ones hardly: the share of them within
    // 10 degrees of it (an even spread: sin 10 degrees, 0.17).
    const auto deeper =
        generateSkyMap(SpaceSky{.enabled = true, .seed = 7, .starMagnitude = 8.0F}, 512);
    auto shareNearBand = [&](float brightest, float faintest) {
        std::size_t stars = 0;
        std::size_t nearBand = 0;
        for (const SkyStar& star : deeper->stars) {
            if (star.magnitude > brightest && star.magnitude <= faintest) {
                ++stars;
                nearBand += std::abs(star.direction.y) < std::sin(10.0F * kDegree) ? 1 : 0;
            }
        }
        return static_cast<double>(nearBand) / static_cast<double>(stars);
    };
    const double bright = shareNearBand(-10.0F, 5.0F);
    const double faint = shareNearBand(7.0F, 8.0F);
    INFO("within 10 degrees of the band: bright stars " << bright << ", faint " << faint);
    CHECK(bright < 1.25 * std::sin(10.0 * kDegree));
    CHECK(faint > 1.4 * std::sin(10.0 * kDegree));
}

TEST_CASE("Sky: the Milky Way's band and center on a black sky", "[sky]")
{
    const auto map = generateSkyMap(SpaceSky{.enabled = true, .seed = 7}, 1024);
    const float center = patchLuminance(*map, -12.0F, 12.0F, -10.0F, 10.0F);
    const float band = patchLuminance(*map, 60.0F, 120.0F, -4.0F, 4.0F);
    const float anticenter = patchLuminance(*map, 160.0F, 200.0F, -4.0F, 4.0F);
    const float pole = patchLuminance(*map, -180.0F, 180.0F, 70.0F, 90.0F);
    INFO("center " << center << ", band " << band << ", anticenter " << anticenter
                   << ", pole " << pole);
    CHECK(center > band);
    CHECK(band > anticenter);
    CHECK(anticenter > 5.0F * pole);
    CHECK(pole < 0.002F);  // Space is black between the stars.
    // The mean counts every direction (and the drawn stars).
    CHECK(luminance(map->meanRadiance) > pole);
    CHECK(luminance(map->meanRadiance) < band);
    // A brighter Milky Way, the same stars.
    const auto brighter = generateSkyMap(SpaceSky{.enabled = true, .seed = 7, .milkyWay = 2.0F},
                                         1024);
    CHECK(patchLuminance(*brighter, 60.0F, 120.0F, -4.0F, 4.0F) > 1.8F * band);
    CHECK(brighter->stars.size() == map->stars.size());
}

TEST_CASE("Sky: the map's lookup", "[sky]")
{
    constexpr std::uint32_t kWidth = 256;
    const auto map = generateSkyMap(SpaceSky{.enabled = true, .seed = 9}, kWidth);
    REQUIRE(map->width == kWidth);
    REQUIRE(map->height == kWidth / 2);
    // The same settings, the same map (shared); another seed, another one.
    CHECK(generateSkyMap(SpaceSky{.enabled = true, .seed = 9}, kWidth) == map);
    CHECK(generateSkyMap(SpaceSky{.enabled = true, .seed = 10}, kWidth)->texels != map->texels);

    // At a texel's center, that texel.
    constexpr std::uint32_t x = 100;
    constexpr std::uint32_t y = 60;
    const float longitude = (static_cast<float>(x) + 0.5F) / kWidth * 360.0F - 180.0F;
    const float latitude = 90.0F - (static_cast<float>(y) + 0.5F) / (kWidth / 2) * 180.0F;
    const Vec3 texel = decodeRgb9e5(map->texels[y * kWidth + x]);
    const Vec3 looked = map->radiance(galactic(longitude, latitude));
    for (int c = 0; c < 3; ++c) {
        CHECK(looked[c] == Approx(texel[c]).epsilon(1e-2));
    }
    // Continuous across longitude 180 (the map's left and right edges).
    const Vec3 left = map->radiance(galactic(-179.999F, 5.0F));
    const Vec3 right = map->radiance(galactic(179.999F, 5.0F));
    CHECK(glm::length(left - right) <= 1e-3F * glm::length(left));
}

TEST_CASE("Sky: a render scene's sky", "[sky][render]")
{
    Scene scene;
    scene.environment().skyColor = {0.01F, 0.0F, 0.0F};
    RenderSceneBuilder builder;
    const RenderScene& renderScene = builder.update(scene);
    CHECK(renderScene.sky.map == nullptr);
    CHECK(skyRadiance(renderScene, {0.0F, 0.0F, -1.0F}) == scene.environment().skyColor);
    CHECK(skyAmbient(renderScene) == scene.environment().skyColor);
    const std::uint64_t plainVersion = renderScene.skyVersion;

    scene.environment().space = SpaceSky{
        .enabled = true, .seed = 4, .brightness = 2.0F, .orientation = {0.0F, 90.0F, 0.0F}};
    scene.markDirty();
    builder.update(scene);
    REQUIRE(renderScene.sky.map != nullptr);
    CHECK(renderScene.skyVersion != plainVersion);
    // A yaw of 90 degrees turns the galactic center (galactic +x) to world -z.
    const Vec3 toCenter = glm::transpose(renderScene.sky.worldToGalactic) * Vec3(1.0F, 0.0F, 0.0F);
    CHECK(toCenter.z == Approx(-1.0F).margin(1e-5F));
    // The sky's color plus the map times the brightness.
    const Vec3 expected =
        scene.environment().skyColor + renderScene.sky.map->radiance({1.0F, 0.0F, 0.0F}) * 2.0F;
    const Vec3 radiance = skyRadiance(renderScene, toCenter);
    const Vec3 ambient = skyAmbient(renderScene);
    for (int c = 0; c < 3; ++c) {
        CHECK(radiance[c] == Approx(expected[c]).epsilon(1e-3));
        CHECK(ambient[c] == Approx(scene.environment().skyColor[c] +
                                   renderScene.sky.map->meanRadiance[c] * 2.0F));
    }

    // A brighter sky keeps the map; another seed makes another one.
    const SkyMap* map = renderScene.sky.map.get();
    const std::uint64_t spaceVersion = renderScene.skyVersion;
    scene.environment().space.brightness = 3.0F;
    scene.markDirty();
    builder.update(scene);
    CHECK(renderScene.sky.map.get() == map);
    CHECK(renderScene.skyVersion != spaceVersion);
    scene.environment().space.seed = 5;
    scene.markDirty();
    builder.update(scene);
    CHECK(renderScene.sky.map.get() != map);
}
