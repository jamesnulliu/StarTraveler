module;

#include <entt/entity/registry.hpp>

module starcore.render;

import std;
import glm;
import starcore.math;
import starcore.scene;

namespace starcore
{

namespace
{

/** @brief A random unit vector in the cone of half angle `spread` around unit `axis`. */
auto coneDirection(const Vec3& axis, float spread, Rng& rng) -> Vec3
{
    // Uniform over the cap's solid angle.
    const float cosMax = std::cos(std::clamp(spread, 0.0F, kPi));
    const float cosTheta = 1.0F - rng.nextFloat() * (1.0F - cosMax);
    const float sinTheta = std::sqrt(std::max(0.0F, 1.0F - cosTheta * cosTheta));
    const float phi = kTwoPi * rng.nextFloat();
    const Vec3 helper = std::abs(axis.x) > 0.9F ? Vec3(0.0F, 1.0F, 0.0F) : Vec3(1.0F, 0.0F, 0.0F);
    const Vec3 u = glm::normalize(glm::cross(axis, helper));
    const Vec3 v = glm::cross(axis, u);
    return glm::normalize(axis * cosTheta + (u * std::cos(phi) + v * std::sin(phi)) * sinTheta);
}

auto uniform(Vec2 range, Rng& rng) -> float
{
    return range.x + (range.y - range.x) * rng.nextFloat();
}

}  // namespace

ParticleSystem::ParticleSystem(std::uint32_t capacity, std::uint32_t seed)
    : m_capacity(capacity), m_rng(seed)
{
}

auto ParticleSystem::spawn(const ParticleSettings& settings, const Vec3& position,
                           const Vec3& direction, const Vec3& velocity) -> Particle*
{
    if (m_particles.size() >= m_capacity) {
        return nullptr;
    }
    const float length = glm::length(direction);
    const Vec3 axis = length > 0.0F ? direction / length : Vec3(0.0F, 0.0F, -1.0F);
    Particle& particle = m_particles.emplace_back();
    particle.position = position;
    particle.previousPosition = position;
    particle.velocity =
        velocity + coneDirection(axis, settings.spread, m_rng) * uniform(settings.speed, m_rng);
    particle.lifetime = std::max(1e-3F, uniform(settings.lifetime, m_rng));
    particle.drag = settings.drag;
    particle.acceleration = settings.acceleration;
    particle.size = settings.size;
    particle.startColor = settings.startColor;
    particle.endColor = settings.endColor;
    m_renderDirty = true;
    return &particle;
}

void ParticleSystem::emit(const ParticleSettings& settings, const Vec3& position,
                          const Vec3& direction, const Vec3& velocity, std::uint32_t count)
{
    for (std::uint32_t i = 0; i < count; ++i) {
        if (spawn(settings, position, direction, velocity) == nullptr) {
            return;
        }
    }
}

void ParticleSystem::update(const Scene& scene, float deltaSeconds)
{
    if (deltaSeconds <= 0.0F) {
        return;
    }
    ++m_updates;
    auto advance = [](Particle& particle, float seconds) {
        particle.velocity += particle.acceleration * seconds;
        particle.velocity *= std::max(0.0F, 1.0F - particle.drag * seconds);
        particle.position += particle.velocity * seconds;
        particle.age += seconds;
    };

    // Move and age what exists.
    for (Particle& particle : m_particles) {
        particle.previousPosition = particle.position;
        advance(particle, deltaSeconds);
    }
    std::erase_if(m_particles,
                  [](const Particle& particle) { return particle.age >= particle.lifetime; });

    // Emit: each particle is born somewhere along the path its emitter moved
    // during the step and has lived the rest of it.
    const auto& registry = scene.registry();
    for (const auto [entity, emitter] : registry.view<const ParticleEmitterComponent>().each()) {
        const Mat4 world = scene.worldTransform(entity);
        const Vec3 position(world[3]);
        const Vec3 direction(world * Vec4(0.0F, 0.0F, -1.0F, 0.0F));
        const auto [slot, inserted] =
            m_emitters.try_emplace(entt::to_integral(entity), EmitterState{position, 0.0F, 0});
        EmitterState& state = slot->second;
        // New, or not updated last time (it was gone or disabled): no trail from there.
        const Vec3 start = inserted || state.seen + 1 != m_updates ? position : state.position;
        state.position = position;
        state.seen = m_updates;
        if (!emitter.enabled || emitter.rate <= 0.0F) {
            state.carry = 0.0F;
            continue;
        }
        const Vec3 velocity = emitter.inheritVelocity ? (position - start) / deltaSeconds
                                                      : Vec3(0.0F);
        state.carry += emitter.rate * deltaSeconds;
        const auto count = static_cast<std::uint32_t>(state.carry);
        state.carry -= static_cast<float>(count);
        for (std::uint32_t i = 0; i < count; ++i) {
            const float born = (static_cast<float>(i) + m_rng.nextFloat()) / static_cast<float>(count);
            Particle* particle =
                spawn(emitter.particles, glm::mix(start, position, born), direction, velocity);
            if (particle == nullptr) {
                break;
            }
            advance(*particle, (1.0F - born) * deltaSeconds);
        }
    }
    std::erase_if(m_emitters, [&](const auto& item) { return item.second.seen != m_updates; });
    m_renderDirty = true;
}

auto ParticleSystem::particles() const -> std::span<const RenderParticle>
{
    if (m_renderDirty) {
        refreshRenderData();
    }
    return m_render;
}

void ParticleSystem::clear()
{
    m_particles.clear();
    m_renderDirty = true;
}

void ParticleSystem::refreshRenderData() const
{
    m_render.resize(m_particles.size());
    for (std::size_t i = 0; i < m_particles.size(); ++i) {
        const Particle& particle = m_particles[i];
        const float t = std::clamp(particle.age / particle.lifetime, 0.0F, 1.0F);
        m_render[i] = RenderParticle{
            .position = particle.position,
            .size = glm::mix(particle.size.x, particle.size.y, t),
            .previousPosition = particle.previousPosition,
            .color = Vec4(glm::mix(particle.startColor, particle.endColor, t), 1.0F),
        };
    }
    m_renderDirty = false;
}

}  // namespace starcore
