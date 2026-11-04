#pragma once
// Planets: worlds out of reach, part of the sky. Each is drawn as seen from the world's
// origin (no parallax: they are much farther away than anywhere the camera goes), over the
// space sky and in front of farther planets, lit by the scene's sphere light that lights it
// most, from where that light is: a sun far beyond the planets lights them in phases, one
// near the origin lights the faces seen from there. Rays that escape the scene see them as
// part of the sky (skyRadiance), so every renderer draws the same planets, and their light
// reaches the scene as the sky's does. Ported to shaders/planet.glsl -- keep them in sync.
//
// A planet is a sphere with a procedural surface of its kind, under an atmosphere that
// scatters its sun's light (blue at the limb, red where the sun sets, a ring of light when
// backlit), with rings in its equatorial plane; the planet shadows its rings, and the rings
// shade it.

#include <array>
#include <cstddef>
#include <cstdint>

#include "starcore/common/enum_names.hpp"
#include "starcore/math/types.hpp"

namespace starcore
{

enum class PlanetKind : std::uint32_t
{
    Rocky = 0,        // Bare rock and dust: light and dark lands (the Moon, Mercury, Mars).
    Terrestrial = 1,  // Oceans (the secondary color), lands, ice caps and clouds.
    Gas = 2,          // A giant's bands along its latitudes, stirred into eddies.
    Ice = 3,          // Bright ice crossed by dark cracks (Europa).
};

template <>
struct EnumNames<PlanetKind>
{
    static constexpr std::array kNames = {"Rocky", "Terrestrial", "Gas", "Ice"};
};

/** @brief What a planet looks like (PlanetComponent's). Colors are albedos (0 to 1). */
struct PlanetLook
{
    PlanetKind kind = PlanetKind::Rocky;
    std::uint32_t seed = 1;
    Vec3 color{0.42F, 0.4F, 0.37F};           // Lands, light bands (zones), ice.
    Vec3 secondaryColor{0.2F, 0.19F, 0.18F};  // Dark lands, oceans, dark bands (belts), cracks.
    // The atmosphere's optical depth straight up from the surface, in the color that
    // scatters most (0: none; Earth's some 0.3, in blue)...
    float atmosphere = 0.0F;
    // ... and of each color relative to it (air scatters blue most; haze is more even).
    Vec3 atmosphereColor{0.18F, 0.42F, 1.0F};
    // Its scale height, in radii (Earth's is 0.0013; seen from far away a thicker one shows).
    float atmosphereHeight = 0.012F;
    // The share of haze in its scattering (haze scatters forward: light around a backlit
    // planet), the rest air (as much backward as forward).
    float haze = 0.0F;
    // Rings in the equatorial plane, in radii (outer <= inner: none), and how much light
    // the densest of them stop.
    float ringInner = 0.0F;
    float ringOuter = 0.0F;
    float ringOpacity = 0.8F;
    Vec3 ringColor{0.72F, 0.66F, 0.56F};

    auto operator==(const PlanetLook&) const -> bool = default;
};

/** @brief A planet as the renderers draw it (RenderSky::planets). */
struct RenderPlanet
{
    Vec3 center{0.0F, 0.0F, -1e5F};  // World.
    float radius = 1e4F;
    Mat3 worldToLocal{1.0F};  // Rotation into the planet's frame: its spin axis along y.
    PlanetLook look;
    Vec3 sunDirection{0.0F, 1.0F, 0.0F};  // Unit, from its center towards its sun.
    Vec3 sunIrradiance{0.0F};             // Its sun's light at it (0: none; it is dark).

    auto operator==(const RenderPlanet&) const -> bool = default;
};

/** @brief The most planets a sky has (the nearest to the origin are kept). */
inline constexpr std::size_t kMaxPlanets = 4;

/**
 * @brief What is seen from the origin towards a unit world direction: the planet in front
 *        of `behind` (the radiance of what is behind it, which shows through its
 *        atmosphere and rings), or `behind`.
 */
[[nodiscard]] auto planetRadiance(const RenderPlanet& planet, Vec3 direction, Vec3 behind)
    -> Vec3;

/** @brief The world origin in a planet's frame, in its radii (planet.glsl's Planet::origin). */
[[nodiscard]] auto planetOrigin(const RenderPlanet& planet) -> Vec3;

/** @brief Optical depth straight up from the surface per color (Planet::atmosphere). */
[[nodiscard]] auto planetAtmosphere(const PlanetLook& look) -> Vec3;

}  // namespace starcore
