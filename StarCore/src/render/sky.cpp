#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <mutex>

#include <glm/common.hpp>
#include <glm/exponential.hpp>
#include <glm/geometric.hpp>
#include <glm/trigonometric.hpp>

#include "noise.hpp"
#include "starcore/common/log.hpp"
#include "starcore/common/parallel.hpp"
#include "starcore/math/random.hpp"
#include "starcore/render/sky.hpp"
#include "starcore/scene/scene.hpp"

namespace starcore
{

namespace
{

// Star counts over the whole sky: log10 N(<m) = 1 + 0.45 m (some 5000 stars brighter
// than magnitude 6, 40000 than 8 and 300000 than 10, as the real sky).
constexpr double kCountOffset = 1.0;
constexpr double kCountSlope = 0.45;
constexpr float kBrightestMagnitude = -1.5F;
constexpr float kFaintestDrawn[2] = {2.0F, 10.0F};  // Range of SpaceSky::starMagnitude.
// Stars down to this many magnitudes fainter than the drawn ones are in the map (the
// grain of the band: most of them crowd there).
constexpr float kMapMagnitudes = 3.5F;
// The flux of a star of magnitude 0: seen over a 70 degree view 1080 pixels high, the
// pixel it falls in gets a radiance of about 5 (bright, blooming); magnitude 6 of 0.02.
constexpr float kZeroMagnitudeFlux = 2e-5F;
// The Milky Way's radiance where it is brightest, before the dust.
constexpr float kMilkyWayRadiance = 0.075F;
// What is left between the stars (galaxies too faint to see one by one).
constexpr float kBackgroundRadiance = 1e-4F;
// Dust dims blue more than red: extinction per optical depth in green (Cardelli et al.'s law,
// R_V = 3.1, at some 610, 550 and 465 nm).
const Vec3 kReddening{0.84F, 1.0F, 1.24F};
// Of a star's color: perceived star colors are pale (bright points saturate the eye and
// sensors alike).
constexpr float kStarSaturation = 0.7F;

constexpr float kDegree = kPi / 180.0F;

auto square(float x) -> float
{
    return x * x;
}

auto smoothstep(float edge0, float edge1, float x) -> float
{
    const float t = std::clamp((x - edge0) / (edge1 - edge0), 0.0F, 1.0F);
    return t * t * (3.0F - 2.0F * t);
}

/** @brief 1 / cosh(x)^2: the profile of a self-gravitating disk across it. */
auto sech2(float x) -> float
{
    const float c = std::cosh(std::min(std::abs(x), 40.0F));
    return 1.0F / (c * c);
}

auto luminance(Vec3 color) -> float
{
    return glm::dot(color, Vec3(0.2126F, 0.7152F, 0.0722F));
}

using detail::fbm;
using detail::noise;
using detail::ridged;

// Galactic coordinates ---------------------------------------------------------------------------

struct Galactic
{
    float longitude;  // Radians, -pi to pi (0: the center).
    float latitude;   // Radians, -pi/2 to pi/2.
};

auto toGalactic(Vec3 g) -> Galactic
{
    const float longitude = std::abs(g.x) + std::abs(g.z) > 0.0F ? std::atan2(g.z, g.x) : 0.0F;
    return {longitude, std::asin(std::clamp(g.y, -1.0F, 1.0F))};
}

auto fromGalactic(float longitude, float latitude) -> Vec3
{
    return {std::cos(latitude) * std::cos(longitude), std::sin(latitude),
            std::cos(latitude) * std::sin(longitude)};
}

auto fromGalacticDegrees(float longitude, float latitude) -> Vec3
{
    return fromGalactic(longitude * kDegree, latitude * kDegree);
}

/** @brief A galactic direction squeezed across the plane: noise of it runs along the band. */
auto alongBand(Vec3 g, float stretch) -> Vec3
{
    return {g.x, g.y * stretch, g.z};
}

/**
 * @brief A patch of the sky (nebula, dark cloud, galaxy): a center, a radius (radians)
 *        along `axis` times `elongation` across it, its texture's own noise.
 */
struct Blob
{
    Vec3 center{1.0F, 0.0F, 0.0F};
    Vec3 axis{0.0F, 1.0F, 0.0F};  // Tangent at the center.
    float radius = 0.01F;
    float elongation = 1.0F;
    float strength = 1.0F;
    float texture = 0.6F;  // How much noise breaks it up (0: smooth).
    Vec3 color{1.0F};
    std::uint32_t seed = 0;
    Vec3 across{1.0F, 0.0F, 0.0F};  // Tangent at the center, across the axis.
    float reach = -1.0F;  // Cosine of the angle from the center beyond which it is nothing.
};

auto makeBlob(Vec3 center, float radius, float elongation, float angle, float strength,
              float texture, Vec3 color, std::uint32_t seed) -> Blob
{
    center = glm::normalize(center);
    // The tangent towards the north pole, turned by `angle`.
    Vec3 north = Vec3(0.0F, 1.0F, 0.0F) - center * center.y;
    north = glm::dot(north, north) > 1e-8F ? glm::normalize(north) : Vec3(1.0F, 0.0F, 0.0F);
    const Vec3 east = glm::cross(north, center);
    const Vec3 axis = std::cos(angle) * north + std::sin(angle) * east;
    // blobShape's s^2 = 9 (3 radii) along its longer side, and a margin.
    const float reach = 3.0F * radius * std::max(elongation, 1.0F) * 1.1F;
    return Blob{.center = center,
                .axis = axis,
                .radius = radius,
                .elongation = elongation,
                .strength = strength,
                .texture = texture,
                .color = color,
                .seed = seed,
                .across = glm::cross(center, axis),
                .reach = reach < kPi / 2.0F ? std::cos(reach) : 0.0F};
}

/** @brief How much of a blob is towards `g` (1 at its center, smooth, noisy). */
auto blobShape(Vec3 g, const Blob& blob) -> float
{
    const float cosine = glm::dot(g, blob.center);
    if (cosine <= std::max(blob.reach, 0.0F)) {
        return 0.0F;
    }
    const Vec3 offset = g - blob.center * cosine;  // In the tangent plane (radians, near it).
    const float s2 = square(glm::dot(offset, blob.axis) / (blob.radius * blob.elongation)) +
                     square(glm::dot(offset, blob.across) / blob.radius);
    if (s2 > 9.0F) {
        return 0.0F;
    }
    const float shape = std::exp(-s2);
    if (blob.texture <= 0.0F) {
        return shape;
    }
    const float detail = fbm(g * (6.0F / blob.radius), 5, blob.seed);
    return shape * std::max(0.0F, 1.0F + blob.texture * (2.6F * detail - 0.3F));
}

// The galaxy ------------------------------------------------------------------------------------

/** @brief The diffuse light and dust of the sky of a seed (radiance at milkyWay 1). */
class Galaxy
{
public:
    explicit Galaxy(std::uint32_t seed) : m_seed(pcgHash(seed ^ 0x27d4eb2dU))
    {
        Rng rng(m_seed);
        auto uniform = [&](float low, float high) { return low + (high - low) * rng.nextFloat(); };
        auto normal = [&](float sigma) {
            // Sum of uniforms: near enough to a Gaussian for placing things.
            return sigma * (rng.nextFloat() + rng.nextFloat() + rng.nextFloat() - 1.5F) * 2.0F;
        };
        auto logUniform = [&](float low, float high) {
            return std::exp(uniform(std::log(low), std::log(high)));
        };
        // Emission nebulae: hydrogen glowing red around young stars, in the band, mostly
        // towards the inner galaxy.
        for (int i = 0; i < 18; ++i) {
            const float longitude = rng.nextFloat() < 0.7F ? normal(55.0F) : uniform(-180.0F, 180.0F);
            const float latitude = normal(1.8F);
            const bool magenta = rng.nextFloat() < 0.25F;
            m_emission.push_back(makeBlob(
                fromGalacticDegrees(longitude, latitude), logUniform(0.35F, 3.2F) * kDegree,
                uniform(1.0F, 2.2F), uniform(0.0F, kPi), uniform(0.06F, 0.3F), 0.9F,
                magenta ? Vec3(1.0F, 0.45F, 0.6F) : Vec3(1.0F, 0.4F, 0.42F), rng.nextUint()));
        }
        // Reflection nebulae: dust lit by nearby stars, blue, off the band.
        for (int i = 0; i < 4; ++i) {
            const float latitude = (rng.nextFloat() < 0.5F ? -1.0F : 1.0F) * uniform(4.0F, 18.0F);
            m_reflection.push_back(makeBlob(
                fromGalacticDegrees(uniform(-180.0F, 180.0F), latitude),
                logUniform(0.3F, 1.2F) * kDegree, uniform(1.0F, 1.8F), uniform(0.0F, kPi),
                uniform(0.08F, 0.2F), 0.8F, Vec3(0.45F, 0.62F, 1.0F), rng.nextUint()));
        }
        // Dark clouds off the band near the center (they hide what is behind them).
        for (int i = 0; i < 9; ++i) {
            const float latitude = (rng.nextFloat() < 0.5F ? -1.0F : 1.0F) * uniform(3.0F, 20.0F);
            m_dark.push_back(makeBlob(fromGalacticDegrees(normal(45.0F), latitude),
                                      logUniform(0.6F, 3.5F) * kDegree, uniform(1.0F, 3.0F),
                                      uniform(0.0F, kPi), uniform(0.8F, 2.2F), 1.0F, Vec3(1.0F),
                                      rng.nextUint()));
        }
        // Satellite galaxies where the real sky has them, and a large spiral farther off.
        m_galaxies.push_back(makeBlob(fromGalacticDegrees(-79.5F, -32.9F), 3.0F * kDegree, 1.6F,
                                      0.6F, 0.12F, 0.45F, Vec3(0.92F, 0.94F, 1.0F),
                                      rng.nextUint()));
        m_galaxies.push_back(makeBlob(fromGalacticDegrees(-79.0F, -31.5F), 0.5F * kDegree, 1.3F,
                                      0.0F, 0.15F, 0.5F, Vec3(1.0F, 0.35F, 0.45F),
                                      rng.nextUint()));  // Its brightest nebula.
        m_galaxies.push_back(makeBlob(fromGalacticDegrees(-57.2F, -44.3F), 1.5F * kDegree, 1.7F,
                                      0.9F, 0.08F, 0.4F, Vec3(0.92F, 0.94F, 1.0F),
                                      rng.nextUint()));
        m_galaxies.push_back(makeBlob(fromGalacticDegrees(121.2F, -21.6F), 0.7F * kDegree, 3.6F,
                                      0.6F, 0.05F, 0.3F, Vec3(1.0F, 0.88F, 0.72F),
                                      rng.nextUint()));
        m_galaxies.push_back(makeBlob(fromGalacticDegrees(121.2F, -21.6F), 0.12F * kDegree,
                                      2.0F, 0.6F, 0.25F, 0.0F, Vec3(1.0F, 0.85F, 0.65F),
                                      rng.nextUint()));  // Its core.
    }

