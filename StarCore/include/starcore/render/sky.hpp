#pragma once
// The sky of a scene in space (SpaceSky): the Milky Way and the stars, generated from a
// seed. Every renderer sees the same data: rays that escape the scene look up the sky
// map (skyRadiance; shaders/sky.glsl), and the GPU renderer draws the brighter stars as
// points over its image (they are not in the map: sub-pixel points would blur in a map,
// and temporal upscalers smear or drop them).
//
// The model is of a view from inside a spiral galaxy, at a distance from its center like
// the Sun's: a band around the whole sky, brightest and warmest towards the center (the
// bulge), split by lanes of dust that dim and redden what is behind them, with clouds of
// stars, a few faint nebulae and two small satellite galaxies. Stars follow the counts of
// the real sky (some 2.8 times as many per fainter magnitude, most of the faint ones
// near the band), with the colors of their temperatures.

#include <cstdint>
#include <memory>
#include <vector>

#include "starcore/math/types.hpp"

namespace starcore
{

struct SpaceSky;

/** @brief A star drawn as a point (SkyMap::stars). */
struct SkyStar
{
    Vec3 direction{0.0F, 0.0F, 1.0F};  // Unit, in the galactic frame.
    // The irradiance it gives (radiance x steradians) at SpaceSky::brightness 1.
    float flux = 0.0F;
    Vec3 color{1.0F};        // Linear RGB of luminance 1.
    float magnitude = 0.0F;  // Apparent magnitude.
};
static_assert(sizeof(SkyStar) == 32, "std430 layout of the star pass's stars");

/**
 * @brief The sky of a SpaceSky at brightness 1, in the galactic frame: an
 *        equirectangular map of its diffuse light (the Milky Way and the stars too faint
 *        to draw) and the stars drawn as points.
 *
 * Map columns go from galactic longitude -180 to 180 degrees (the galactic center in the
 * middle), rows from latitude 90 (north) down to -90. The galactic frame: x towards the
 * center, y towards the north pole, longitude 90 towards z.
 */
struct SkyMap
{
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    // RGB9E5 (VK_FORMAT_E5B9G9R9_UFLOAT_PACK32), row by row from the north.
    std::vector<std::uint32_t> texels;
    std::vector<SkyStar> stars;  // Brightest first.
    Vec3 meanRadiance{0.0F};     // Over all directions, the drawn stars' included.

    /** @brief Radiance of the map (bilinear, as shaders/sky.glsl) towards a galactic direction. */
    [[nodiscard]] auto radiance(Vec3 galacticDirection) const -> Vec3;
};

inline constexpr std::uint32_t kSkyMapWidth = 4096;  // Some 0.09 degrees per texel.

/**
 * @brief The sky of `settings`: its seed, Milky Way and star magnitude (brightness and
 *        orientation apply where it is looked up). Generated in parallel (some 0.7 s at
 *        the default width on a 16-core laptop CPU); calls with the same settings share one
 *        map while it is in use.
 * @param width Of the map (height: half of it); tests use small ones.
 */
[[nodiscard]] auto generateSkyMap(const SpaceSky& settings, std::uint32_t width = kSkyMapWidth)
    -> std::shared_ptr<const SkyMap>;

/** @brief Rotation from world to galactic directions of `settings.orientation`. */
[[nodiscard]] auto worldToGalactic(const SpaceSky& settings) -> Mat3;

/** @brief Stars brighter than an apparent magnitude in the whole sky (the model's counts). */
[[nodiscard]] auto starsBrighterThan(float magnitude) -> double;

/** @brief The flux of a star of an apparent magnitude (SkyStar::flux). */
[[nodiscard]] auto starFlux(float magnitude) -> float;

/** @brief Linear RGB of luminance 1 of a black body at `kelvin`. */
[[nodiscard]] auto blackbodyColor(float kelvin) -> Vec3;

/** @brief Shared-exponent packing of a non-negative color (VK_FORMAT_E5B9G9R9_UFLOAT_PACK32). */
[[nodiscard]] auto encodeRgb9e5(Vec3 color) -> std::uint32_t;
[[nodiscard]] auto decodeRgb9e5(std::uint32_t packed) -> Vec3;

}  // namespace starcore
