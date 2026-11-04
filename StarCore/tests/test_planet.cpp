// Planets in the sky (starcore/render/planet.hpp): how they are lit and composited over what
// is behind them, and the planets of a render scene.

#include <algorithm>
#include <cmath>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <glm/geometric.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/matrix.hpp>

#include <starcore/render/planet.hpp>
#include <starcore/render/render_scene.hpp>
#include <starcore/scene/components.hpp>
#include <starcore/scene/entity.hpp>
#include <starcore/scene/scene.hpp>
#include <starcore/scene/serializer.hpp>

using namespace starcore;
using Catch::Approx;

namespace
{

constexpr float kDegree = kPi / 180.0F;

auto luminance(Vec3 color) -> float
{
    return glm::dot(color, Vec3(0.2126F, 0.7152F, 0.0722F));
}

/** @brief A unit planet 10 away down -z, lit from +x, of one gray. */
auto testPlanet(PlanetKind kind) -> RenderPlanet
{
    RenderPlanet planet;
    planet.center = {0.0F, 0.0F, -10.0F};
    planet.radius = 1.0F;
    planet.look.kind = kind;
    planet.look.color = Vec3(0.5F);
    planet.look.secondaryColor = Vec3(0.5F);
    planet.sunDirection = {1.0F, 0.0F, 0.0F};
    planet.sunIrradiance = Vec3(3.0F);
    return planet;
}

/** @brief The direction from the origin to a point of the planet's frame (in its radii). */
auto towards(const RenderPlanet& planet, Vec3 local) -> Vec3
{
    const Vec3 offset = glm::transpose(planet.worldToLocal) * local * planet.radius;
    return glm::normalize(planet.center + offset);
}

}  // namespace

TEST_CASE("Planets: lit by their sun, over what is behind them", "[planet][sky]")
{
    RenderPlanet planet = testPlanet(PlanetKind::Gas);
    const Vec3 behind{0.2F, 0.3F, 0.4F};
    // Elsewhere: what is behind.
    CHECK(planetRadiance(planet, glm::normalize(Vec3(0.0F, 0.3F, -1.0F)), behind) == behind);
    CHECK(planetRadiance(planet, {0.0F, 0.0F, 1.0F}, behind) == behind);
    // Its night side hides what is behind.
    CHECK(planetRadiance(planet, towards(planet, {-0.6F, 0.0F, 0.8F}), behind) == Vec3(0.0F));
    // Its day side: Lambertian (a gas giant), its albedo some 0.5 (its bands vary it).
    const float mu = 0.6F;
    const Vec3 lit = planetRadiance(planet, towards(planet, {0.6F, 0.0F, 0.8F}), behind);
    CHECK(luminance(lit) > 0.3F * 3.0F * mu / kPi);
    CHECK(luminance(lit) < 0.7F * 3.0F * mu / kPi);
    // Without a sun, a dark disc.
    planet.sunIrradiance = Vec3(0.0F);
    CHECK(planetRadiance(planet, towards(planet, {0.6F, 0.0F, 0.8F}), behind) == Vec3(0.0F));
}

TEST_CASE("Planets: every kind is lit within its albedo", "[planet][sky]")
{
    for (const PlanetKind kind :
         {PlanetKind::Rocky, PlanetKind::Terrestrial, PlanetKind::Gas, PlanetKind::Ice}) {
        RenderPlanet planet = testPlanet(kind);
        planet.look.color = {0.4F, 0.5F, 0.3F};
        planet.look.secondaryColor = {0.05F, 0.1F, 0.2F};
        planet.sunDirection = {0.0F, 0.0F, 1.0F};  // Full: lit as seen.
        INFO(enumToString(kind));
        float darkest = 1e9F;
        float brightest = 0.0F;
        for (float x = -0.9F; x <= 0.9F; x += 0.05F) {
            for (float y = -0.9F; y <= 0.9F; y += 0.05F) {
                if (x * x + y * y > 0.81F) {
                    continue;
                }
                const Vec3 local{x, y, std::sqrt(1.0F - x * x - y * y)};
                const Vec3 radiance = planetRadiance(planet, towards(planet, local), Vec3(0.0F));
                REQUIRE(std::isfinite(radiance.r + radiance.g + radiance.b));
                darkest = std::min(darkest, luminance(radiance));
                brightest = std::max(brightest, luminance(radiance));
            }
        }
        // Albedos up to 1 (clouds, fresh craters; the view's rays fan out a little from the
        // sun's), and a glint of the sun on water.
        CHECK(darkest > 0.0F);
        CHECK(brightest < (kind == PlanetKind::Terrestrial ? 3.0F : 1.2F) * 3.0F / kPi);
        CHECK(brightest > 1.5F * darkest);  // Not one color: a surface.
    }
}