    /** @brief Relative density of the band's stars towards galactic coordinates (at most 1). */
    [[nodiscard]] static auto bandDensity(Galactic c) -> float
    {
        const float longitude = c.longitude / kDegree;
        const float latitude = c.latitude / kDegree;
        const float along = 0.25F + 0.75F * std::exp(-square(longitude / 70.0F));
        return along * (0.7F * sech2(latitude / 4.0F) + 0.3F * sech2(latitude / 14.0F));
    }

    /**
     * @brief What varies over degrees, not over the map's texels: generated on a coarser
     *        grid and interpolated (most of the work otherwise).
     */
    struct Smooth
    {
        float clouds = 0.0F;  // Star clouds, long along the band (-1 to 1).
        float young = 0.0F;   // Where the young (bluer) star clouds are (-1 to 1).
        float cirrus = 0.0F;  // Faint dust (-1 to 1).
        float wave = 0.0F;    // How the dust lane meanders (-1 to 1).
        Vec3 warp{0.0F};      // Displacement of the dust's noise: its lanes curl.
    };

    [[nodiscard]] auto smooth(Vec3 g) const -> Smooth
    {
        const Vec3 band = alongBand(g, 2.0F);
        return {.clouds = fbm(band * 7.0F, 5, m_seed + 1U),
                .young = fbm(g * 3.0F, 4, m_seed + 3U),
                .cirrus = fbm(g * 3.5F, 4, m_seed + 14U),
                .wave = noise(alongBand(g, 1.5F) * 3.0F, m_seed + 11U),
                .warp = {fbm(band * 4.0F, 4, m_seed + 21U), fbm(band * 4.0F, 4, m_seed + 22U),
                         fbm(band * 4.0F, 4, m_seed + 23U)}};
    }

