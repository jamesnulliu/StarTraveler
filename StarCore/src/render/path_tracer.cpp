#include <algorithm>
#include <optional>

#include <glm/geometric.hpp>

#include "starcore/render/path_tracer.hpp"
#include "starcore/render/sampling.hpp"

namespace starcore
{

namespace
{

using namespace sampling;

constexpr std::uint32_t kTileSize = 16;
constexpr std::uint32_t kRussianRouletteStart = 3;
// Thin surfaces a shadow ray (or a preview ray) may pass through.
constexpr int kMaxThinLayers = 8;
// Thin surfaces transmitting at least this much do not take the primary id.
constexpr float kSeeThrough = 0.5F;

auto maxComponent(Vec3 v) -> float
{
    return std::max({v.r, v.g, v.b});
}

/** @brief Ray origin just off the surface, on the side `direction` leaves to. */
auto offsetOrigin(Vec3 position, Vec3 geometricNormal, Vec3 direction) -> Vec3
{
    const float scale = 1e-4F + 2e-6F * maxComponent(glm::abs(position));
    return position +
           geometricNormal * (glm::dot(direction, geometricNormal) >= 0.0F ? scale : -scale);
}

auto isThinTransmissive(const RenderMaterial& m) -> bool
{
    return (m.flags & kMaterialThinWalled) != 0U && m.transmission > 0.0F;
}

auto isSeeThrough(const RenderMaterial& m) -> bool
{
    return (m.flags & kMaterialThinWalled) != 0U &&
           m.transmission * (1.0F - m.metallic) >= kSeeThrough;
}

/** @brief Entity of the first surface along a ray that is not see-through. */
auto firstOpaqueEntity(const RenderScene& scene, Ray ray, std::uint32_t mask) -> std::uint32_t
{
    for (int layer = 0; layer < kMaxThinLayers; ++layer) {
        const auto hit = traceClosestHit(scene, ray, 0.0F, kInfinity, mask);
        if (!hit) {
            break;
        }
        const RenderInstance& instance = scene.instances[hit->instanceIndex];
        if (!isSeeThrough(scene.materials[instance.materialIndex])) {
            return instance.entityId;
        }
        ray.origin = offsetOrigin(hit->position, hit->geometricNormal, ray.direction);
    }
    return kNoEntityId;
}

/** @brief Expected straight-through transmission of a thin-walled surface. */
auto thinTransmittance(const RenderMaterial& m, float cosTheta) -> Vec3
{
    const float reflectance = fresnelDielectric(cosTheta, 1.0F / m.ior);
    return Vec3(m.albedo) * ((1.0F - m.metallic) * m.transmission * (1.0F - reflectance));
}

auto materialOf(const RenderScene& scene, const SurfaceHit& hit) -> const RenderMaterial&
{
    return scene.materials[scene.instances[hit.instanceIndex].materialIndex];
}

/**
 * @brief Fraction of light from `lightInstance` arriving along `ray`: 1 when
 *        unoccluded, attenuated by thin-walled transmissive surfaces, 0 when
 *        blocked by anything else (or when the ray misses the light).
 */
auto shadowTransmittance(const RenderScene& scene, Ray ray, std::uint32_t lightInstance,
                         std::uint32_t mask) -> Vec3
{
    Vec3 transmittance{1.0F};
    for (int layer = 0; layer < kMaxThinLayers; ++layer) {
        const auto hit = traceClosestHit(scene, ray, 0.0F, kInfinity, mask);
        if (!hit) {
            return Vec3(0.0F);
        }
        if (hit->instanceIndex == lightInstance) {
            return transmittance;
        }
        const RenderMaterial& material = materialOf(scene, *hit);
        if (!isThinTransmissive(material)) {
            return Vec3(0.0F);
        }
        transmittance *=
            thinTransmittance(material, std::abs(glm::dot(ray.direction, hit->geometricNormal)));
        if (maxComponent(transmittance) <= 0.0F) {
            return Vec3(0.0F);
        }
        ray.origin = offsetOrigin(hit->position, hit->geometricNormal, ray.direction);
    }
    return Vec3(0.0F);
}

/** @brief Solid-angle pdf of sampling light `index` from `position` (0 from inside it). */
auto directLightPdf(const RenderScene& scene, std::size_t index, Vec3 position) -> float
{
    const RenderLight& light = scene.lights[index];
    const Vec3 toCenter = light.center - position;
    const float distance2 = glm::dot(toCenter, toCenter);
    if (distance2 <= light.radius * light.radius) {
        return 0.0F;
    }
    return conePdf(coneOneMinusCos(light.radius, distance2)) /
           static_cast<float>(scene.lights.size());
}

/**
 * @brief Next event estimation for the diffuse lobe: radiance of one sampled
 *        sphere light times cos / pi, MIS-weighted against cosine sampling
 *        (the albedo is applied by the caller).
 */
auto sampleDirectLight(const RenderScene& scene, const PathTracerSettings& settings,
                       const SurfaceHit& hit, Rng& rng) -> Vec3
{
    const std::size_t count = scene.lights.size();
    const std::size_t index =
        std::min(count - 1, static_cast<std::size_t>(rng.nextFloat() * static_cast<float>(count)));
    const RenderLight& light = scene.lights[index];
    const Vec3 toCenter = light.center - hit.position;
    const float distance2 = glm::dot(toCenter, toCenter);
    const float u1 = rng.nextFloat();
    const float u2 = rng.nextFloat();
    if (distance2 <= light.radius * light.radius) {
        return Vec3(0.0F);
    }
    const float oneMinusCos = coneOneMinusCos(light.radius, distance2);
    const Vec3 direction = sampleCone(toCenter / std::sqrt(distance2), oneMinusCos, u1, u2);
    const float cosine = glm::dot(direction, hit.normal);
    if (cosine <= 0.0F || glm::dot(direction, hit.geometricNormal) <= 0.0F) {
        return Vec3(0.0F);
    }
    const Vec3 transmittance = shadowTransmittance(
        scene, Ray{offsetOrigin(hit.position, hit.geometricNormal, direction), direction},
        light.instanceIndex, settings.instanceMask);
    if (maxComponent(transmittance) <= 0.0F) {
        return Vec3(0.0F);
    }
    const float lightPdf = conePdf(oneMinusCos) / static_cast<float>(count);
    const float bsdfPdf = cosine / kPi;
    return light.emission * transmittance * (cosine / kPi) *
           (powerHeuristic(lightPdf, bsdfPdf) / lightPdf);
}

enum class Lobe : std::uint8_t
{
    Diffuse,
    Specular,
    Transmission,
};

struct Scatter
{
    Vec3 direction;
    Vec3 weight;  // BSDF * cos / (pdf * lobe probability).
    Lobe lobe = Lobe::Diffuse;
    // Straight through a thin sheet: the path keeps its MIS state.
    bool passThrough = false;
};

/**
 * @brief Sample the BSDF: metal (GGX, Schlick Fresnel tinted by the base
 *        color) or dielectric (GGX coat with exact Fresnel over either
 *        transmission -- thin sheet or refracting volume -- or Lambert).
 *        Lobes are chosen with probabilities equal to their energy.
 *
 * Multiple scattering between microfacets is approximated in an energy-
 * conserving way: the masking-shadowing weight is dropped and directions
 * that a single microfacet sends to the wrong side of the surface are
 * folded back, so white materials pass the white furnace test (as reference
 * multiple-scattering GGX does) instead of darkening at grazing angles.
 *
 * @return nullopt when the path is absorbed (only where shading and
 *         geometric normals disagree).
 */
auto sampleBsdf(const RenderMaterial& material, Vec3 wo, const SurfaceHit& hit, Rng& rng)
    -> std::optional<Scatter>
{
    const Vec3 baseColor(material.albedo);
    const Frame frame = makeFrame(hit.normal);
    Vec3 woLocal = frame.toLocal(wo);
    woLocal.z = std::max(woLocal.z, 1e-5F);
    woLocal = glm::normalize(woLocal);
    const float alpha = roughnessToAlpha(material.roughness);

    auto reflectLocal = [&](Vec3 h) {
        Vec3 wi = 2.0F * glm::dot(woLocal, h) * h - woLocal;
        wi.z = std::abs(wi.z);  // Fold multiply-scattered light back up.
        return wi;
    };
    auto aboveSurface = [&](Vec3 world) { return glm::dot(world, hit.geometricNormal) > 0.0F; };

    const float u1 = rng.nextFloat();
    const float u2 = rng.nextFloat();
    const Vec3 h = sampleGgxVisibleNormal(woLocal, alpha, u1, u2);
    if (rng.nextFloat() < material.metallic) {
        const Vec3 wi = reflectLocal(h);
        const Vec3 world = frame.toWorld(wi);
        if (!aboveSurface(world)) {
            return std::nullopt;
        }
        return Scatter{world, fresnelSchlick(baseColor, glm::dot(woLocal, h)), Lobe::Specular};
    }

    const bool thin = (material.flags & kMaterialThinWalled) != 0U;
    // n_incident / n_transmitted: entering the volume through its front face.
    const float eta = thin || hit.frontFace ? 1.0F / material.ior : material.ior;
    const float reflectance = fresnelDielectric(glm::dot(woLocal, h), eta);
    if (rng.nextFloat() < reflectance) {
        const Vec3 world = frame.toWorld(reflectLocal(h));
        if (!aboveSurface(world)) {
            return std::nullopt;
        }
        return Scatter{world, Vec3(1.0F), Lobe::Specular};
    }

    if (rng.nextFloat() < material.transmission) {
        Vec3 transmitted{0.0F};
        if (thin) {
            // A sheet: refracting in and out cancels; mirror the reflection.
            transmitted = reflectLocal(h);
        } else if (!refract(-woLocal, h, eta, transmitted)) {
            return std::nullopt;  // Unreachable: the Fresnel term was 1.
        }
        transmitted.z = -std::abs(transmitted.z);  // Always through the surface.
        const Vec3 world = glm::normalize(frame.toWorld(transmitted));
        if (aboveSurface(world)) {
            return std::nullopt;
        }
        return Scatter{world, baseColor, Lobe::Transmission, thin};
    }

    const Vec3 direction = rng.cosineHemisphere(hit.normal);
    if (!aboveSurface(direction)) {
        return std::nullopt;
    }
    return Scatter{direction, baseColor, Lobe::Diffuse};
}
}  // namespace

auto tracePath(const RenderScene& scene, Ray ray, const PathTracerSettings& settings, Rng& rng,
               std::uint32_t* primaryEntityId) -> Vec3
{
    Vec3 radiance{0.0F};
    Vec3 throughput{1.0F};
    if (primaryEntityId != nullptr) {
        *primaryEntityId = firstOpaqueEntity(scene, ray, settings.instanceMask);
    }
    const bool nextEvent = settings.nextEventEstimation && !scene.lights.empty();

    // Indirect light (two or more scattering events between the light and the
    // camera) is optionally clamped against fireflies; direct light never is.
    auto add = [&](Vec3 contribution, std::uint32_t scatteringEvents) {
        if (settings.fireflyClamp > 0.0F && scatteringEvents > 1) {
            const float peak = maxComponent(contribution);
            if (peak > settings.fireflyClamp) {
                contribution *= settings.fireflyClamp / peak;
            }
        }
        radiance += contribution;
    };

    // Scattering events so far (straight passes through window panes don't count).
    std::uint32_t scatterings = 0;
    // Last diffuse vertex, to MIS-weight lights found by BSDF sampling.
    bool fromDiffuse = false;
    Vec3 diffusePosition{0.0F};
    float diffusePdf = 0.0F;

    for (std::uint32_t bounce = 0; bounce < settings.maxBounces; ++bounce) {
        const auto hit = traceClosestHit(scene, ray, 0.0F, kInfinity, settings.instanceMask);
        if (!hit.has_value()) {
            add(throughput * scene.skyColor, scatterings);
            break;
        }
        const RenderMaterial& material =
            scene.materials[scene.instances[hit->instanceIndex].materialIndex];

        const Vec3 emission(material.emission);
        if (maxComponent(emission) > 0.0F) {
            float weight = 1.0F;
            if (nextEvent && fromDiffuse) {
                if (const std::int32_t light = scene.lightOfInstance[hit->instanceIndex];
                    light >= 0) {
                    weight = powerHeuristic(
                        diffusePdf,
                        directLightPdf(scene, static_cast<std::size_t>(light), diffusePosition));
                }
            }
            add(throughput * emission * weight, scatterings);
        }

        const Vec3 wo = -glm::normalize(ray.direction);
        const auto scatter = sampleBsdf(material, wo, *hit, rng);
        if (!scatter.has_value()) {
            break;  // Absorbed.
        }
        if (scatter->lobe == Lobe::Diffuse && nextEvent) {
            add(throughput * Vec3(material.albedo) * sampleDirectLight(scene, settings, *hit, rng),
                scatterings + 1);
        }
        throughput *= scatter->weight;
        if (!scatter->passThrough) {
            ++scatterings;
        }
        if (scatter->lobe == Lobe::Diffuse) {
            fromDiffuse = true;
            diffusePosition = hit->position;
            diffusePdf = std::max(0.0F, glm::dot(scatter->direction, hit->normal)) / kPi;
        } else if (!scatter->passThrough) {
            fromDiffuse = false;
        }

        if (bounce >= kRussianRouletteStart) {
            const float survival = std::min(1.0F, maxComponent(throughput));
            if (rng.nextFloat() >= survival) {
                break;
            }
            throughput /= survival;
        }
        ray = Ray{offsetOrigin(hit->position, hit->geometricNormal, scatter->direction),
                  scatter->direction};
    }
    return radiance;
}

auto shadePreview(const RenderScene& scene, Ray ray, const PathTracerSettings& settings,
                  std::uint32_t* primaryEntityId) -> Vec3
{
    constexpr float kAmbient = 0.04F;
    constexpr float kHeadlight = 0.15F;
    if (primaryEntityId != nullptr) {
        *primaryEntityId = firstOpaqueEntity(scene, ray, settings.instanceMask);
    }
    Vec3 result{0.0F};
    Vec3 throughput{1.0F};
    for (int layer = 0; layer < kMaxThinLayers; ++layer) {
        const auto hit = traceClosestHit(scene, ray, 0.0F, kInfinity, settings.instanceMask);
        if (!hit.has_value()) {
            return result + throughput * scene.skyColor;
        }
        const RenderMaterial& material =
            scene.materials[scene.instances[hit->instanceIndex].materialIndex];

        // Diffuse response to: the sky (ambient), a headlight and the sphere
        // lights (exact irradiance of a sphere above the horizon, shadowed).
        Vec3 irradiance = scene.skyColor + Vec3(kAmbient) +
                          Vec3(kHeadlight * std::max(0.0F, -glm::dot(hit->normal, ray.direction)));
        for (const RenderLight& light : scene.lights) {
            const Vec3 toCenter = light.center - hit->position;
            const float distance2 = glm::dot(toCenter, toCenter);
            if (distance2 <= light.radius * light.radius) {
                continue;
            }
            const Vec3 direction = toCenter / std::sqrt(distance2);
            const float cosine = glm::dot(direction, hit->normal);
            if (cosine <= 0.0F) {
                continue;
            }
            irradiance += light.emission * (cosine * light.radius * light.radius / distance2) *
                          shadowTransmittance(
                              scene,
                              Ray{offsetOrigin(hit->position, hit->geometricNormal, direction),
                                  direction},
                              light.instanceIndex, settings.instanceMask);
        }

        const float transmitted = isThinTransmissive(material)
                                      ? (1.0F - material.metallic) * material.transmission
                                      : 0.0F;
        result += throughput * (Vec3(material.emission) +
                                Vec3(material.albedo) * irradiance * (1.0F - transmitted));
        if (transmitted <= 0.0F) {
            return result;
        }
        throughput *= Vec3(material.albedo) * transmitted;
        ray.origin = offsetOrigin(hit->position, hit->geometricNormal, ray.direction);
    }
    return result;
}

CpuPathTracer::CpuPathTracer(unsigned threadCount) : m_executor(threadCount)
{
}

void CpuPathTracer::resize(std::uint32_t width, std::uint32_t height)
{
    if (width == m_image.width() && height == m_image.height()) {
        return;
    }
    m_accumulation.resize(width, height, Vec4(0.0F));
    m_image.resize(width, height, 0U);
    m_entityIds.resize(width, height, kNoEntityId);
    resetAccumulation();
}

auto CpuPathTracer::needsReset(const RenderScene& scene, const Camera& camera,
                               const PathTracerSettings& settings) const -> bool
{
    return m_frameIndex == 0 || !settings.accumulate ||
           scene.sceneVersion != m_lastSceneVersion || camera.view() != m_lastView ||
           camera.projection() != m_lastProjection || settings != m_lastSettings;
}

void CpuPathTracer::render(const RenderScene& scene, const Camera& camera,
                           const PathTracerSettings& settings)
{
    const std::uint32_t width = m_image.width();
    const std::uint32_t height = m_image.height();
    if (width == 0 || height == 0) {
        return;
    }
    if (needsReset(scene, camera, settings)) {
        m_frameIndex = 0;
        m_accumulation.fill(Vec4(0.0F));
    }
    m_lastSceneVersion = scene.sceneVersion;
    m_lastView = camera.view();
    m_lastProjection = camera.projection();
    m_lastSettings = settings;

    const std::uint32_t frame = m_frameIndex++;
    const bool writeIds = frame == 0;
    const bool preview = settings.mode == RenderMode::Preview;
    const std::uint32_t samples = preview ? 1U : std::max(1U, settings.samplesPerPixel);
    const std::uint32_t tilesX = (width + kTileSize - 1) / kTileSize;
    const std::uint32_t tilesY = (height + kTileSize - 1) / kTileSize;

    m_executor.parallelFor(tilesX * tilesY, [&](std::size_t tile) {
        const std::uint32_t x0 = static_cast<std::uint32_t>(tile % tilesX) * kTileSize;
        const std::uint32_t y0 = static_cast<std::uint32_t>(tile / tilesX) * kTileSize;
        const std::uint32_t x1 = std::min(width, x0 + kTileSize);
        const std::uint32_t y1 = std::min(height, y0 + kTileSize);

        for (std::uint32_t y = y0; y < y1; ++y) {
            for (std::uint32_t x = x0; x < x1; ++x) {
                Rng rng = Rng::forPixel(x, y, frame, settings.seed);
                Vec3 color{0.0F};
                std::uint32_t entityId = kNoEntityId;
                for (std::uint32_t s = 0; s < samples; ++s) {
                    // Jittered sub-pixel position: anti-aliasing for free.
                    const Vec2 pixel{static_cast<float>(x) + rng.nextFloat(),
                                     static_cast<float>(y) + rng.nextFloat()};
                    std::uint32_t* idOut = s == 0 && writeIds ? &entityId : nullptr;
                    const Ray ray = camera.generateRay(pixel);
                    color += preview ? shadePreview(scene, ray, settings, idOut)
                                     : tracePath(scene, ray, settings, rng, idOut);
                }
                color /= static_cast<float>(samples);

                Vec4& accumulated = m_accumulation.at(x, y);
                accumulated += Vec4(color, 1.0F);
                Vec3 display = Vec3(accumulated) / accumulated.a;
                display = toneMap(display * settings.exposure, settings.toneMapper);
                if (settings.srgbOutput) {
                    display = linearToSrgb(display);
                }
                m_image.at(x, y) = packRgba8(Vec4(display, 1.0F));
                if (writeIds) {
                    m_entityIds.at(x, y) = entityId;
                }
            }
        }
    });
}

}  // namespace starcore
