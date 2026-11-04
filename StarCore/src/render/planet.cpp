#include "starcore/render/planet.hpp"

#include <algorithm>
#include <cmath>

#include <glm/common.hpp>
#include <glm/exponential.hpp>
#include <glm/geometric.hpp>

#include "noise.hpp"
#include "starcore/math/random.hpp"

namespace starcore
{

namespace
{

using detail::fbm;
using detail::noise;
using detail::ridged;

// The atmosphere's top, in scale heights above the surface (its density there: e^-8).
constexpr float kAtmosphereTop = 8.0F;
// Steps of a view ray through the atmosphere.
constexpr int kAtmosphereSteps = 16;
// Henyey-Greenstein asymmetry of haze's scattering, and of ring particles'.
constexpr float kHazeForward = 0.8F;
constexpr float kRingForward = 0.6F;
// Sun glint on oceans: Blinn-Phong exponent and water's reflectance where it shows.
constexpr float kGlintExponent = 400.0F;
constexpr float kGlintReflectance = 0.05F;
// Of craters: their slope over Surface::slope (depth some fifth of the diameter).
constexpr float kRelief = 0.25F;
constexpr float kLn2 = 0.693147181F;

auto smoothstep(float edge0, float edge1, float x) -> float
{
    const float t = std::clamp((x - edge0) / (edge1 - edge0), 0.0F, 1.0F);
    return t * t * (3.0F - 2.0F * t);
}

auto maxOf(Vec3 v) -> float
{
    return std::max({v.x, v.y, v.z});
}

auto rayleighPhase(float cosine) -> float
{
    return 3.0F / (16.0F * kPi) * (1.0F + cosine * cosine);
}

auto henyeyGreenstein(float cosine, float g) -> float
{
    const float d = 1.0F + g * g - 2.0F * g * cosine;
    return (1.0F - g * g) / (4.0F * kPi * d * std::sqrt(d));
}

/**
 * @brief The optical depth to space from altitude h towards a direction of cosine mu to the
 *        zenith, over the depth straight up from the surface: Chapman's grazing incidence
 *        function times the density at h, as approximated by Schueler (GPU Pro 3). x (the
 *        planet's radius) and h in half-heights, ln 2 scale heights.
 */
auto chapman(float x, float h, float mu) -> float
{
    const float c = std::sqrt(x + h);
    if (mu >= 0.0F) {
        return c / (c * mu + 1.0F) * std::exp2(-h);
    }
    // Below the horizon: through the lowest point of the ray, less what is behind.
    const float x0 = std::sqrt(1.0F - mu * mu) * (x + h);
    const float c0 = std::sqrt(x0);
    return 2.0F * c0 * std::exp2(std::min(x - x0, 100.0F)) - c / (1.0F - c * mu) * std::exp2(-h);
}

/** @brief How much of the sun's light gets through the atmosphere to a point. */
auto sunTransmittance(Vec3 depth, float height, Vec3 p, Vec3 sun) -> Vec3
{
    const float r = glm::length(p);
    const float h = (r - 1.0F) / (height * kLn2);
    return glm::exp(-depth * chapman(1.0F / (height * kLn2), h, glm::dot(p, sun) / r));
}

struct Ring
{
    float opacity = 0.0F;
    Vec3 color{0.0F};
};

/** @brief The rings at a radius: ringlets, a dark gap, a faint inner part, soft edges. */
auto ringAt(const PlanetLook& look, float r) -> Ring
{
    const float x = (r - look.ringInner) / (look.ringOuter - look.ringInner);
    if (x <= 0.0F || x >= 1.0F) {
        return {};
    }
    const std::uint32_t seed = look.seed;
    const float broad = noise(Vec3(r * 9.0F, 0.5F, 0.5F), seed + 11U);
    const float ringlets = noise(Vec3(r * 37.0F, 0.5F, 0.5F), seed + 12U);
    const float fine = noise(Vec3(r * 140.0F, 0.5F, 0.5F), seed + 13U);
    float density = std::clamp(0.6F + 0.9F * broad + 0.45F * ringlets + 0.25F * fine, 0.0F, 1.0F);
    const float gap =
        0.55F + 0.2F * static_cast<float>(pcgHash(seed + 14U) >> 8U) * (1.0F / 16777216.0F);
    const float offGap = (x - gap) / 0.02F;
    density *= 1.0F - 0.92F * std::exp(-offGap * offGap);
    density *= 0.25F + 0.75F * smoothstep(0.05F, 0.3F, x);
    density *= smoothstep(0.0F, 0.02F, x) * (1.0F - smoothstep(0.98F, 1.0F, x));
    return {
        .opacity = look.ringOpacity * density,
        .color = look.ringColor * (0.8F + 0.4F * smoothstep(-0.4F, 0.4F, broad)),
    };
}

struct Surface
{
    Vec3 albedo{0.0F};
    float water = 0.0F;  // Open ocean (its glint).
    Vec3 slope{0.0F};    // Of its relief (craters): tilts the shading normal.
};

auto unitFloat(std::uint32_t hash) -> float
{
    return static_cast<float>(hash >> 8U) * (1.0F / 16777216.0F);
}

/**
 * @brief Craters of one size at unit point p, `frequency` cells per radius: in each cell of a
 *        3D grid, a ball around a random point, sliced by the sphere (deeper centers, smaller
 *        craters): bowls with raised rims, dark floors and bright rims, some fresh and bright.
 *        Their relief (Surface::slope) shades them: a bowl's slope over its radius is the
 *        same at any size.
 */
void addCraters(Vec3 p, float frequency, std::uint32_t seed, float strength, Surface& surface)
{
    const Vec3 q = p * frequency;
    const Vec3 cell = glm::floor(q);
    for (int i = 0; i < 27; ++i) {
        const Vec3 corner = cell + Vec3(static_cast<float>(i % 3 - 1),
                                        static_cast<float>(i / 3 % 3 - 1),
                                        static_cast<float>(i / 9 - 1));
        const std::uint32_t h0 = pcgHash(
            seed + pcgHash(static_cast<std::uint32_t>(static_cast<std::int32_t>(corner.x)) +
                           pcgHash(static_cast<std::uint32_t>(static_cast<std::int32_t>(corner.y)) +
                                   pcgHash(static_cast<std::uint32_t>(
                                       static_cast<std::int32_t>(corner.z))))));
        const std::uint32_t h1 = pcgHash(h0);
        const std::uint32_t h2 = pcgHash(h1);
        const std::uint32_t h3 = pcgHash(h2);
        const Vec3 center = corner + Vec3(0.15F) + 0.7F * Vec3(unitFloat(h0), unitFloat(h1),
                                                               unitFloat(h2));
        const float size = unitFloat(h3);
        const float radius = 0.12F + 0.3F * size * size;
        const Vec3 offset = q - center;
        const float distance = glm::length(offset);
        const float d = distance / radius;
        if (d >= 1.6F || distance <= 0.0F) {
            continue;
        }
        const float offRim = (d - 1.0F) / 0.18F;
        const float rim = std::exp(-offRim * offRim);
        const bool fresh = (h3 & 0xFFU) < 30U;
        float albedo = glm::mix(0.86F, 1.0F, smoothstep(0.55F, 1.0F, d)) * (1.0F + 0.2F * rim);
        if (fresh) {
            albedo *= 1.0F + 0.6F * std::exp(-0.8F * d * d);
        }
        surface.albedo *= glm::mix(1.0F, albedo, strength);
        // The bowl (d^2 inside) and the rim, over d.
        const float rise = (d < 1.0F ? 1.2F * d : 0.0F) - 0.35F * rim * 2.0F * offRim / 0.18F;
        surface.slope += (strength * rise / distance) * offset;
    }
}

auto rockySurface(const PlanetLook& look, Vec3 p) -> Surface
{
    const float regions = fbm(p * 1.3F, 4, look.seed);
    const float detail = fbm(p * 6.0F, 4, look.seed + 1U);
    const float highlands = smoothstep(-0.06F, 0.06F, regions + 0.2F * detail);
    Surface surface{.albedo = glm::mix(look.secondaryColor, look.color, highlands) *
                              (1.0F + 0.3F * detail)};
    // The seas are younger: fewer craters.
    const float strength = glm::mix(0.35F, 1.0F, highlands);
    addCraters(p, 5.0F, look.seed + 20U, strength, surface);
    addCraters(p, 11.0F, look.seed + 21U, strength, surface);
    addCraters(p, 23.0F, look.seed + 22U, strength, surface);
    return surface;
}

auto terrestrialSurface(const PlanetLook& look, Vec3 p) -> Surface
{
    const std::uint32_t seed = look.seed;
    const Vec3 warp(noise(p * 1.5F, seed + 3U), noise(p * 1.5F + Vec3(17.1F), seed + 3U),
                    noise(p * 1.5F + Vec3(31.7F), seed + 3U));
    const float height = fbm(p * 1.7F + 0.5F * warp, 6, seed);
    const float land = smoothstep(0.04F, 0.06F, height);
    const float dry = smoothstep(-0.15F, 0.25F, noise(p * 2.5F, seed + 4U));
    Vec3 ground =
        glm::mix(look.color, glm::min(look.color * Vec3(1.6F, 1.3F, 0.85F), Vec3(1.0F)), dry);
    ground *= 0.85F + 0.8F * std::max(height - 0.05F, 0.0F);  // Mountains, lighter.
    const Vec3 sea = look.secondaryColor * (0.8F + 0.8F * smoothstep(-0.12F, 0.05F, height));
    const float ice =
        smoothstep(0.76F, 0.84F, std::abs(p.y) + 0.08F * fbm(p * 4.0F, 3, seed + 5U));
    // Clouds: swirled by the same flow, sharp-edged, thicker in the storm belts.
    const float belts = 0.05F * std::cos(p.y * 9.0F);
    const float cloud =
        smoothstep(0.03F, 0.15F, fbm(p * 3.4F + 0.6F * warp, 6, seed + 6U) + belts);
    Vec3 albedo = glm::mix(glm::mix(sea, ground, land), Vec3(0.82F, 0.85F, 0.88F), ice);
    albedo = glm::mix(albedo, Vec3(0.9F), cloud);
    return {.albedo = albedo, .water = (1.0F - land) * (1.0F - ice) * (1.0F - cloud)};
}

auto gasSurface(const PlanetLook& look, Vec3 p) -> Surface
{
    const std::uint32_t seed = look.seed;
    const float swirl = fbm(p * 3.0F, 4, seed + 5U);
    const float latitude = p.y + 0.04F * swirl;
    const float belts = noise(Vec3(0.5F, latitude * 7.0F, 0.5F), seed);
    const float streaks =
        fbm(Vec3(p.x * 1.5F, latitude * 22.0F, p.z * 1.5F) + Vec3(1.5F * swirl), 4, seed + 6U);
    const float t = smoothstep(-0.25F, 0.25F, belts + 0.45F * streaks);
    Vec3 albedo = glm::mix(look.secondaryColor, look.color, t);
    albedo *= 1.0F + 0.3F * noise(Vec3(0.5F, latitude * 45.0F, 0.5F), seed + 7U);
    const float polar = smoothstep(0.7F, 0.97F, std::abs(p.y));
    return {.albedo = glm::mix(albedo, look.secondaryColor * 0.75F, 0.6F * polar)};
}

auto iceSurface(const PlanetLook& look, Vec3 p) -> Surface
{
    const float base = fbm(p * 2.0F, 4, look.seed);
    const float cracks = ridged(p * 3.5F, 4, look.seed + 1U);
    Vec3 albedo = glm::mix(look.color, glm::mix(look.color, look.secondaryColor, 0.4F),
                           smoothstep(-0.1F, 0.3F, base));
    albedo = glm::mix(albedo, look.secondaryColor, 0.85F * smoothstep(0.75F, 0.92F, cracks));
    Surface surface{.albedo =
                        albedo * (0.92F + 0.16F * (0.5F + fbm(p * 12.0F, 2, look.seed + 2U)))};
    addCraters(p, 9.0F, look.seed + 20U, 0.3F, surface);  // A young surface: few, soft.
    return surface;
}

auto surfaceAt(const PlanetLook& look, Vec3 p) -> Surface
{
    Surface surface;
    switch (look.kind) {
    case PlanetKind::Terrestrial:
        surface = terrestrialSurface(look, p);
        break;
    case PlanetKind::Gas:
        surface = gasSurface(look, p);
        break;
    case PlanetKind::Ice:
        surface = iceSurface(look, p);
        break;
    case PlanetKind::Rocky:
    default:
        surface = rockySurface(look, p);
        break;
    }
    surface.albedo = glm::min(surface.albedo, Vec3(1.0F));  // Bright craters on light lands.
    return surface;
}

/** @brief Of a point's sunlight, what gets past the rings. */
auto ringShadow(const PlanetLook& look, Vec3 p, Vec3 sun) -> float
{
    if (look.ringOuter <= look.ringInner || std::abs(sun.y) < 1e-4F) {
        return 1.0F;
    }
    const float t = -p.y / sun.y;
    if (t <= 0.0F) {
        return 1.0F;
    }
    const Vec3 crossing = p + t * sun;
    const float r = std::sqrt(crossing.x * crossing.x + crossing.z * crossing.z);
    return 1.0F - ringAt(look, r).opacity;
}

/** @brief The light leaving the surface at unit point n towards -d. */
auto surfaceRadiance(const RenderPlanet& planet, Vec3 depth, Vec3 n, Vec3 d, Vec3 sun) -> Vec3
{
    const PlanetLook& look = planet.look;
    const float muSun = glm::dot(n, sun);
    if (muSun <= 0.0F) {
        return Vec3(0.0F);
    }
    Vec3 light = planet.sunIrradiance * ringShadow(look, n, sun);
    if (maxOf(depth) > 0.0F) {
        light *= sunTransmittance(depth, look.atmosphereHeight, n, sun);
    }
    const Surface surface = surfaceAt(look, n);
    // Tilted by its relief (the slope's part along the surface).
    const Vec3 along = surface.slope - glm::dot(surface.slope, n) * n;
    const Vec3 normal = glm::normalize(n - kRelief * along);
    const float muLit = std::max(glm::dot(normal, sun), 0.0F);
    const float muView = std::max(-glm::dot(normal, d), 0.0F);
    // Regolith and ice look flat (Lommel-Seeliger: the full Moon is no darker at its
    // edge); clouds and gas, Lambertian.
    const bool regolith = look.kind == PlanetKind::Rocky || look.kind == PlanetKind::Ice;
    const float shading = regolith ? 2.0F * muLit / (muLit + muView + 1e-4F) : muLit;
    Vec3 radiance = surface.albedo / kPi * light * shading;
    const Vec3 halfway = sun - d;
    if (surface.water > 0.0F && glm::dot(halfway, halfway) > 1e-12F) {
        const float glint =
            (kGlintExponent + 8.0F) / (8.0F * kPi) *
            std::pow(std::max(glm::dot(n, glm::normalize(halfway)), 0.0F), kGlintExponent);
        radiance += light * (surface.water * kGlintReflectance * glint * muSun);
    }
    return radiance;
}

/** @brief The rings' light at a point of them, seen along d. */
auto ringRadiance(const RenderPlanet& planet, const Ring& ring, Vec3 p, Vec3 d, Vec3 sun) -> Vec3
{
    // The planet's shadow, with a soft edge (the sun is a disc; the edge refracted).
    const float along = glm::dot(p, sun);
    const float offAxis = std::sqrt(std::max(glm::dot(p, p) - along * along, 0.0F));
    const float shadow = along < 0.0F ? smoothstep(0.98F, 1.02F, offAxis) : 1.0F;
    const Vec3 light = planet.sunIrradiance * shadow;
    const float muSun = std::abs(sun.y);
    const float muView = std::abs(d.y);
    const float shading = 2.0F * muSun / (muSun + muView + 1e-4F);
    // The lit face reflects; through the other comes what the particles scatter forward.
    const bool lit = sun.y * d.y < 0.0F;
    const float alpha = ring.opacity;
    const float forward = henyeyGreenstein(glm::dot(d, sun), kRingForward);
    const float seen = lit ? shading / kPi : (1.0F - alpha) * (shading / kPi + 0.6F * forward);
    return ring.color * light * (alpha * seen);
}

}  // namespace

auto planetOrigin(const RenderPlanet& planet) -> Vec3
{
    return planet.worldToLocal * (-planet.center / planet.radius);
}

auto planetAtmosphere(const PlanetLook& look) -> Vec3
{
    const float strongest = maxOf(look.atmosphereColor);
    if (look.atmosphere <= 0.0F || look.atmosphereHeight <= 0.0F || strongest <= 0.0F) {
        return Vec3(0.0F);
    }
    return look.atmosphere * glm::max(look.atmosphereColor, Vec3(0.0F)) / strongest;
}

auto planetRadiance(const RenderPlanet& planet, Vec3 direction, Vec3 behind) -> Vec3
{
    const PlanetLook& look = planet.look;
    const Vec3 o = planetOrigin(planet);
    const Vec3 d = planet.worldToLocal * direction;
    const Vec3 sun = planet.worldToLocal * planet.sunDirection;
    const Vec3 depth = planetAtmosphere(look);
    const float height = look.atmosphereHeight;
    const bool atmosphere = maxOf(depth) > 0.0F;
    const bool rings = look.ringOuter > look.ringInner;
    const float top = atmosphere ? 1.0F + kAtmosphereTop * height : 1.0F;
    const float bound = std::max(top, rings ? look.ringOuter : 0.0F);

    // Distances along the ray from its point closest to the center (q), where it is u = 0.
    const float b = glm::dot(o, d);
    const Vec3 q = o - b * d;
    const float q2 = glm::dot(q, q);
    if (q2 >= bound * bound || (b > 0.0F && glm::dot(o, o) > bound * bound)) {
        return behind;
    }
    const bool surface = q2 < 1.0F && b < 0.0F;
    const float front = surface ? -std::sqrt(1.0F - q2) : 0.0F;  // Where the planet stops it.

    Vec3 radiance = behind;
    if (surface) {
        radiance = surfaceRadiance(planet, depth, glm::normalize(q + front * d), d, sun);
    }
    Ring ring;
    Vec3 ringPoint{0.0F};
    float ringAlong = 0.0F;
    if (rings && std::abs(d.y) > 1e-6F) {
        const float t = -o.y / d.y;
        if (t > 0.0F) {
            ringPoint = o + t * d;
            ring = ringAt(look, std::sqrt(ringPoint.x * ringPoint.x + ringPoint.z * ringPoint.z));
            ringAlong = t + b;
        }
    }
    const bool ringBehind = ring.opacity > 0.0F && ringAlong > front;
    if (ringBehind && !surface) {
        radiance = ringRadiance(planet, ring, ringPoint, d, sun) + (1.0F - ring.opacity) * radiance;
    }
    if (atmosphere && q2 < top * top) {
        // Single scattering of the sun's light along the ray's way through it.
        const float enter = std::max(-std::sqrt(top * top - q2), b);
        const float exit = surface ? front : -enter;
        const float step = (exit - enter) / static_cast<float>(kAtmosphereSteps);
        const float cosine = glm::dot(d, sun);
        const float phase = glm::mix(rayleighPhase(cosine), henyeyGreenstein(cosine, kHazeForward),
                                     look.haze);
        Vec3 transmittance(1.0F);
        Vec3 scattered(0.0F);
        for (int i = 0; i < kAtmosphereSteps; ++i) {
            const Vec3 p = q + (enter + (static_cast<float>(i) + 0.5F) * step) * d;
            const float altitude = (glm::length(p) - 1.0F) / height;
            const Vec3 stepTransmittance = glm::exp(-depth * (std::exp(-altitude) * step / height));
            scattered += transmittance * (Vec3(1.0F) - stepTransmittance) *
                         sunTransmittance(depth, height, p, sun);
            transmittance *= stepTransmittance;
        }
        radiance = radiance * transmittance + planet.sunIrradiance * (scattered * phase);
    }
    if (ring.opacity > 0.0F && !ringBehind) {
        radiance = ringRadiance(planet, ring, ringPoint, d, sun) + (1.0F - ring.opacity) * radiance;
    }
    return radiance;
}

}  // namespace starcore