    /** @brief Optical depth (in green) of the dust in front of the band's light towards g. */
    [[nodiscard]] auto dust(Vec3 g, Galactic c, const Smooth& smooth) const -> float
    {
        const float longitude = c.longitude / kDegree;
        const float latitude = c.latitude / kDegree;
        // The main layer: thinner than the stars' disk, thicker towards the center (the
        // nearer clouds there look larger), wavy.
        const float wave = smooth.wave;
        const float thickness = 0.9F + 1.4F * std::exp(-square(longitude / 50.0F));
        float depth = 0.0F;
        const float lane = sech2((latitude - wave) / thickness);
        // The great rift: a broad dark lane over the inner galaxy on one side of the center.
        const float rift = smoothstep(-25.0F, -5.0F, longitude) *
                           (1.0F - smoothstep(50.0F, 85.0F, longitude)) *
                           sech2((latitude - 1.8F - 0.5F * wave) / 3.0F);
        // Near the center, a few clouds off the plane.
        const float off = std::exp(-square(longitude / 60.0F)) *
                          smoothstep(16.0F, 2.0F, std::abs(latitude));
        if (lane + rift + off > 1e-3F) {
            // Patchy, with soft but definite edges (most of the light between the patches
            // gets through: the star clouds), curled and laced with filaments.
            const Vec3 p = alongBand(g, 2.5F) * 9.0F + smooth.warp * 1.1F;
            const float noise = fbm(p, 6, m_seed + 12U);
            const float patches = smoothstep(-0.3F, 0.25F, noise);
            const float filaments = ridged(p * 2.4F, 5, m_seed + 13U);
            depth += (4.5F * lane + 3.5F * rift) * patches * (0.35F + 1.6F * filaments);
            depth += 3.0F * off * smoothstep(0.3F, 0.55F, noise) * (0.5F + filaments);
        }
        for (const Blob& cloud : m_dark) {
            depth += cloud.strength * blobShape(g, cloud);
        }
        // Faint cirrus, near the plane.
        depth += 0.15F * std::max(0.0F, smooth.cirrus) * std::exp(-square(latitude / 20.0F));
        return depth;
    }

    /**
     * @brief The diffuse light towards g at milkyWay 1 (radiance; the background apart), and
     *        the dust's optical depth there.
     * @param c Of g.
     * @param smooth Of g (interpolated).
     * @param speckle The texel's unresolved stars: random, mean 1 (they are few, the light of
     *        a texel varies).
     */
    [[nodiscard]] auto diffuse(Vec3 g, Galactic c, const Smooth& smooth, float speckle,
                               float& depth) const -> Vec3
    {
        const float longitude = c.longitude / kDegree;
        const float latitude = c.latitude / kDegree;
        // The disk: brightest towards the center, a thin layer and a fainter thick one.
        const float along = 0.12F + 0.38F * std::exp(-square(longitude / 28.0F)) +
                            0.5F * std::exp(-square(longitude / 95.0F));
        const float thin = 1.7F + 1.3F * std::exp(-square(longitude / 35.0F));
        float disk = along * (0.85F * sech2(latitude / thin) + 0.15F * sech2(latitude / 6.0F));
        // The bulge: boxy, around the center.
        float bulge = 0.0F;
        if (std::abs(longitude) < 90.0F && std::abs(latitude) < 60.0F) {
            const float r = std::pow(std::pow(std::abs(longitude / 11.0F), 2.4F) +
                                         std::pow(std::abs(latitude / 7.5F), 2.4F),
                                     1.0F / 2.4F);
            bulge = 1.1F * std::exp(-std::pow(r, 1.4F));
        }
        Vec3 light{0.0F};
        // Far from the band (above some 50 degrees) its light, even where its clouds and grain
        // brighten it most (8 times), is a percent of the background's: none of that is needed.
        if (std::max(disk, bulge) > 2e-6F) {
            // Star clouds, long along the band, and their grain down to half a degree.
            const float clouds = smoothstep(-0.45F, 0.4F, smooth.clouds);
            disk *= 0.3F + 1.4F * clouds * clouds;
            const float grain = fbm(alongBand(g, 2.0F) * 35.0F, 3, m_seed + 2U);
            disk *= square(std::max(0.0F, 1.0F + 0.6F * grain));
            bulge *= (0.9F + 0.2F * clouds) * (1.0F + 0.3F * grain);
            // Old stars warm (the bulge's most), young star clouds bluer, in patches.
            const float young = 0.7F * smoothstep(0.05F, 0.45F, smooth.young);
            const Vec3 diskColor = glm::mix(m_diskColor, m_youngColor, young);
            light = (diskColor * disk + m_bulgeColor * bulge) * (0.6F + 0.4F * speckle);
        }
        for (const Blob& nebula : m_emission) {
            light += nebula.color * (nebula.strength * along * blobShape(g, nebula));
        }
        for (const Blob& nebula : m_reflection) {
            light += nebula.color * (nebula.strength * blobShape(g, nebula));
        }
        // The dust in front of it all dims and reddens it.
        depth = dust(g, c, smooth);
        light *= glm::exp(-depth * kReddening);
        // Galaxies beyond, off the band.
        for (const Blob& galaxy : m_galaxies) {
            light += galaxy.color * (galaxy.strength * blobShape(g, galaxy));
        }
        return light * kMilkyWayRadiance;
    }

private:
    std::uint32_t m_seed;
    // The integrated light of the disk's old stars, of young star clouds, of the bulge's
    // giants (luminance 1).
    Vec3 m_diskColor = blackbodyColor(5600.0F);
    Vec3 m_youngColor = blackbodyColor(9000.0F);
    Vec3 m_bulgeColor = blackbodyColor(4700.0F);
    std::vector<Blob> m_emission;
    std::vector<Blob> m_reflection;
    std::vector<Blob> m_dark;
    std::vector<Blob> m_galaxies;
};

// The nebula ------------------------------------------------------------------------------------

/**
 * @brief SpaceSky::nebula and corona: clouds painted over the sky rather than modelled on the
 *        real one. Noise of noise of the direction curls them; the side of a cloud that faces
 *        the center is lit, and light gathers there.
 */
class Nebula
{
public:
    explicit Nebula(const SpaceSky& settings)
        : m_seed(pcgHash(settings.seed ^ 0x9D2C5680U)),
          m_coronaRadius(settings.corona * kDegree)
    {
        const float length = glm::length(settings.nebulaCenter);
        m_center = length > 0.0F ? settings.nebulaCenter / length : Vec3(0.0F, 0.0F, -1.0F);
        const Vec3 helper = std::abs(m_center.y) < 0.9F ? Vec3(0.0F, 1.0F, 0.0F)
                                                        : Vec3(1.0F, 0.0F, 0.0F);
        m_right = glm::normalize(glm::cross(helper, m_center));
        m_up = glm::cross(m_center, m_right);
    }