TEST_CASE("Planets: an atmosphere scatters and dims", "[planet][sky]")
{
    RenderPlanet planet = testPlanet(PlanetKind::Rocky);
    planet.look.atmosphere = 0.3F;
    planet.look.atmosphereColor = {0.18F, 0.42F, 1.0F};
    planet.look.atmosphereHeight = 0.02F;
    const float height = planet.look.atmosphereHeight;
    // Just above the limb on the day side: a blue glow against black space.
    const Vec3 limb = planetRadiance(planet, towards(planet, {1.0F + 2.0F * height, 0.05F, 0.0F}),
                                     Vec3(0.0F));
    CHECK(luminance(limb) > 0.01F);
    CHECK(limb.b > limb.r);
    // Through the night side's limb: what is behind, dimmer and redder (no sunlight there).
    const Vec3 through = planetRadiance(
        planet, towards(planet, {-1.0F - 1.0F * height, 0.05F, 0.0F}), Vec3(1.0F));
    CHECK(through.r < 1.0F);
    CHECK(through.b < 0.9F * through.r);
    // Above its top: nothing.
    CHECK(planetRadiance(planet, towards(planet, {1.0F + 9.0F * height, 0.0F, 0.0F}),
                         Vec3(0.5F)) == Vec3(0.5F));

    // Backlit (its sun behind it), haze scatters forward: a ring of light around it.
    planet.sunDirection = {0.0F, 0.0F, -1.0F};
    auto rim = [&](float haze) {
        planet.look.haze = haze;
        return luminance(planetRadiance(
            planet, towards(planet, {0.0F, 1.0F + 1.5F * height, 0.0F}), Vec3(0.0F)));
    };
    CHECK(rim(0.8F) > 3.0F * rim(0.0F));
}

TEST_CASE("Planets: rings, shadowed by their planet", "[planet][sky]")
{
    RenderPlanet planet = testPlanet(PlanetKind::Gas);
    planet.look.ringInner = 1.5F;
    planet.look.ringOuter = 2.3F;
    planet.look.ringOpacity = 0.9F;
    // Tilted 30 degrees towards the viewer; its sun above the rings' plane, to its +x.
    const Mat3 localToWorld =
        glm::mat3_cast(glm::angleAxis(30.0F * kDegree, Vec3(1.0F, 0.0F, 0.0F)));
    planet.worldToLocal = glm::transpose(localToWorld);
    planet.sunDirection = localToWorld * glm::normalize(Vec3(1.0F, 0.5F, 0.0F));
    // Across the rings on the sunny side, and on the side in the planet's shadow.
    int seen = 0;
    int shadowed = 0;
    int samples = 0;
    for (float r = 1.55F; r < 2.25F; r += 0.02F, ++samples) {
        const Vec3 sunny = planetRadiance(planet, towards(planet, {r, 0.0F, 0.0F}), Vec3(0.0F));
        const Vec3 shade = planetRadiance(planet, towards(planet, {-r, 0.0F, 0.0F}), Vec3(0.0F));
        seen += luminance(sunny) > 1e-3F ? 1 : 0;
        shadowed += luminance(shade) < 0.05F * luminance(sunny) ? 1 : 0;
        CHECK(luminance(sunny) < 3.0F / kPi);
    }
    CHECK(seen > samples * 3 / 4);
    CHECK(shadowed >= seen * 3 / 4);
    // They hide part of what is behind them; beyond them, nothing.
    planet.sunIrradiance = Vec3(0.0F);
    CHECK(luminance(planetRadiance(planet, towards(planet, {1.8F, 0.0F, 0.0F}), Vec3(1.0F))) <
          1.0F);
    CHECK(planetRadiance(planet, towards(planet, {2.4F, 0.0F, 0.0F}), Vec3(1.0F)) == Vec3(1.0F));
}

