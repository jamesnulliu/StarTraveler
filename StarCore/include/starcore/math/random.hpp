#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

#include <glm/geometric.hpp>

#include "starcore/math/types.hpp"

namespace starcore
{

/**
 * @brief PCG hash (RXS-M-XS 32), see Jarzynski & Olano, "Hash Functions for
 *        GPU Rendering", JCGT 2020. Written so it translates 1:1 to shaders.
 */
[[nodiscard]] constexpr auto pcgHash(std::uint32_t value) -> std::uint32_t
{
    const std::uint32_t state = value * 747796405U + 2891336453U;
    const std::uint32_t word =
        ((state >> ((state >> 28U) + 4U)) ^ state) * 277803737U;
    return (word >> 22U) ^ word;
}

/**
 * @brief Small deterministic PCG random number generator.
 *
 * Every pixel/frame gets an independent stream via forPixel(), which fixes the
 * legacy `seed = pixelIndex * frameIndex` scheme (correlated streams, and
 * pixel 0 always used seed 0).
 */
class Rng
{
public:
    constexpr explicit Rng(std::uint32_t seed) : m_state(pcgHash(seed))
    {
    }

    [[nodiscard]] static constexpr auto forPixel(std::uint32_t x,
                                                 std::uint32_t y,
                                                 std::uint32_t frame,
                                                 std::uint32_t seed = 0) -> Rng
    {
        return Rng(pcgHash(x + pcgHash(y + pcgHash(frame + pcgHash(seed)))));
    }

    constexpr auto nextUint() -> std::uint32_t
    {
        m_state = m_state * 747796405U + 2891336453U;
        const std::uint32_t word =
            ((m_state >> ((m_state >> 28U) + 4U)) ^ m_state) * 277803737U;
        return (word >> 22U) ^ word;
    }

    /** @brief Uniform float in [0, 1). */
    constexpr auto nextFloat() -> float
    {
        return static_cast<float>(nextUint() >> 8U) * 0x1p-24F;
    }

    /** @brief Uniform direction on the unit sphere. */
    auto onUnitSphere() -> Vec3
    {
        const float z = 1.0F - 2.0F * nextFloat();
        const float r = std::sqrt(std::max(0.0F, 1.0F - z * z));
        const float phi = kTwoPi * nextFloat();
        return {r * std::cos(phi), r * std::sin(phi), z};
    }

    /** @brief Uniform point inside the unit sphere. */
    auto inUnitSphere() -> Vec3
    {
        return onUnitSphere() * std::cbrt(nextFloat());
    }

    /**
     * @brief Cosine-weighted direction in the hemisphere around `normal`
     *        (`normal` must be normalized). pdf = cos(theta) / pi.
     */
    auto cosineHemisphere(Vec3 normal) -> Vec3
    {
        const Vec3 direction = normal + onUnitSphere();
        const float length2 = glm::dot(direction, direction);
        return length2 > 1e-12F ? direction / std::sqrt(length2) : normal;
    }

private:
    std::uint32_t m_state;
};

}  // namespace starcore