    struct Sample
    {
        Vec3 light{0.0F};  // Radiance at strength 1.
        float dust = 0.0F;  // Optical depth (in green) in front of the stars.
    };

    [[nodiscard]] auto clouds(Vec3 g) const -> Sample
    {
        const Vec3 wide = g * 1.7F;
        const Vec3 warp{fbm(wide, 4, m_seed), fbm(wide, 4, m_seed + 1U),
                        fbm(wide, 4, m_seed + 2U)};
        const Vec3 fine = g * 4.1F + warp * 2.2F;
        const Vec3 curl{fbm(fine, 3, m_seed + 3U), fbm(fine, 3, m_seed + 4U),
                        fbm(fine, 3, m_seed + 5U)};
        const Vec3 p = g * 2.0F + warp * 1.15F + curl * 0.28F;
        const float body = fbm(p, 6, m_seed + 6U);
        // Thinner towards the center than here: this side of the cloud faces the light.
        const float cosine = glm::dot(g, m_center);
        const float nearer = fbm(p + (m_center - g * cosine) * 0.18F, 6, m_seed + 6U);
        const float lit = std::clamp(0.5F + (body - nearer) * 3.5F, 0.0F, 1.0F);
        const float filaments = ridged(p * 2.1F + curl, 5, m_seed + 7U);
        const float cloud = 0.14F + 0.86F * smoothstep(-0.12F, 0.55F, body);
        const float wisps = cloud * (0.45F + 0.8F * filaments * filaments);

        const float angle = std::acos(std::clamp(cosine, -1.0F, 1.0F));
        const float gathered = 0.2F + 0.9F * std::exp(-square(angle / 1.0F)) +
                               1.5F * std::exp(-square(angle / 0.32F));

        const float hue = fbm(g * 1.1F + warp * 0.8F, 3, m_seed + 8U);
        const float tint = fbm(g * 1.9F - warp * 0.6F, 3, m_seed + 10U);
        Vec3 color = glm::mix(kNebulaBlue, kNebulaTeal, smoothstep(-0.3F, 0.3F, tint));
        color = glm::mix(color, kNebulaViolet, smoothstep(0.0F, 0.3F, hue));
        color = glm::mix(color, kNebulaMagenta, smoothstep(0.22F, 0.55F, hue));
        color = glm::mix(color, kNebulaIce, 0.75F * std::exp(-square(angle / 0.3F)) * lit);

        const float lanes = fbm(p * 1.5F + warp, 6, m_seed + 9U);
        const float dust = 2.4F * smoothstep(0.02F, 0.42F, lanes);
        Sample sample;
        sample.light =
            color * (kNebulaRadiance * wisps * gathered * (0.35F + 1.3F * lit) * std::exp(-dust));
        sample.dust = dust * 0.6F;
        return sample;
    }

    /** @brief The light around the star at the center: streamers falling away from its limb. */
    [[nodiscard]] auto corona(Vec3 g) const -> Vec3
    {
        if (m_coronaRadius <= 0.0F) {
            return Vec3(0.0F);
        }
        const float cosine = glm::dot(g, m_center);
        // Out to kCoronaReach radii: past that, nothing an image shows.
        if (cosine < std::cos(std::min(m_coronaRadius * kCoronaReach, kPi))) {
            return Vec3(0.0F);
        }
        const float r = std::max(std::acos(std::min(cosine, 1.0F)) / m_coronaRadius, 0.98F);
        const float around = std::atan2(glm::dot(g, m_up), glm::dot(g, m_right));
        // Around the limb on a circle, so that it joins up; slowly along the radius, so that
        // features stretch outwards into streamers.
        const Vec3 p{std::cos(around) * 4.5F, std::sin(around) * 4.5F, std::log(r) * 1.1F};
        const float streamers = ridged(p, 4, m_seed + 11U);
        // Smooth at the limb, which would look torn; the streamers start a little way out.
        const float inner = 0.55F * std::pow(r, -7.0F);
        const float outer = std::pow(r, -2.4F) *
                            (0.12F + 0.55F * streamers * streamers * smoothstep(1.0F, 1.5F, r));
        const float fade = 1.0F - smoothstep(0.55F * kCoronaReach, kCoronaReach, r);
        const Vec3 color = glm::mix(kCoronaInner, kCoronaOuter, smoothstep(1.0F, 3.5F, r));
        return color * (kCoronaRadiance * (inner + outer) * fade);
    }

private:
    static constexpr float kNebulaRadiance = 0.16F;
    static constexpr float kCoronaRadiance = 0.7F;  // At the limb, about.
    static constexpr float kCoronaReach = 14.0F;    // Radii of the star.
    static constexpr Vec3 kNebulaBlue{0.05F, 0.17F, 0.68F};
    static constexpr Vec3 kNebulaTeal{0.03F, 0.5F, 0.6F};
    static constexpr Vec3 kNebulaViolet{0.36F, 0.12F, 0.74F};
    static constexpr Vec3 kNebulaMagenta{0.8F, 0.1F, 0.42F};
    static constexpr Vec3 kNebulaIce{0.6F, 0.82F, 1.0F};
    static constexpr Vec3 kCoronaInner{0.42F, 0.66F, 1.0F};
    static constexpr Vec3 kCoronaOuter{0.08F, 0.27F, 1.0F};

