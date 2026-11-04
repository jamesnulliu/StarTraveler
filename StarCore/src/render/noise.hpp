#pragma once
// Gradient noise of the generated sky and planets (private). Ported to shaders/noise.glsl
// (the planets are shaded by every renderer) -- keep them in sync.

#include <array>
#include <cmath>
#include <cstdint>

#include "starcore/math/random.hpp"
#include "starcore/math/types.hpp"

namespace starcore::detail
{

/** @brief One of the 12 cube edge gradients (Perlin's improved noise) dotted with (x, y, z). */
inline auto gradientDot(std::uint32_t hash, float x, float y, float z) -> float
{
    const std::uint32_t h = hash & 15U;
    const float u = h < 8U ? x : y;
    const float v = h < 4U ? y : (h == 12U || h == 14U ? x : z);
    return ((h & 1U) == 0U ? u : -u) + ((h & 2U) == 0U ? v : -v);
}

inline auto fade(float t) -> float
{
    return t * t * t * (t * (t * 6.0F - 15.0F) + 10.0F);
}

/** @brief a to b (std::lerp's guarantees cost a lot in the inner loops of the noise). */
inline auto lerpFast(float a, float b, float t) -> float
{
    return a + t * (b - a);
}

/** @brief The integer at or below x (std::floor is a call). */
inline auto floorToInt(float x) -> std::int32_t
{
    const auto truncated = static_cast<std::int32_t>(x);
    return x < static_cast<float>(truncated) ? truncated - 1 : truncated;
}

/** @brief Gradient noise, about -1 to 1 (standard deviation some 0.25). */
inline auto noise(Vec3 p, std::uint32_t seed) -> float
{
    const std::int32_t cellX = floorToInt(p.x);
    const std::int32_t cellY = floorToInt(p.y);
    const std::int32_t cellZ = floorToInt(p.z);
    const auto x = static_cast<std::uint32_t>(cellX);
    const auto y = static_cast<std::uint32_t>(cellY);
    const auto z = static_cast<std::uint32_t>(cellZ);
    const Vec3 f = p - Vec3(static_cast<float>(cellX), static_cast<float>(cellY),
                            static_cast<float>(cellZ));
    // A corner's hash: of x + the hash of y + the hash of z + the hash of the seed (the
    // corners share the inner ones).
    const std::uint32_t seedHash = pcgHash(seed);
    const std::array<std::uint32_t, 2> zHash{pcgHash(z + seedHash), pcgHash(z + 1U + seedHash)};
    const std::array<std::uint32_t, 4> yzHash{pcgHash(y + zHash[0]), pcgHash(y + 1U + zHash[0]),
                                              pcgHash(y + zHash[1]), pcgHash(y + 1U + zHash[1])};
    auto corner = [&](std::uint32_t dx, std::uint32_t dy, std::uint32_t dz) {
        return gradientDot(pcgHash(x + dx + yzHash[dz * 2U + dy]), f.x - static_cast<float>(dx),
                           f.y - static_cast<float>(dy), f.z - static_cast<float>(dz));
    };
    const float u = fade(f.x);
    const float v = fade(f.y);
    const float w = fade(f.z);
    const float x00 = lerpFast(corner(0U, 0U, 0U), corner(1U, 0U, 0U), u);
    const float x10 = lerpFast(corner(0U, 1U, 0U), corner(1U, 1U, 0U), u);
    const float x01 = lerpFast(corner(0U, 0U, 1U), corner(1U, 0U, 1U), u);
    const float x11 = lerpFast(corner(0U, 1U, 1U), corner(1U, 1U, 1U), u);
    return lerpFast(lerpFast(x00, x10, v), lerpFast(x01, x11, v), w);
}

/** @brief Fractal sum of octaves of noise, about -1 to 1 (most of it within +-0.5). */
inline auto fbm(Vec3 p, int octaves, std::uint32_t seed) -> float
{
    float sum = 0.0F;
    float amplitude = 0.5F;
    float total = 0.0F;
    for (int octave = 0; octave < octaves; ++octave) {
        sum += amplitude * noise(p, seed + static_cast<std::uint32_t>(octave) * 7919U);
        total += amplitude;
        p *= 2.03F;  // Not 2: octaves' lattices would line up.
        amplitude *= 0.5F;
    }
    return sum / total;
}

/** @brief Sharp ridges (filaments) of octaves of noise, 0 to 1. */
inline auto ridged(Vec3 p, int octaves, std::uint32_t seed) -> float
{
    float sum = 0.0F;
    float amplitude = 0.5F;
    float total = 0.0F;
    for (int octave = 0; octave < octaves; ++octave) {
        const float ridge =
            1.0F - std::abs(noise(p, seed + static_cast<std::uint32_t>(octave) * 104729U));
        sum += amplitude * ridge * ridge * ridge;
        total += amplitude;
        p *= 2.03F;
        amplitude *= 0.5F;
    }
    return sum / total;
}

}  // namespace starcore::detail
