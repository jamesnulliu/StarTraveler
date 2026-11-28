#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <optional>
#include <vector>

#include <glm/geometric.hpp>

#include "starcore/render/exposure.hpp"
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
// Shadow rays to a point of a mesh light stop this fraction of the way short of it.
constexpr float kShadowShortening = 1e-3F;
// Reflections at least this rough also sample the lights (smoother ones are too
// sharp for it: their lights are found by sampling the reflection).
constexpr float kGlossyLightRoughness = 0.15F;

auto maxComponent(Vec3 v) -> float
{
    return std::max(v.r, std::max(v.g, v.b));  // Not std::max({...}): hot path.
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
 * @brief Fraction of the light of a point `distance` along `ray` that arrives:
 *        1 when unoccluded, attenuated by thin-walled transmissive surfaces, 0
 *        when anything else is before it (the light's own surface too: a
 *        closed mesh's far side behind its near side).
 */
auto shadowTransmittance(const RenderScene& scene, Ray ray, float distance, std::uint32_t mask)
    -> Vec3
{
    // Short of the point: not its own surface.
    float remaining = distance * (1.0F - kShadowShortening);
    Vec3 transmittance{1.0F};
    for (int layer = 0; layer < kMaxThinLayers; ++layer) {
        const auto hit = traceClosestHit(scene, ray, 0.0F, remaining, mask);
        if (!hit) {
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
        remaining -= hit->t;
    }
    return Vec3(0.0F);
}

/** @brief How bright a light is (luminance of its emission), to pick it by. */
auto lightBrightness(Vec3 emission) -> float
{
    return 0.2126F * emission.r + 0.7152F * emission.g + 0.0722F * emission.b;
}

/**
 * @brief How much a light may light a surface point, roughly: its brightness
 *        times the solid angle it covers. 0 if it cannot: below the surface's
 *        horizon, around the point (a sphere), or the surface itself.
 */
auto lightWeight(const RenderLight& light, Vec3 position, Vec3 normal,
                 std::uint32_t surfaceInstance) -> float
{
    if (light.instanceIndex == surfaceInstance) {
        return 0.0F;
    }
    const Vec3 toCenter = light.center - position;
    if (glm::dot(toCenter, normal) < -light.radius) {
        return 0.0F;
    }
    const float distance2 = glm::dot(toCenter, toCenter);
    const float brightness = lightBrightness(light.emission);
    if (light.triangleCount == 0) {
        if (distance2 <= light.radius * light.radius) {
            return 0.0F;
        }
        return brightness * kTwoPi * coneOneMinusCos(light.radius, distance2);
    }
    // A mesh: its area over the squared distance, at most a hemisphere.
    return brightness * std::min(light.area / std::max(distance2, 1e-12F), kTwoPi);
}

/** @brief Sum of the lights' weights at a surface point. */
auto totalLightWeight(const RenderScene& scene, Vec3 position, Vec3 normal,
                      std::uint32_t surfaceInstance) -> float
{
    float total = 0.0F;
    for (const RenderLight& light : scene.lights) {
        total += lightWeight(light, position, normal, surfaceInstance);
    }
    return total;
}

/**
 * @brief Pick a light for a surface point by the lights' weights, `u` in
 *        [0, 1); nullopt if none may light it. `probability`: of the pick.
 *        One pass (weighted reservoir sampling, `u` rescaled at each light).
 */
auto pickLight(const RenderScene& scene, Vec3 position, Vec3 normal,
               std::uint32_t surfaceInstance, float u, float& probability)
    -> std::optional<std::size_t>
{
    float total = 0.0F;
    float weight = 0.0F;
    std::optional<std::size_t> picked;
    for (std::size_t i = 0; i < scene.lights.size(); ++i) {
        const float w = lightWeight(scene.lights[i], position, normal, surfaceInstance);
        if (w <= 0.0F) {
            continue;
        }
        total += w;
        const float keep = w / total;
        if (u < keep) {
            picked = i;
            weight = w;
            u /= keep;
        } else {
            u = (u - keep) / (1.0F - keep);
        }
    }
    probability = total > 0.0F ? weight / total : 0.0F;
    return picked;
}

/** @brief Triangle `triangle` of a mesh light, in world space. */
auto lightTriangle(const RenderScene& scene, const RenderLight& light, std::uint32_t triangle)
    -> std::array<Vec3, 3>
{
    const RenderInstance& instance = scene.instances[light.instanceIndex];
    const MeshData& mesh = *scene.geometries[instance.geometryIndex].mesh;
    std::array<Vec3, 3> vertices;
    for (std::size_t k = 0; k < 3; ++k) {
        vertices[k] = Vec3(instance.objectToWorld * Vec4(mesh.vertex(triangle, k), 1.0F));
    }
    return vertices;
}

/**
 * @brief Solid-angle pdf, from `position`, of sampling `point` on triangle
 *        `triangle` of a mesh light (picked by area, the point uniformly).
 */
auto meshLightPdf(const RenderScene& scene, const RenderLight& light, std::uint32_t triangle,
                  Vec3 position, Vec3 point) -> float
{
    const std::array<Vec3, 3> v = lightTriangle(scene, light, triangle);
    const Vec3 normal = glm::cross(v[1] - v[0], v[2] - v[0]);
    const float doubleArea = glm::length(normal);
    const Vec3 toPoint = point - position;
    const float distance2 = glm::dot(toPoint, toPoint);
    if (doubleArea <= 0.0F || distance2 <= 0.0F) {
        return 0.0F;
    }
    const float cosine = std::abs(glm::dot(toPoint, normal)) / (doubleArea * std::sqrt(distance2));
    if (cosine <= 0.0F) {
        return 0.0F;
    }
    const float* cdf = &scene.lightTriangleCdf[light.firstTriangle];
    const float picked = cdf[triangle] - (triangle > 0 ? cdf[triangle - 1] : 0.0F);
    return picked * (2.0F / doubleArea) * distance2 / cosine;
}

/** @brief A direction to a light, the distance to its point there and the solid-angle pdf. */
struct LightSample
{
    Vec3 direction{0.0F};
    float distance = 0.0F;
    float pdf = 0.0F;
};

auto sampleLight(const RenderScene& scene, const RenderLight& light, Vec3 position, Rng& rng)
    -> std::optional<LightSample>
{
    if (light.triangleCount == 0) {
        const Vec3 toCenter = light.center - position;
        const float distance2 = glm::dot(toCenter, toCenter);
        const float u1 = rng.nextFloat();
        const float u2 = rng.nextFloat();
        if (distance2 <= light.radius * light.radius) {
            return std::nullopt;
        }
        const float oneMinusCos = coneOneMinusCos(light.radius, distance2);
        const Vec3 direction = sampleCone(toCenter / std::sqrt(distance2), oneMinusCos, u1, u2);
        // To the sphere's near surface that way (the cone's directions all meet it).
        const float along = glm::dot(toCenter, direction);
        const float offAxis2 = std::max(0.0F, distance2 - along * along);
        return LightSample{
            .direction = direction,
            .distance =
                along - std::sqrt(std::max(0.0F, light.radius * light.radius - offAxis2)),
            .pdf = conePdf(oneMinusCos)};
    }
    // The first triangle whose cumulative area exceeds u.
    const float u = rng.nextFloat();
    const float* cdf = &scene.lightTriangleCdf[light.firstTriangle];
    std::uint32_t low = 0;
    std::uint32_t high = light.triangleCount - 1;
    while (low < high) {
        const std::uint32_t middle = (low + high) / 2;
        if (u < cdf[middle]) {
            high = middle;
        } else {
            low = middle + 1;
        }
    }
    const float u1 = rng.nextFloat();
    const float u2 = rng.nextFloat();
    const std::array<Vec3, 3> v = lightTriangle(scene, light, low);
    const float s = std::sqrt(u1);
    const Vec3 point = v[0] * (1.0F - s) + v[1] * (s * (1.0F - u2)) + v[2] * (s * u2);
    const Vec3 toPoint = point - position;
    const float distance = glm::length(toPoint);
    const float pdf = meshLightPdf(scene, light, low, position, point);
    if (distance <= 0.0F || pdf <= 0.0F) {
        return std::nullopt;
    }
    return LightSample{.direction = toPoint / distance, .distance = distance, .pdf = pdf};
}

/**
 * @brief Solid-angle pdf of next event estimation at a surface point (picking
 *        light `index` and its point `lightHit`), for MIS against BSDF sampling.
 */
auto directLightPdf(const RenderScene& scene, std::size_t index, Vec3 position, Vec3 normal,
                    std::uint32_t surfaceInstance, const SurfaceHit& lightHit) -> float
{
    const RenderLight& light = scene.lights[index];
    const float weight = lightWeight(light, position, normal, surfaceInstance);
    if (weight <= 0.0F) {
        return 0.0F;
    }
    const float probability = weight / totalLightWeight(scene, position, normal, surfaceInstance);
    if (light.triangleCount == 0) {
        const Vec3 toCenter = light.center - position;
        return probability *
               conePdf(coneOneMinusCos(light.radius, glm::dot(toCenter, toCenter)));
    }
    return probability * meshLightPdf(scene, light, lightHit.primitive, position, lightHit.position);
}

/**
 * @brief Next event estimation for the diffuse lobe: radiance of one sampled
 *        light times cos / pi, MIS-weighted against cosine sampling (the
 *        albedo is applied by the caller).
 */
auto sampleDirectLight(const RenderScene& scene, const PathTracerSettings& settings,
                       const SurfaceHit& hit, Rng& rng) -> Vec3
{
    float probability = 0.0F;
    const auto index =
        pickLight(scene, hit.position, hit.normal, hit.instanceIndex, rng.nextFloat(), probability);
    if (!index) {
        return Vec3(0.0F);
    }
    const RenderLight& light = scene.lights[*index];
    const auto sample = sampleLight(scene, light, hit.position, rng);
    if (!sample) {
        return Vec3(0.0F);
    }
    const Vec3 direction = sample->direction;
    const float cosine = glm::dot(direction, hit.normal);
    if (cosine <= 0.0F || glm::dot(direction, hit.geometricNormal) <= 0.0F) {
        return Vec3(0.0F);
    }
    const Vec3 transmittance = shadowTransmittance(
        scene, Ray{offsetOrigin(hit.position, hit.geometricNormal, direction), direction},
        sample->distance, settings.instanceMask);
    if (maxComponent(transmittance) <= 0.0F) {
        return Vec3(0.0F);
    }
    const float lightPdf = probability * sample->pdf;
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
    // Probability that the sampled microfacet chose this lobe (reflection: metal or
    // dielectric).
    float probability = 1.0F;
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
    const bool thin = (material.flags & kMaterialThinWalled) != 0U;
    // n_incident / n_transmitted: entering the volume through its front face.
    const float eta = thin || hit.frontFace ? 1.0F / material.ior : material.ior;
    const float reflectance = fresnelDielectric(glm::dot(woLocal, h), eta);
    const float reflection = material.metallic + (1.0F - material.metallic) * reflectance;
    if (rng.nextFloat() < material.metallic) {
        const Vec3 wi = reflectLocal(h);
        const Vec3 world = frame.toWorld(wi);
        if (!aboveSurface(world)) {
            return std::nullopt;
        }
        return Scatter{world, fresnelSchlick(baseColor, glm::dot(woLocal, h)), Lobe::Specular,
                       false, reflection};
    }

    if (rng.nextFloat() < reflectance) {
        const Vec3 world = frame.toWorld(reflectLocal(h));
        if (!aboveSurface(world)) {
            return std::nullopt;
        }
        return Scatter{world, Vec3(1.0F), Lobe::Specular, false, reflection};
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

/** @brief The reflection lobe at a direction: BSDF times cosine, and sampling's density. */
struct Reflection
{
    Vec3 value{0.0F};
    float pdf = 0.0F;
};

/**
 * @brief The reflection lobe as sampleBsdf samples it, for a direction `wi`:
 *        metal and dielectric reflection about a visible microfacet normal,
 *        directions below the surface folded up -- so `wi` comes from two
 *        microfacets, the one mirroring `wo` to it and, if `wi` is lower than
 *        `wo`, the one mirroring `wo` below it. `pdf`: the density with which
 *        sampleBsdf returns `wi` from the reflection lobe (of all lobes).
 */
auto evaluateReflection(const RenderMaterial& material, Vec3 wo, const SurfaceHit& hit, Vec3 wi)
    -> Reflection
{
    const Frame frame = makeFrame(hit.normal);
    Vec3 woLocal = frame.toLocal(wo);
    woLocal.z = std::max(woLocal.z, 1e-5F);
    woLocal = glm::normalize(woLocal);
    const Vec3 wiLocal = frame.toLocal(wi);
    Reflection result;
    if (wiLocal.z <= 0.0F) {
        return result;
    }
    const float alpha = roughnessToAlpha(material.roughness);
    const bool thin = (material.flags & kMaterialThinWalled) != 0U;
    const float eta = thin || hit.frontFace ? 1.0F / material.ior : material.ior;
    const float metal = material.metallic;
    for (int folded = 0; folded < 2; ++folded) {
        if (folded == 1 && wiLocal.z >= woLocal.z) {
            break;  // The microfacet would face down.
        }
        const Vec3 h =
            glm::normalize(woLocal + Vec3(wiLocal.x, wiLocal.y, folded == 1 ? -wiLocal.z : wiLocal.z));
        const float density = ggxVisibleNormalPdf(woLocal, h, alpha);
        if (density <= 0.0F) {
            continue;
        }
        const float cosine = glm::dot(woLocal, h);
        const float perDirection = density / (4.0F * cosine);  // Mirroring's Jacobian.
        const float reflectance = fresnelDielectric(cosine, eta);
        result.pdf += (metal + (1.0F - metal) * reflectance) * perDirection;
        result.value += (metal * fresnelSchlick(Vec3(material.albedo), cosine) +
                         Vec3((1.0F - metal) * reflectance)) *
                        perDirection;
    }
    return result;
}

/**
 * @brief Next event estimation for the reflection lobe: radiance of one
 *        sampled light times the lobe (BSDF times cosine), MIS-weighted
 *        against sampling the lobe.
 */
auto sampleReflectedLight(const RenderScene& scene, const PathTracerSettings& settings,
                          const RenderMaterial& material, Vec3 wo, const SurfaceHit& hit, Rng& rng)
    -> Vec3
{
    float probability = 0.0F;
    const auto index =
        pickLight(scene, hit.position, hit.normal, hit.instanceIndex, rng.nextFloat(), probability);
    if (!index) {
        return Vec3(0.0F);
    }
    const RenderLight& light = scene.lights[*index];
    const auto sample = sampleLight(scene, light, hit.position, rng);
    if (!sample || glm::dot(sample->direction, hit.geometricNormal) <= 0.0F) {
        return Vec3(0.0F);
    }
    const Reflection reflection = evaluateReflection(material, wo, hit, sample->direction);
    if (maxComponent(reflection.value) <= 0.0F) {
        return Vec3(0.0F);
    }
    const Vec3 transmittance = shadowTransmittance(
        scene,
        Ray{offsetOrigin(hit.position, hit.geometricNormal, sample->direction), sample->direction},
        sample->distance, settings.instanceMask);
    if (maxComponent(transmittance) <= 0.0F) {
        return Vec3(0.0F);
    }
    const float lightPdf = probability * sample->pdf;
    return light.emission * transmittance * reflection.value *
           (powerHeuristic(lightPdf, reflection.pdf) / lightPdf);
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
    // The last vertex that also sampled the lights (diffuse or glossy), to MIS-weight
    // lights found by BSDF sampling from it: where it was and the density of the
    // direction sampled there (diffuse: given the lobe; glossy: of all lobes).
    bool mis = false;
    Vec3 misPosition{0.0F};
    Vec3 misNormal{0.0F};
    std::uint32_t misInstance = 0;
    float misPdf = 0.0F;

    for (std::uint32_t bounce = 0; bounce < settings.maxBounces; ++bounce) {
        const auto hit = traceClosestHit(scene, ray, 0.0F, kInfinity, settings.instanceMask);
        if (!hit.has_value()) {
            add(throughput * skyRadiance(scene, ray.direction), scatterings);
            break;
        }
        const RenderMaterial& material =
            scene.materials[scene.instances[hit->instanceIndex].materialIndex];

        const Vec3 emission(material.emission);
        if (maxComponent(emission) > 0.0F) {
            float weight = 1.0F;
            if (nextEvent && mis) {
                if (const std::int32_t light = scene.lightOfInstance[hit->instanceIndex];
                    light >= 0) {
                    weight = powerHeuristic(
                        misPdf, directLightPdf(scene, static_cast<std::size_t>(light),
                                               misPosition, misNormal, misInstance, *hit));
                }
            }
            add(throughput * emission * weight, scatterings);
        }

        const Vec3 wo = -glm::normalize(ray.direction);
        const auto scatter = sampleBsdf(material, wo, *hit, rng);
        if (!scatter.has_value()) {
            break;  // Absorbed.
        }
        // The lights for the lobe sampled: diffuse, or glossy reflection (not near
        // mirrors: their lights are better found by sampling the lobe).
        const bool glossy =
            scatter->lobe == Lobe::Specular && material.roughness >= kGlossyLightRoughness;
        if (scatter->lobe == Lobe::Diffuse && nextEvent) {
            add(throughput * Vec3(material.albedo) * sampleDirectLight(scene, settings, *hit, rng),
                scatterings + 1);
        } else if (glossy && nextEvent) {
            // Done when the microfacet picked reflection: weighted up by that chance.
            add(throughput * sampleReflectedLight(scene, settings, material, wo, *hit, rng) /
                    scatter->probability,
                scatterings + 1);
        }
        throughput *= scatter->weight;
        if (!scatter->passThrough) {
            ++scatterings;
        }
        if (scatter->lobe == Lobe::Diffuse || glossy) {
            mis = true;
            misPosition = hit->position;
            misNormal = hit->normal;
            misInstance = hit->instanceIndex;
            misPdf = glossy ? evaluateReflection(material, wo, *hit, scatter->direction).pdf
                            : std::max(0.0F, glm::dot(scatter->direction, hit->normal)) / kPi;
        } else if (!scatter->passThrough) {
            mis = false;
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
    const Vec3 ambient = skyAmbient(scene);
    for (int layer = 0; layer < kMaxThinLayers; ++layer) {
        const auto hit = traceClosestHit(scene, ray, 0.0F, kInfinity, settings.instanceMask);
        if (!hit.has_value()) {
            return result + throughput * skyRadiance(scene, ray.direction);
        }
        const RenderMaterial& material =
            scene.materials[scene.instances[hit->instanceIndex].materialIndex];

        // Diffuse response to: the sky (ambient), a headlight and the sphere
        // lights (exact irradiance of a sphere above the horizon, shadowed).
        Vec3 irradiance = ambient + Vec3(kAmbient) +
                          Vec3(kHeadlight * std::max(0.0F, -glm::dot(hit->normal, ray.direction)));
        for (const RenderLight& light : scene.lights) {
            if (light.triangleCount > 0) {
                continue;  // Meshes: their own emission only.
            }
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
                              std::sqrt(distance2) - light.radius, settings.instanceMask);
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
           scene.version != m_lastSceneVersion || camera.view() != m_lastView ||
           camera.projection() != m_lastProjection || settings != m_lastSettings;
}

void CpuPathTracer::render(const RenderScene& scene, const Camera& camera,
                           const PathTracerSettings& settings, const RenderControl& control)
{
    const std::uint32_t width = m_image.width();
    const std::uint32_t height = m_image.height();
    if (width == 0 || height == 0) {
        return;
    }
    if (needsReset(scene, camera, settings)) {
        m_frameIndex = 0;
        m_nextTile = 0;
        m_passStride = std::clamp(control.firstPassStride, 1U, kTileSize);
        m_idStride = 0;
        m_accumulation.fill(Vec4(0.0F));
    }
    m_lastSceneVersion = scene.version;
    m_lastView = camera.view();
    m_lastProjection = camera.projection();
    m_lastSettings = settings;
    if (!settings.autoExposure) {
        m_adaptedExposure = 0.0F;
    }
    m_displayExposure = settings.autoExposure && m_adaptedExposure > 0.0F
                            ? m_adaptedExposure * settings.exposure
                            : settings.exposure;

    const std::uint32_t tileCount = ((width + kTileSize - 1) / kTileSize) *
                                    ((height + kTileSize - 1) / kTileSize);
    const std::uint32_t batch = std::max(1U, m_executor.threadCount() * 2U);
    const float budget = control.timeBudgetMs;
    const auto start = std::chrono::steady_clock::now();
    auto timeLeft = [&] {
        return budget > 0.0F &&
               std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - start)
                       .count() < budget;
    };
    do {
        // Without a budget, and for the first pass after a reset (no stale
        // tiles from an old view), the whole pass is rendered.
        const std::uint32_t remaining = tileCount - m_nextTile;
        const std::uint32_t count =
            budget <= 0.0F || m_frameIndex == 0 ? remaining : std::min(remaining, batch);
        renderTiles(scene, camera, settings, m_nextTile, count);
        m_nextTile += count;
        if (m_nextTile == tileCount) {
            m_nextTile = 0;
            ++m_frameIndex;
            if (m_idStride == 0 || m_passStride < m_idStride) {
                m_idStride = m_passStride;
            }
            m_passStride = std::max(1U, m_passStride / 2U);
            if (budget <= 0.0F) {
                break;  // One pass per call.
            }
        }
    } while (timeLeft());
    if (settings.autoExposure) {
        updateExposure(settings);
    }
}

auto CpuPathTracer::displayPixel(Vec4 accumulated, const PathTracerSettings& settings) const
    -> std::uint32_t
{
    Vec3 display = Vec3(accumulated) / accumulated.a;
    display = toneMap(display * m_displayExposure, settings.toneMapper);
    if (settings.srgbOutput) {
        display = linearToSrgb(display);
    }
    return packRgba8(Vec4(display, 1.0F));
}

void CpuPathTracer::updateExposure(const PathTracerSettings& settings)
{
    const std::uint32_t width = m_accumulation.width();
    const std::uint32_t height = m_accumulation.height();
    // The accumulated image's histogram (exposure.hpp), row by row.
    std::vector<std::array<std::uint32_t, kExposureBins>> rows(height);
    m_executor.parallelFor(height, [&](std::size_t y) {
        auto& histogram = rows[y];
        histogram.fill(0U);
        for (std::uint32_t x = 0; x < width; ++x) {
            const Vec4 accumulated = m_accumulation.at(x, static_cast<std::uint32_t>(y));
            if (accumulated.a > 0.0F) {
                const Vec3 color = Vec3(accumulated) / accumulated.a;
                ++histogram[exposureBin(std::max({color.r, color.g, color.b}))];
            }
        }
    });
    std::array<std::uint32_t, kExposureBins> histogram{};
    for (const auto& row : rows) {
        for (std::uint32_t bin = 0; bin < kExposureBins; ++bin) {
            histogram[bin] += row[bin];
        }
    }
    const float metered = meteredExposure(histogram);
    const auto now = std::chrono::steady_clock::now();
    const float seconds = std::clamp(
        std::chrono::duration<float>(now - m_lastExposureTime).count(), 0.001F, 0.1F);
    m_lastExposureTime = now;
    m_adaptedExposure = m_adaptedExposure > 0.0F
                            ? adaptExposure(m_adaptedExposure, metered, seconds)
                            : metered;  // The first frame: no adapting from nothing.
    const float exposure = m_adaptedExposure * settings.exposure;
    if (exposure == m_displayExposure) {
        return;
    }
    // The whole image at the new exposure.
    m_displayExposure = exposure;
    m_executor.parallelFor(height, [&](std::size_t y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            const Vec4 accumulated = m_accumulation.at(x, static_cast<std::uint32_t>(y));
            if (accumulated.a > 0.0F) {
                m_image.at(x, static_cast<std::uint32_t>(y)) = displayPixel(accumulated, settings);
            }
        }
    });
}

void CpuPathTracer::renderTiles(const RenderScene& scene, const Camera& camera,
                                const PathTracerSettings& settings, std::uint32_t firstTile,
                                std::uint32_t tileCount)
{
    const std::uint32_t width = m_image.width();
    const std::uint32_t height = m_image.height();
    const std::uint32_t tilesX = (width + kTileSize - 1) / kTileSize;
    const std::uint32_t frame = m_frameIndex;
    const std::uint32_t stride = m_passStride;
    // Ids are written by the first pass and again once the resolution is finer.
    const bool writeIds = m_idStride == 0 || stride < m_idStride;
    const bool preview = settings.mode == RenderMode::Preview;
    const std::uint32_t samples = preview ? 1U : std::max(1U, settings.samplesPerPixel);

    m_executor.parallelFor(tileCount, [&](std::size_t index) {
        const auto tile = static_cast<std::uint32_t>(firstTile + index);
        const std::uint32_t x0 = (tile % tilesX) * kTileSize;
        const std::uint32_t y0 = (tile / tilesX) * kTileSize;
        const std::uint32_t x1 = std::min(width, x0 + kTileSize);
        const std::uint32_t y1 = std::min(height, y0 + kTileSize);

        // One sample per stride x stride block, written to all its pixels.
        for (std::uint32_t by = y0; by < y1; by += stride) {
            for (std::uint32_t bx = x0; bx < x1; bx += stride) {
                const std::uint32_t bw = std::min(stride, x1 - bx);
                const std::uint32_t bh = std::min(stride, y1 - by);
                Rng rng = Rng::forPixel(bx, by, frame, settings.seed);
                Vec3 color{0.0F};
                std::uint32_t entityId = kNoEntityId;
                for (std::uint32_t s = 0; s < samples; ++s) {
                    // Jittered position in the block: anti-aliasing for free.
                    const Vec2 pixel{static_cast<float>(bx) + rng.nextFloat() * static_cast<float>(bw),
                                     static_cast<float>(by) + rng.nextFloat() * static_cast<float>(bh)};
                    std::uint32_t* idOut = s == 0 && writeIds ? &entityId : nullptr;
                    const Ray ray = camera.generateRay(pixel);
                    color += preview ? shadePreview(scene, ray, settings, idOut)
                                     : tracePath(scene, ray, settings, rng, idOut);
                }
                color /= static_cast<float>(samples);

                for (std::uint32_t y = by; y < by + bh; ++y) {
                    for (std::uint32_t x = bx; x < bx + bw; ++x) {
                        Vec4& accumulated = m_accumulation.at(x, y);
                        accumulated += Vec4(color, 1.0F);
                        m_image.at(x, y) = displayPixel(accumulated, settings);
                        if (writeIds) {
                            m_entityIds.at(x, y) = entityId;
                        }
                    }
                }
            }
        }
    });
}
}  // namespace starcore