    std::uint32_t m_seed;
    float m_coronaRadius;
    Vec3 m_center{0.0F, 0.0F, -1.0F};
    Vec3 m_right{1.0F, 0.0F, 0.0F};
    Vec3 m_up{0.0F, 1.0F, 0.0F};
};

// Stars ------------------------------------------------------------------------------------------

/** @brief Temperature range of a spectral class and its share of bright and of faint stars. */
struct SpectralClass
{
    float coolest;
    float hottest;
    float bright;  // Among stars brighter than magnitude 3 (the hot ones are luminous)...
    float faint;   // ... and fainter than 10 (more of the cool ones, seen nearby).
};
constexpr std::array<SpectralClass, 6> kSpectralClasses{{
    {10000.0F, 26000.0F, 0.22F, 0.03F},  // B
    {7500.0F, 10000.0F, 0.24F, 0.10F},   // A
    {6000.0F, 7500.0F, 0.13F, 0.22F},    // F
    {5200.0F, 6000.0F, 0.11F, 0.25F},    // G
    {3800.0F, 5200.0F, 0.25F, 0.28F},    // K
    {2600.0F, 3800.0F, 0.05F, 0.12F},    // M
}};

auto starColor(float magnitude, Rng& rng) -> Vec3
{
    const float faintness = smoothstep(3.0F, 10.0F, magnitude);
    float pick = rng.nextFloat();
    const SpectralClass* chosen = &kSpectralClasses.back();
    for (const SpectralClass& spectral : kSpectralClasses) {
        pick -= std::lerp(spectral.bright, spectral.faint, faintness);
        if (pick < 0.0F) {
            chosen = &spectral;
            break;
        }
    }
    const float kelvin = std::exp(
        std::lerp(std::log(chosen->coolest), std::log(chosen->hottest), rng.nextFloat()));
    const Vec3 color = glm::mix(Vec3(1.0F), blackbodyColor(kelvin), kStarSaturation);
    return color / luminance(color);
}

constexpr std::uint32_t kStarsPerChunk = 8192;

auto texelPosition(Vec3 g, std::uint32_t width, std::uint32_t height) -> Vec2;

/** @brief The dust's optical depth per map texel (Galaxy::diffuse's), looked up. */
struct DustMap
{
    const std::vector<float>& depth;
    std::uint32_t width;
    std::uint32_t height;

