// Particles: many small, short-lived glowing points (exhaust, sparks, dust),
// simulated on the CPU and drawn by the GPU renderer as additive sprites over
// its image. They emit light into the picture (and its bloom) but light
// nothing and cast no shadows: they are not part of the render scene.

export module starcore.render:particles;

import std;
import starcore.math;
import starcore.scene;

export namespace starcore
{

/** @brief A particle as drawn (`Particle` in shaders/particle.vert). */
struct RenderParticle
{
    Vec3 position{0.0F};
    float size = 0.0F;  // Sprite radius in world units.
    // Where it was a frame ago: moving particles are drawn as streaks.
    Vec3 previousPosition{0.0F};
    float padding = 0.0F;
    Vec4 color{0.0F};  // rgb: emitted radiance, a: unused.
};
static_assert(sizeof(RenderParticle) == 48, "std430 layout of Particle in particle.vert");

/**
 * @brief A pool of simulated particles: emitted by entities with a
 *        ParticleEmitterComponent or by emit() calls, moved with their
 *        velocity, drag and acceleration, dropped when their life ends.
 */
class ParticleSystem
{
public:
    explicit ParticleSystem(std::uint32_t capacity = 16384, std::uint32_t seed = 1);

    /**
     * @brief Advance by `deltaSeconds`: emit from the scene's enabled
     *        emitters (spread along the path an emitter moved, so fast ones
     *        leave continuous trails), then move and age all particles.
     */
    void update(const Scene& scene, float deltaSeconds);

    /**
     * @brief Emit `count` particles at `position` into a cone around
     *        `direction` (settings.spread), with `velocity` added (they move
     *        with the next update). Particles beyond the capacity are dropped.
     */
    void emit(const ParticleSettings& settings, const Vec3& position, const Vec3& direction,
              const Vec3& velocity, std::uint32_t count);

    /** @brief The particles as of the last update or emit. */
    [[nodiscard]] auto particles() const -> std::span<const RenderParticle>;
    [[nodiscard]] auto size() const noexcept -> std::size_t
    {
        return m_particles.size();
    }
    [[nodiscard]] auto capacity() const noexcept -> std::uint32_t
    {
        return m_capacity;
    }
    /** @brief Drop all particles (emitters remember where they were). */
    void clear();

private:
    struct Particle
    {
        Vec3 position{0.0F};
        Vec3 previousPosition{0.0F};
        Vec3 velocity{0.0F};
        float age = 0.0F;
        float lifetime = 1.0F;
        float drag = 0.0F;
        Vec3 acceleration{0.0F};
        Vec2 size{0.0F};
        Vec3 startColor{0.0F};
        Vec3 endColor{0.0F};
    };

    /** @brief Where an emitter was last update and its fraction of a particle due. */
    struct EmitterState
    {
        Vec3 position{0.0F};
        float carry = 0.0F;
        std::uint64_t seen = 0;
    };

    auto spawn(const ParticleSettings& settings, const Vec3& position, const Vec3& direction,
               const Vec3& velocity) -> Particle*;
    void refreshRenderData() const;

    std::uint32_t m_capacity;
    Rng m_rng;
    std::vector<Particle> m_particles;
    std::unordered_map<std::uint32_t, EmitterState> m_emitters;  // By entity.
    std::uint64_t m_updates = 0;
    mutable std::vector<RenderParticle> m_render;
    mutable bool m_renderDirty = true;
};

}  // namespace starcore
