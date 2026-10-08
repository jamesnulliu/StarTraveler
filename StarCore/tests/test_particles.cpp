#include <algorithm>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <starcore/render/particles.hpp>
#include <starcore/scene/entity.hpp>
#include <starcore/scene/scene.hpp>
#include <starcore/scene/serializer.hpp>

using namespace starcore;
using Catch::Matchers::WithinAbs;

namespace
{

/** @brief Straight along local -Z at 1 unit/s, living 10 s. */
auto straightEmitter(float rate) -> ParticleEmitterComponent
{
    return ParticleEmitterComponent{
        .particles = {.lifetime = {10.0F, 10.0F}, .speed = {1.0F, 1.0F}, .spread = 0.0F},
        .rate = rate,
        .inheritVelocity = false,
    };
}

}  // namespace

TEST_CASE("Particle emitters emit at their rate along their -Z", "[particles]")
{
    Scene scene;
    Entity engine = scene.createEntity("Engine");
    engine.add<ParticleEmitterComponent>(straightEmitter(100.0F));
    ParticleSystem system;
    for (int i = 0; i < 10; ++i) {
        system.update(scene, 0.05F);
    }
    REQUIRE(system.size() == 50);  // 0.5 s at 100 per second.
    for (const RenderParticle& particle : system.particles()) {
        REQUIRE(particle.position.z <= 0.0F);
        REQUIRE_THAT(particle.position.x, WithinAbs(0.0, 1e-5));
        REQUIRE_THAT(particle.position.y, WithinAbs(0.0, 1e-5));
        // A step ago it was 0.05 closer (or it was born during the step).
        REQUIRE(particle.previousPosition.z >= particle.position.z);
        REQUIRE(particle.previousPosition.z - particle.position.z <= 0.05F + 1e-5F);
    }
    // The oldest left at the first update: 0.5 s at 1 unit/s.
    const float farthest = std::ranges::min(
        system.particles(), {}, [](const RenderParticle& p) { return p.position.z; }).position.z;
    REQUIRE_THAT(farthest, WithinAbs(-0.5, 0.05));

    // Disabled emitters stop; the particles live on.
    engine.get<ParticleEmitterComponent>().enabled = false;
    system.update(scene, 0.5F);
    REQUIRE(system.size() == 50);
}

TEST_CASE("Particles age, shrink, change color and die", "[particles]")
{
    Scene scene;
    ParticleSystem system;
    const ParticleSettings settings{.lifetime = {0.2F, 0.2F},
                                    .speed = {0.0F, 0.0F},
                                    .size = {1.0F, 0.0F},
                                    .startColor = Vec3(2.0F),
                                    .endColor = Vec3(0.0F)};
    system.emit(settings, Vec3(1.0F), Vec3(0.0F, 0.0F, -1.0F), Vec3(0.0F), 5);
    REQUIRE(system.size() == 5);
    REQUIRE(system.particles()[0].size == 1.0F);
    system.update(scene, 0.1F);
    REQUIRE_THAT(system.particles()[0].size, WithinAbs(0.5, 1e-5));
    REQUIRE_THAT(system.particles()[0].color.r, WithinAbs(1.0, 1e-5));
    system.update(scene, 0.11F);
    REQUIRE(system.size() == 0);
}

TEST_CASE("Moving emitters leave trails and pass on their velocity", "[particles]")
{
    Scene scene;
    Entity engine = scene.createEntity("Engine");
    ParticleEmitterComponent emitter = straightEmitter(1000.0F);
    emitter.particles.speed = Vec2(0.0F);
    engine.add<ParticleEmitterComponent>(emitter);
    ParticleSystem system;
    system.update(scene, 0.1F);
    system.clear();  // Keep the emitter's position, drop its particles.
    system.update(scene, 0.1F);
    REQUIRE(system.size() == 100);

    // It moves 10 units in one step: its particles are spread along the way.
    engine.patch<TransformComponent>([](auto& t) { t.translation.x = 10.0F; });
    system.clear();
    system.update(scene, 0.1F);
    REQUIRE(system.size() == 100);
    const auto [left, right] = std::ranges::minmax(
        system.particles(), {}, [](const RenderParticle& p) { return p.position.x; });
    REQUIRE(left.position.x < 1.0F);
    REQUIRE(right.position.x > 9.0F);

    // ... and move on with its 100 units/s (inheritVelocity).
    engine.patch<ParticleEmitterComponent>([](auto& e) { e.inheritVelocity = true; });
    engine.patch<TransformComponent>([](auto& t) { t.translation.x = 20.0F; });
    system.clear();
    system.update(scene, 0.1F);
    system.update(scene, 0.1F);  // The emitter rests now, the particles do not.
    std::size_t moving = 0;
    for (const RenderParticle& particle : system.particles()) {
        moving += particle.position.x - particle.previousPosition.x > 9.0F ? 1 : 0;
    }
    REQUIRE(moving == 100);
}

TEST_CASE("Particle bursts stop at the capacity", "[particles]")
{
    ParticleSystem system(10);
    system.emit(ParticleSettings{.spread = kPi}, Vec3(0.0F), Vec3(0.0F, 1.0F, 0.0F), Vec3(0.0F),
                25);
    REQUIRE(system.size() == 10);
    REQUIRE(system.capacity() == 10);
    system.clear();
    REQUIRE(system.particles().empty());
}

TEST_CASE("Particle emitters are saved with the scene", "[particles][serialization]")
{
    Scene scene;
    Entity engine = scene.createEntity("Engine");
    ParticleEmitterComponent emitter = straightEmitter(42.0F);
    emitter.particles.startColor = {1.0F, 2.0F, 3.0F};
    emitter.particles.acceleration = {0.0F, -9.8F, 0.0F};
    engine.add<ParticleEmitterComponent>(emitter);

    Scene loaded;
    REQUIRE(deserializeScene(serializeScene(scene), loaded).has_value());
    const Entity copy = loaded.findEntityByName("Engine");
    REQUIRE(copy);
    const auto& read = copy.get<ParticleEmitterComponent>();
    REQUIRE(read.rate == 42.0F);
    REQUIRE_FALSE(read.inheritVelocity);
    REQUIRE(read.particles.startColor == Vec3(1.0F, 2.0F, 3.0F));
    REQUIRE(read.particles.acceleration == Vec3(0.0F, -9.8F, 0.0F));
    REQUIRE(read.particles.lifetime == Vec2(10.0F));
}