    [[nodiscard]] auto at(Vec3 g) const -> float
    {
        const Vec2 position = texelPosition(g, width, height);
        const auto x = std::clamp(static_cast<std::int32_t>(position.x + 0.5F), 0,
                                  static_cast<std::int32_t>(width) - 1);
        const auto y = std::clamp(static_cast<std::int32_t>(position.y + 0.5F), 0,
                                  static_cast<std::int32_t>(height) - 1);
        return depth[static_cast<std::size_t>(y) * width + static_cast<std::size_t>(x)];
    }
};

/**
 * @brief The stars down to the map's magnitude (dimmed and reddened by the dust in front
 *        of the farther, fainter ones): those to draw and those for the map.
 */
void generateStars(ParallelExecutor& executor, const DustMap& dust, const SpaceSky& settings,
                   std::vector<SkyStar>& drawn, std::vector<SkyStar>& faint)
{
    const float drawnLimit = std::clamp(settings.starMagnitude, kFaintestDrawn[0], kFaintestDrawn[1]);
    const double total = starsBrighterThan(drawnLimit + kMapMagnitudes);
    const auto count = static_cast<std::uint32_t>(total);
    const std::uint32_t chunks = (count + kStarsPerChunk - 1) / kStarsPerChunk;
    std::vector<std::vector<SkyStar>> stars(chunks);
    executor.parallelFor(chunks, [&](std::size_t chunk) {
        const auto first = static_cast<std::uint32_t>(chunk) * kStarsPerChunk;
        const std::uint32_t last = std::min(count, first + kStarsPerChunk);
        Rng rng(pcgHash(settings.seed * 0x9E3779B9U + static_cast<std::uint32_t>(chunk)));
        std::vector<SkyStar>& result = stars[chunk];
        result.reserve(last - first);
        for (std::uint32_t i = first; i < last; ++i) {
            // The magnitudes' distribution inverted: N(<m) = u N.
            const double u = 1.0 - static_cast<double>(rng.nextFloat());
            float magnitude = std::max(
                kBrightestMagnitude,
                static_cast<float>((std::log10(u * total) - kCountOffset) / kCountSlope));
            // Fainter stars, farther on average, crowd towards the band (a sky without one:
            // all over it).
            const float crowding = 10.0F * smoothstep(2.0F, 11.0F, magnitude) *
                                   std::min(settings.milkyWay, 1.0F);
            Vec3 direction;
            Galactic c{};
            do {
                direction = rng.onUnitSphere();
                c = toGalactic(direction);
            } while (rng.nextFloat() * (1.0F + crowding) >
                     1.0F + crowding * Galaxy::bandDensity(c));
            Vec3 color = starColor(magnitude, rng);
            // The dust in front of the farther ones.
            const float depth = dust.at(direction) * smoothstep(3.0F, 10.0F, magnitude);
            if (depth > 0.0F) {
                color *= glm::exp(-depth * kReddening);
                const float dimming = std::max(luminance(color), 1e-6F);
                color /= dimming;
                magnitude -= 2.5F * std::log10(dimming);
            }
            result.push_back({direction, starFlux(magnitude), color, magnitude});
        }
    });
    for (const auto& chunk : stars) {
        for (const SkyStar& star : chunk) {
            (star.magnitude <= drawnLimit ? drawn : faint).push_back(star);
        }
    }
    std::ranges::sort(drawn, {}, &SkyStar::magnitude);
}

// The map ----------------------------------------------------------------------------------------

// Of the grid of what varies over degrees (Galaxy::Smooth): some 0.35 degrees per texel.
constexpr std::uint32_t kSmoothWidth = 1024;

// How much the light of a texel's unresolved stars varies (Galaxy::diffuse's speckle).
constexpr float kSpeckleSigma = 1.6F;

/** @brief Log-normal of mean 1 (kSpeckleSigma) from a hash. */
auto logNormal(std::uint32_t hash) -> float
{
    const float u1 = (static_cast<float>(hash >> 8U) + 0.5F) * (1.0F / 16777216.0F);
    const float u2 = static_cast<float>(pcgHash(hash) >> 8U) * (1.0F / 16777216.0F);
    const float gaussian = std::sqrt(-2.0F * std::log(u1)) * std::cos(kTwoPi * u2);
    return std::exp(kSpeckleSigma * gaussian - 0.5F * kSpeckleSigma * kSpeckleSigma);
}

/** @brief Solid angle of a texel of a row of an equirectangular map. */
auto texelSolidAngle(std::uint32_t row, std::uint32_t width, std::uint32_t height) -> float
{
    const float polar = (static_cast<float>(row) + 0.5F) / static_cast<float>(height) * kPi;
    return (kTwoPi / static_cast<float>(width)) * (kPi / static_cast<float>(height)) *
           std::sin(polar);
}

/** @brief Continuous texel coordinates of a galactic direction (texel centers at +0.5). */
auto texelPosition(Vec3 g, std::uint32_t width, std::uint32_t height) -> Vec2
{
    const Galactic c = toGalactic(g);
    return {(c.longitude / kTwoPi + 0.5F) * static_cast<float>(width) - 0.5F,
            (0.5F - c.latitude / kPi) * static_cast<float>(height) - 0.5F};
}

/** @brief The four texels around a position and their bilinear weights. */
struct Bilinear
{
    std::array<std::uint32_t, 4> index;
    std::array<float, 4> weight;
};

auto bilinear(Vec2 position, std::uint32_t width, std::uint32_t height) -> Bilinear
{
    const float floorX = std::floor(position.x);
    const float floorY = std::floor(position.y);
    const float fx = position.x - floorX;
    const float fy = position.y - floorY;
    const auto w = static_cast<std::int32_t>(width);
    const auto h = static_cast<std::int32_t>(height);
    std::int32_t x0 = static_cast<std::int32_t>(floorX);
    if (x0 < 0) {
        x0 += w;
    }
    const std::int32_t x1 = x0 + 1 == w ? 0 : x0 + 1;
    const std::int32_t y0 = std::max(static_cast<std::int32_t>(floorY), 0);
    const std::int32_t y1 = std::min(static_cast<std::int32_t>(floorY) + 1, h - 1);
    auto at = [&](std::int32_t x, std::int32_t y) { return static_cast<std::uint32_t>(y * w + x); };
    return {{at(x0, y0), at(x1, y0), at(x0, y1), at(x1, y1)},
            {(1.0F - fx) * (1.0F - fy), fx * (1.0F - fy), (1.0F - fx) * fy, fx * fy}};
}

auto buildSkyMap(const SpaceSky& settings, std::uint32_t width) -> std::shared_ptr<const SkyMap>
{
    auto map = std::make_shared<SkyMap>();
    map->width = std::max(width, 4U);
    map->height = map->width / 2;
    const std::uint32_t w = map->width;
    const std::uint32_t h = map->height;
    const Galaxy galaxy(settings.seed);
    ParallelExecutor executor;

    // What varies over degrees, on a coarser grid of its own (whatever the map's width).
    constexpr std::uint32_t sw = kSmoothWidth;
    constexpr std::uint32_t sh = kSmoothWidth / 2;
    std::vector<Galaxy::Smooth> smooth(static_cast<std::size_t>(sw) * sh);
    executor.parallelFor(sh, [&](std::size_t row) {
        const float latitude =
            kPi / 2.0F - (static_cast<float>(row) + 0.5F) / static_cast<float>(sh) * kPi;
        for (std::uint32_t column = 0; column < sw; ++column) {
            const float longitude =
                (static_cast<float>(column) + 0.5F) / static_cast<float>(sw) * kTwoPi - kPi;
            smooth[row * sw + column] = galaxy.smooth(fromGalactic(longitude, latitude));
        }
    });

    // The diffuse light and the dust, row by row.
    std::vector<Vec3> image(static_cast<std::size_t>(w) * h);
    std::vector<float> depth(image.size());
    const std::uint32_t speckleSeed = pcgHash(settings.seed ^ 0x5bd1e995U);
    std::vector<float> longitudes(w);
    std::vector<Vec2> longitudeCosSin(w);
    for (std::uint32_t column = 0; column < w; ++column) {
        longitudes[column] =
            (static_cast<float>(column) + 0.5F) / static_cast<float>(w) * kTwoPi - kPi;
        longitudeCosSin[column] = {std::cos(longitudes[column]), std::sin(longitudes[column])};
    }
    executor.parallelFor(h, [&](std::size_t row) {
        const float latitude =
            kPi / 2.0F - (static_cast<float>(row) + 0.5F) / static_cast<float>(h) * kPi;
        const float cosLatitude = std::cos(latitude);
        const float sinLatitude = std::sin(latitude);
        // Where the texels' centers are on the coarse grid.
        const float y = (static_cast<float>(row) + 0.5F) * (static_cast<float>(sh) / h) - 0.5F;
        for (std::uint32_t column = 0; column < w; ++column) {
            const Vec3 g{cosLatitude * longitudeCosSin[column].x, sinLatitude,
                         cosLatitude * longitudeCosSin[column].y};
            const float x =
                (static_cast<float>(column) + 0.5F) * (static_cast<float>(sw) / w) - 0.5F;
            const Bilinear coarse = bilinear({x, y}, sw, sh);
            Galaxy::Smooth s;
            for (std::size_t k = 0; k < 4; ++k) {
                const Galaxy::Smooth& texel = smooth[coarse.index[k]];
                s.clouds += texel.clouds * coarse.weight[k];
                s.young += texel.young * coarse.weight[k];
                s.cirrus += texel.cirrus * coarse.weight[k];
                s.wave += texel.wave * coarse.weight[k];
                s.warp += texel.warp * coarse.weight[k];
            }
            const std::size_t i = row * w + column;
            const float speckle = logNormal(pcgHash(static_cast<std::uint32_t>(i) ^ speckleSeed));
            image[i] =
                galaxy.diffuse(g, {longitudes[column], latitude}, s, speckle, depth[i]) *
                    settings.milkyWay +
                Vec3(kBackgroundRadiance);
        }
    });

    // The nebula's clouds, soft enough for half the texels (most of a map's work otherwise),
    // and the star's corona, whose limb is sharp, at each.
    if (settings.nebula > 0.0F || settings.corona > 0.0F) {
        const Nebula nebula(settings);
        const std::uint32_t cw = std::max(w / 2, 4U);
        const std::uint32_t ch = cw / 2;
        std::vector<Nebula::Sample> clouds(static_cast<std::size_t>(cw) * ch);
        if (settings.nebula > 0.0F) {
            executor.parallelFor(ch, [&](std::size_t row) {
                const float latitude =
                    kPi / 2.0F - (static_cast<float>(row) + 0.5F) / static_cast<float>(ch) * kPi;
                for (std::uint32_t column = 0; column < cw; ++column) {
                    const float longitude =
                        (static_cast<float>(column) + 0.5F) / static_cast<float>(cw) * kTwoPi - kPi;
                    clouds[row * cw + column] = nebula.clouds(fromGalactic(longitude, latitude));
                }
            });
        }
        executor.parallelFor(h, [&](std::size_t row) {
            const float latitude =
                kPi / 2.0F - (static_cast<float>(row) + 0.5F) / static_cast<float>(h) * kPi;
            const float cosLatitude = std::cos(latitude);
            const float sinLatitude = std::sin(latitude);
            const float y = (static_cast<float>(row) + 0.5F) * (static_cast<float>(ch) / h) - 0.5F;
            for (std::uint32_t column = 0; column < w; ++column) {
                const std::size_t i = row * w + column;
                image[i] += nebula.corona({cosLatitude * longitudeCosSin[column].x, sinLatitude,
                                           cosLatitude * longitudeCosSin[column].y});
                if (settings.nebula > 0.0F) {
                    const float x =
                        (static_cast<float>(column) + 0.5F) * (static_cast<float>(cw) / w) - 0.5F;
                    const Bilinear coarse = bilinear({x, y}, cw, ch);
                    for (std::size_t k = 0; k < 4; ++k) {
                        const Nebula::Sample& texel = clouds[coarse.index[k]];
                        image[i] += texel.light * (settings.nebula * coarse.weight[k]);
                        depth[i] += texel.dust * coarse.weight[k];
                    }
                }
            }
        });
    }

    // The stars: the faint ones spread over the texels around them, by bands of rows in
    // parallel (each adds what falls on its own rows, in the stars' order).
    std::vector<SkyStar> faint;
    generateStars(executor, DustMap{depth, w, h}, settings, map->stars, faint);
    std::vector<Vec2> positions(faint.size());
    executor.parallelFor((faint.size() + kStarsPerChunk - 1) / kStarsPerChunk,
                         [&](std::size_t chunk) {
                             const std::size_t first = chunk * kStarsPerChunk;
                             const std::size_t last =
                                 std::min(faint.size(), first + kStarsPerChunk);
                             for (std::size_t i = first; i < last; ++i) {
                                 positions[i] = texelPosition(faint[i].direction, w, h);
                             }
                         });
    std::vector<float> rowSolidAngle(h);
    for (std::uint32_t row = 0; row < h; ++row) {
        rowSolidAngle[row] = texelSolidAngle(row, w, h);
    }
    constexpr std::uint32_t kRowsPerBand = 32;
    executor.parallelFor((h + kRowsPerBand - 1) / kRowsPerBand, [&](std::size_t band) {
        const auto top = static_cast<std::uint32_t>(band) * kRowsPerBand;
        const std::uint32_t bottom = std::min(top + kRowsPerBand, h);
        for (std::size_t i = 0; i < faint.size(); ++i) {
            // Its rows: floor(y) and the next.
            if (positions[i].y < static_cast<float>(top) - 1.0F ||
                positions[i].y >= static_cast<float>(bottom)) {
                continue;
            }
            const Bilinear texels = bilinear(positions[i], w, h);
            for (std::size_t k = 0; k < 4; ++k) {
                const std::uint32_t row = texels.index[k] / w;
                if (row >= top && row < bottom) {
                    image[texels.index[k]] +=
                        faint[i].color * (faint[i].flux * texels.weight[k] / rowSolidAngle[row]);
                }
            }
        }
    });

    // Packed, and the mean radiance (texels by their solid angle, the drawn stars).
    map->texels.resize(image.size());
    std::vector<double> rowSums(static_cast<std::size_t>(h) * 3);
    {
        executor.parallelFor(h, [&](std::size_t row) {
            Vec3 sum{0.0F};
            for (std::uint32_t column = 0; column < w; ++column) {
                const Vec3 radiance = image[row * w + column];
                map->texels[row * w + column] = encodeRgb9e5(radiance);
                sum += radiance;
            }
            const float solidAngle = texelSolidAngle(static_cast<std::uint32_t>(row), w, h);
            for (int c = 0; c < 3; ++c) {
                rowSums[row * 3 + c] = static_cast<double>(sum[c] * solidAngle);
            }
        });
    }
    std::array<double, 3> total{};
    for (std::size_t row = 0; row < h; ++row) {
        for (int c = 0; c < 3; ++c) {
            total[c] += rowSums[row * 3 + c];
        }
    }
    for (const SkyStar& star : map->stars) {
        for (int c = 0; c < 3; ++c) {
            total[c] += static_cast<double>(star.color[c] * star.flux);
        }
    }
    for (int c = 0; c < 3; ++c) {
        map->meanRadiance[c] = static_cast<float>(total[c] / (4.0 * std::numbers::pi));
    }
    return map;
}

}  // namespace

auto SkyMap::radiance(Vec3 galacticDirection) const -> Vec3
{
    if (texels.empty()) {
        return Vec3(0.0F);
    }
    const Bilinear texel = bilinear(texelPosition(galacticDirection, width, height), width, height);
    Vec3 result{0.0F};
    for (std::size_t i = 0; i < 4; ++i) {
        result += decodeRgb9e5(texels[texel.index[i]]) * texel.weight[i];
    }
    return result;
}

auto generateSkyMap(const SpaceSky& settings, std::uint32_t width) -> std::shared_ptr<const SkyMap>
{
    // Maps in use, by what they are made of.
    struct Entry
    {
        std::uint32_t seed;
        float milkyWay;
        float starMagnitude;
        float nebula;
        Vec3 nebulaCenter;
        float corona;
        std::uint32_t width;
        std::weak_ptr<const SkyMap> map;
    };
    static std::mutex mutex;
    static std::vector<Entry> entries;
    {
        const std::scoped_lock lock(mutex);
        std::erase_if(entries, [](const Entry& entry) { return entry.map.expired(); });
        for (const Entry& entry : entries) {
            if (entry.seed == settings.seed && entry.milkyWay == settings.milkyWay &&
                entry.starMagnitude == settings.starMagnitude &&
                entry.nebula == settings.nebula && entry.nebulaCenter == settings.nebulaCenter &&
                entry.corona == settings.corona && entry.width == width) {
                if (auto map = entry.map.lock()) {
                    return map;
                }
            }
        }
    }
    const auto start = std::chrono::steady_clock::now();
    auto map = buildSkyMap(settings, width);
    SC_CORE_INFO("Space sky {}: {}x{} map, {} stars drawn, in {:.0f} ms", settings.seed, width,
                 width / 2, map->stars.size(),
                 std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
                     .count());
    const std::scoped_lock lock(mutex);
    entries.push_back({settings.seed, settings.milkyWay, settings.starMagnitude, settings.nebula,
                       settings.nebulaCenter, settings.corona, width, map});
    return map;
}

auto worldToGalactic(const SpaceSky& settings) -> Mat3
{
    // The orientation turns galactic directions into world ones.
    return glm::transpose(glm::mat3_cast(Quat(glm::radians(settings.orientation))));
}

auto starsBrighterThan(float magnitude) -> double
{
    return std::pow(10.0, kCountOffset + kCountSlope * static_cast<double>(magnitude));
}

auto starFlux(float magnitude) -> float
{
    return kZeroMagnitudeFlux * std::pow(10.0F, -0.4F * magnitude);
}

namespace
{

/** @brief Lobe of a fit of the CIE 1931 color matching functions (Wyman et al. 2013). */
auto lobe(float wavelength, float mean, float below, float above) -> float
{
    const float t = (wavelength - mean) / (wavelength < mean ? below : above);
    return std::exp(-0.5F * t * t);
}

auto computeBlackbody(float kelvin) -> Vec3
{
    // Planck's law over the visible spectrum, weighted by the color matching functions.
    constexpr float kSecondRadiation = 1.4388e7F;  // hc / k in nm K.
    Vec3 xyz{0.0F};
    for (float wavelength = 380.0F; wavelength <= 780.0F; wavelength += 5.0F) {
        const float l5 = std::pow(wavelength * 1e-3F, 5.0F);
        const float planck = 1.0F / (l5 * (std::exp(kSecondRadiation / (wavelength * kelvin)) - 1.0F));
        const float x = 1.056F * lobe(wavelength, 599.8F, 37.9F, 31.0F) +
                        0.362F * lobe(wavelength, 442.0F, 16.0F, 26.7F) -
                        0.065F * lobe(wavelength, 501.1F, 20.4F, 26.2F);
        const float y = 0.821F * lobe(wavelength, 568.8F, 46.9F, 40.5F) +
                        0.286F * lobe(wavelength, 530.9F, 16.3F, 31.1F);
        const float z = 1.217F * lobe(wavelength, 437.0F, 11.8F, 36.0F) +
                        0.681F * lobe(wavelength, 459.0F, 26.0F, 13.8F);
        xyz += planck * Vec3(x, y, z);
    }
    // XYZ to linear sRGB (D65), out-of-gamut parts clipped.
    const Vec3 rgb = glm::max(
        Vec3(3.2406F * xyz.x - 1.5372F * xyz.y - 0.4986F * xyz.z,
             -0.9689F * xyz.x + 1.8758F * xyz.y + 0.0415F * xyz.z,
             0.0557F * xyz.x - 0.2040F * xyz.y + 1.0570F * xyz.z),
        Vec3(0.0F));
    return rgb / std::max(luminance(rgb), 1e-30F);
}

constexpr std::size_t kBlackbodyTable = 256;
constexpr float kTableCoolest = 1000.0F;
constexpr float kTableHottest = 40000.0F;

}  // namespace

auto blackbodyColor(float kelvin) -> Vec3
{
    // Tabulated over the logarithm of the temperature.
    static const auto table = [] {
        std::array<Vec3, kBlackbodyTable> values{};
        for (std::size_t i = 0; i < kBlackbodyTable; ++i) {
            const float t = static_cast<float>(i) / static_cast<float>(kBlackbodyTable - 1);
            values[i] = computeBlackbody(
                std::exp(std::lerp(std::log(kTableCoolest), std::log(kTableHottest), t)));
        }
        return values;
    }();
    const float t = (std::log(std::clamp(kelvin, kTableCoolest, kTableHottest)) -
                     std::log(kTableCoolest)) /
                    (std::log(kTableHottest) - std::log(kTableCoolest)) *
                    static_cast<float>(kBlackbodyTable - 1);
    const auto i = std::min(static_cast<std::size_t>(t), kBlackbodyTable - 2);
    const Vec3 color = glm::mix(table[i], table[i + 1], t - static_cast<float>(i));
    return color / luminance(color);
}

auto encodeRgb9e5(Vec3 color) -> std::uint32_t
{
    // EXT_texture_shared_exponent: 9-bit mantissas, a 5-bit exponent (bias 15).
    constexpr int kMantissaBits = 9;
    constexpr int kBias = 15;
    constexpr float kMax = 511.0F / 512.0F * 65536.0F;
    const Vec3 c = glm::clamp(color, Vec3(0.0F), Vec3(kMax));
    const float largest = std::max({c.r, c.g, c.b});
    int exponent = std::max(-kBias - 1, static_cast<int>(std::floor(std::log2(std::max(largest, 1e-30F))))) +
                   1 + kBias;
    float scale = std::ldexp(1.0F, exponent - kBias - kMantissaBits);
    if (static_cast<int>(std::floor(largest / scale + 0.5F)) == (1 << kMantissaBits)) {
        ++exponent;
        scale *= 2.0F;
    }
    auto mantissa = [&](float value) {
        return static_cast<std::uint32_t>(std::floor(value / scale + 0.5F)) & 0x1FFU;
    };
    return mantissa(c.r) | (mantissa(c.g) << 9U) | (mantissa(c.b) << 18U) |
           (static_cast<std::uint32_t>(exponent) << 27U);
}

auto decodeRgb9e5(std::uint32_t packed) -> Vec3
{
    const float scale = std::ldexp(1.0F, static_cast<int>(packed >> 27U) - 15 - 9);
    return Vec3(static_cast<float>(packed & 0x1FFU), static_cast<float>((packed >> 9U) & 0x1FFU),
                static_cast<float>((packed >> 18U) & 0x1FFU)) *
           scale;
}

}  // namespace starcore
