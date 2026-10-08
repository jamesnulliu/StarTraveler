#include <cmath>

#include <glm/common.hpp>
#include <glm/geometric.hpp>

#include "gameplay/space_dust.hpp"

namespace startraveler
{

using namespace starcore;

namespace
{

constexpr float kSize = 0.025F;                // Speck radius, world units.
const Vec3 kColor{1.2F, 1.4F, 1.8F};           // Emitted radiance: faint, cold.
constexpr float kNearFade[2] = {2.5F, 6.0F};   // Distances: invisible -> full.

}  // namespace

SpaceDust::SpaceDust(std::uint32_t count, float extent, std::uint32_t seed)
    : m_positions(count), m_extent(extent), m_rng(seed)
{
}

void SpaceDust::update(const Vec3& center)
{
    const float half = 0.5F * m_extent;
    if (!m_placed) {
        for (Vec3& position : m_positions) {
            position = center - half +
                       Vec3(m_rng.nextFloat(), m_rng.nextFloat(), m_rng.nextFloat()) * m_extent;
        }
        m_placed = true;
        return;
    }
    // Wrap into [center - half, center + half) per axis.
    for (Vec3& position : m_positions) {
        const Vec3 offset = position - center + half;
        position = center - half + (offset - m_extent * glm::floor(offset / m_extent));
    }
}

void SpaceDust::appendParticles(std::vector<RenderParticle>& particles, const Vec3& center) const
{
    const float half = 0.5F * m_extent;
    for (const Vec3& position : m_positions) {
        const float distance = glm::distance(position, center);
        const float fade = glm::smoothstep(kNearFade[0], kNearFade[1], distance) *
                           (1.0F - glm::smoothstep(0.7F * half, half, distance));
        if (fade <= 0.0F) {
            continue;
        }
        // At rest in space: only the camera's motion streaks them.
        particles.push_back(RenderParticle{.position = position,
                                           .size = kSize,
                                           .previousPosition = position,
                                           .color = Vec4(kColor * fade, 1.0F)});
    }
}

}  // namespace startraveler