TEST_CASE("Planets: a render scene's planets", "[planet][sky][render]")
{
    Scene scene;
    scene.environment().skyColor = Vec3(0.0F);
    // A sun far away to +x, and a dim lamp nearer the planet (lighting it less).
    Entity sun = scene.createEntity("Sun");
    sun.get<TransformComponent>().translation = {10000.0F, 0.0F, 0.0F};
    sun.add<SphereComponent>(100.0F);
    sun.add<MaterialComponent>(MaterialComponent{
        .albedo = Vec3(0.0F), .emissionColor = Vec3(1.0F), .emissionPower = 50.0F});
    Entity lamp = scene.createEntity("Lamp");
    lamp.get<TransformComponent>().translation = {0.0F, 0.0F, -3000.0F};
    lamp.add<SphereComponent>(1.0F);
    lamp.add<MaterialComponent>(
        MaterialComponent{.emissionColor = Vec3(1.0F), .emissionPower = 10.0F});
    Entity world = scene.createEntity("World");
    auto& transform = world.get<TransformComponent>();
    transform.translation = {0.0F, 0.0F, -5000.0F};
    transform.rotation = glm::angleAxis(20.0F * kDegree, Vec3(0.0F, 0.0F, 1.0F));
    transform.scale = Vec3(2.0F);
    world.add<PlanetComponent>(PlanetComponent{.radius = 500.0F});

    RenderSceneBuilder builder;
    const RenderScene& renderScene = builder.update(scene);
    REQUIRE(renderScene.sky.planets.size() == 1);
    const RenderPlanet& planet = renderScene.sky.planets[0];
    CHECK(planet.center == Vec3(0.0F, 0.0F, -5000.0F));
    CHECK(planet.radius == Approx(1000.0F));
    // Its spin axis, tilted with the entity.
    const Vec3 axis = glm::transpose(planet.worldToLocal) * Vec3(0.0F, 1.0F, 0.0F);
    CHECK(axis.x == Approx(-std::sin(20.0F * kDegree)).margin(1e-5));
    CHECK(axis.y == Approx(std::cos(20.0F * kDegree)).margin(1e-5));
    // Lit by the sun, from where it is: its light there is its radiance times its solid angle.
    const Vec3 toSun = Vec3(10000.0F, 0.0F, 5000.0F);
    CHECK(glm::length(planet.sunDirection - glm::normalize(toSun)) < 1e-5F);
    const float sine2 = 100.0F * 100.0F / glm::dot(toSun, toSun);
    const float solidAngle = kTwoPi * (1.0F - std::sqrt(1.0F - sine2));
    CHECK(planet.sunIrradiance.r == Approx(50.0F * solidAngle).epsilon(1e-3));
    // In the sky, towards its day side.
    CHECK(luminance(skyRadiance(renderScene, glm::normalize(Vec3(800.0F, 0.0F, -5000.0F)))) > 0.0F);
    CHECK(skyRadiance(renderScene, {0.0F, 1.0F, 0.0F}) == Vec3(0.0F));

    // Unchanged, the same sky; changed, another one.
    const std::uint64_t version = renderScene.skyVersion;
    scene.markDirty();
    builder.update(scene);
    CHECK(renderScene.skyVersion == version);
    world.get<PlanetComponent>().look.seed = 9;
    scene.markDirty();
    builder.update(scene);
    CHECK(renderScene.skyVersion != version);

    // The nearest four, farthest first.
    for (int i = 0; i < 4; ++i) {
        Entity more = scene.createEntity("More");
        const float z = -2000.0F * static_cast<float>(i + 1);
        more.get<TransformComponent>().translation = {0.0F, 0.0F, z};
        more.add<PlanetComponent>(PlanetComponent{.radius = 100.0F});
    }
    builder.update(scene);
    REQUIRE(renderScene.sky.planets.size() == kMaxPlanets);
    for (std::size_t i = 0; i + 1 < kMaxPlanets; ++i) {
        CHECK(glm::length(renderScene.sky.planets[i].center) >
              glm::length(renderScene.sky.planets[i + 1].center));
    }
    // Nearest (to their surfaces): 1900, 3900, 4000 (the world), 5900; the one 7900 away dropped.
    CHECK(renderScene.sky.planets[0].center.z == -6000.0F);
    CHECK(renderScene.sky.planets[1].center.z == -5000.0F);
}

TEST_CASE("Planets are saved with the scene", "[planet][serializer]")
{
    Scene scene;
    Entity world = scene.createEntity("World");
    PlanetComponent planet{.radius = 1234.0F};
    planet.look = PlanetLook{.kind = PlanetKind::Terrestrial,
                             .seed = 7,
                             .color = {0.1F, 0.2F, 0.3F},
                             .secondaryColor = {0.01F, 0.02F, 0.03F},
                             .atmosphere = 0.3F,
                             .atmosphereColor = {0.2F, 0.4F, 1.0F},
                             .atmosphereHeight = 0.01F,
                             .haze = 0.25F,
                             .ringInner = 1.4F,
                             .ringOuter = 2.2F,
                             .ringOpacity = 0.6F,
                             .ringColor = {0.7F, 0.6F, 0.5F}};
    world.add<PlanetComponent>(planet);
    Scene loaded;
    REQUIRE(deserializeScene(serializeScene(scene), loaded).has_value());
    const Entity copy = loaded.findEntityByName("World");
    REQUIRE(copy);
    const auto& read = copy.get<PlanetComponent>();
    CHECK(read.radius == 1234.0F);
    CHECK(read.look == planet.look);
}
