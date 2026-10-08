// The path tracer's estimator, shared by the GPU renderers: port of tracePath /
// shadePreview (src/render/path_tracer.cpp) -- same estimator, same random
// streams. Keep them in sync.
//
// The including shader defines the parameters below.

#ifndef STARCORE_PATH_GLSL
#define STARCORE_PATH_GLSL

#include "scene.glsl"

uint instanceMask();
uint lightCount();
vec3 skyColor();
uint maxBounces();
bool nextEventEnabled();
float fireflyClamp();  // 0 = off.

const uint kRussianRouletteStart = 3u;
// Thin surfaces a shadow ray (or a preview ray) may pass through.
const int kMaxThinLayers = 8;

const uint kLobeDiffuse = 0u;
const uint kLobeSpecular = 1u;
const uint kLobeTransmission = 2u;

/** Ray origin just off the surface, on the side `direction` leaves to. */
vec3 offsetOrigin(vec3 position, vec3 geometricNormal, vec3 direction)
{
    float scale = 1e-4 + 2e-6 * maxComponent(abs(position));
    return position +
           geometricNormal * (dot(direction, geometricNormal) >= 0.0 ? scale : -scale);
}

/** Entity of the first surface along a ray that is not see-through. */
uint firstOpaqueEntity(vec3 origin, vec3 direction)
{
    for (int layer = 0; layer < kMaxThinLayers; ++layer) {
        SurfaceHit hit;
        if (!traceClosestHit(origin, direction, instanceMask(), hit)) {
            break;
        }
        Instance instance = instances[hit.instanceIndex];
        if (!isSeeThrough(materials[instance.materialIndex])) {
            return instance.entityId;
        }
        origin = offsetOrigin(hit.position, hit.geometricNormal, direction);
    }
    return kNoEntityId;
}

/** Expected straight-through transmission of a thin-walled surface. */
vec3 thinTransmittance(Material m, float cosTheta)
{
    float reflectance = fresnelDielectric(cosTheta, 1.0 / m.ior);
    return m.albedo.rgb * ((1.0 - m.metallic) * m.transmission * (1.0 - reflectance));
}

/** Light from `lightInstance` arriving along the ray: attenuated by thin panes, 0 if blocked. */
vec3 shadowTransmittance(vec3 origin, vec3 direction, uint lightInstance)
{
    vec3 transmittance = vec3(1.0);
    for (int layer = 0; layer < kMaxThinLayers; ++layer) {
        SurfaceHit hit;
        if (!traceClosestHit(origin, direction, instanceMask(), hit)) {
            return vec3(0.0);
        }
        if (hit.instanceIndex == lightInstance) {
            return transmittance;
        }
        Material material = materialOf(hit.instanceIndex);
        if (!isThinTransmissive(material)) {
            return vec3(0.0);
        }
        transmittance *= thinTransmittance(material, abs(dot(direction, hit.geometricNormal)));
        if (maxComponent(transmittance) <= 0.0) {
            return vec3(0.0);
        }
        origin = offsetOrigin(hit.position, hit.geometricNormal, direction);
    }
    return vec3(0.0);
}

/** Solid-angle pdf of sampling light `index` from `position` (0 from inside it). */
float directLightPdf(uint index, vec3 position)
{
    Light light = lights[index];
    vec3 toCenter = light.center - position;
    float distance2 = dot(toCenter, toCenter);
    if (distance2 <= light.radius * light.radius) {
        return 0.0;
    }
    return conePdf(coneOneMinusCos(light.radius, distance2)) / float(lightCount());
}

/** Next event estimation for the diffuse lobe (its weight applied by the caller). */
vec3 sampleDirectLight(SurfaceHit hit, inout uint rng)
{
    uint count = lightCount();
    uint index = min(count - 1u, uint(nextFloat(rng) * float(count)));
    Light light = lights[index];
    vec3 toCenter = light.center - hit.position;
    float distance2 = dot(toCenter, toCenter);
    float u1 = nextFloat(rng);
    float u2 = nextFloat(rng);
    if (distance2 <= light.radius * light.radius) {
        return vec3(0.0);
    }
    float oneMinusCos = coneOneMinusCos(light.radius, distance2);
    vec3 direction = sampleCone(toCenter / sqrt(distance2), oneMinusCos, u1, u2);
    float cosine = dot(direction, hit.normal);
    if (cosine <= 0.0 || dot(direction, hit.geometricNormal) <= 0.0) {
        return vec3(0.0);
    }
    vec3 transmittance = shadowTransmittance(
        offsetOrigin(hit.position, hit.geometricNormal, direction), direction,
        light.instanceIndex);
    if (maxComponent(transmittance) <= 0.0) {
        return vec3(0.0);
    }
    float lightPdf = conePdf(oneMinusCos) / float(count);
    float bsdfPdf = cosine / kPi;
    return light.emission * transmittance * (cosine / kPi) *
           (powerHeuristic(lightPdf, bsdfPdf) / lightPdf);
}

struct Scatter
{
    vec3 direction;
    vec3 weight;  // BSDF * cos / (pdf * lobe probability).
    uint lobe;
    // Straight through a thin sheet: the path keeps its MIS state.
    bool passThrough;
    // Probability of the sampled set of lobes (sampleReflection), else 1.
    float scale;
};

/** Mirror wo about h, folding multiply-scattered light back above the surface. */
vec3 reflectLocal(vec3 wo, vec3 h)
{
    vec3 wi = 2.0 * dot(wo, h) * h - wo;
    wi.z = abs(wi.z);
    return wi;
}

/** See sampleBsdf in path_tracer.cpp. Returns false when the path is absorbed. */
bool sampleBsdf(Material material, vec3 wo, SurfaceHit hit, inout uint rng, out Scatter scatter)
{
    scatter.direction = vec3(0.0);
    scatter.weight = vec3(0.0);
    scatter.lobe = kLobeDiffuse;
    scatter.passThrough = false;
    scatter.scale = 1.0;

    vec3 baseColor = material.albedo.rgb;
    Frame frame = makeFrame(hit.normal);
    vec3 woLocal = toLocal(frame, wo);
    woLocal.z = max(woLocal.z, 1e-5);
    woLocal = normalize(woLocal);
    float alpha = roughnessToAlpha(material.roughness);

    float u1 = nextFloat(rng);
    float u2 = nextFloat(rng);
    vec3 h = sampleGgxVisibleNormal(woLocal, alpha, u1, u2);
    if (nextFloat(rng) < material.metallic) {
        vec3 world = toWorld(frame, reflectLocal(woLocal, h));
        if (dot(world, hit.geometricNormal) <= 0.0) {
            return false;
        }
        scatter.direction = world;
        scatter.weight = fresnelSchlick(baseColor, dot(woLocal, h));
        scatter.lobe = kLobeSpecular;
        return true;
    }

    bool thin = (material.flags & kMaterialThinWalled) != 0u;
    // n_incident / n_transmitted: entering the volume through its front face.
    float eta = thin || hit.frontFace ? 1.0 / material.ior : material.ior;
    float reflectance = fresnelDielectric(dot(woLocal, h), eta);
    if (nextFloat(rng) < reflectance) {
        vec3 world = toWorld(frame, reflectLocal(woLocal, h));
        if (dot(world, hit.geometricNormal) <= 0.0) {
            return false;
        }
        scatter.direction = world;
        scatter.weight = vec3(1.0);
        scatter.lobe = kLobeSpecular;
        return true;
    }

    if (nextFloat(rng) < material.transmission) {
        vec3 transmitted;
        if (thin) {
            // A sheet: refracting in and out cancels; mirror the reflection.
            transmitted = reflectLocal(woLocal, h);
        } else if (!refractDirection(-woLocal, h, eta, transmitted)) {
            return false;  // Unreachable: the Fresnel term was 1.
        }
        transmitted.z = -abs(transmitted.z);  // Always through the surface.
        vec3 world = normalize(toWorld(frame, transmitted));
        if (dot(world, hit.geometricNormal) > 0.0) {
            return false;
        }
        scatter.direction = world;
        scatter.weight = baseColor;
        scatter.lobe = kLobeTransmission;
        scatter.passThrough = thin;
        return true;
    }

    vec3 direction = cosineHemisphere(hit.normal, rng);
    if (dot(direction, hit.geometricNormal) <= 0.0) {
        return false;
    }
    scatter.direction = direction;
    scatter.weight = baseColor;
    scatter.lobe = kLobeDiffuse;
    return true;
}

/**
 * sampleBsdf of a thin sheet without the straight pass through it (which the
 * hybrid renderer follows separately): one of the other lobes, chosen by
 * their probabilities relative to each other and weighted by their sum.
 */
bool sampleReflection(Material material, vec3 wo, SurfaceHit hit, inout uint rng,
                      out Scatter scatter)
{
    scatter.direction = vec3(0.0);
    scatter.weight = vec3(0.0);
    scatter.lobe = kLobeDiffuse;
    scatter.passThrough = false;
    scatter.scale = 1.0;

    vec3 baseColor = material.albedo.rgb;
    Frame frame = makeFrame(hit.normal);
    vec3 woLocal = toLocal(frame, wo);
    woLocal.z = max(woLocal.z, 1e-5);
    woLocal = normalize(woLocal);
    float alpha = roughnessToAlpha(material.roughness);

    float u1 = nextFloat(rng);
    float u2 = nextFloat(rng);
    vec3 h = sampleGgxVisibleNormal(woLocal, alpha, u1, u2);
    float reflectance = fresnelDielectric(dot(woLocal, h), 1.0 / material.ior);
    float metal = material.metallic;
    float dielectric = (1.0 - metal) * reflectance;
    float diffuse = (1.0 - metal) * (1.0 - reflectance) * (1.0 - material.transmission);
    float total = metal + dielectric + diffuse;
    if (total <= 0.0) {
        return false;
    }
    scatter.scale = total;

    float choice = nextFloat(rng) * total;
    if (choice < metal + dielectric) {
        vec3 world = toWorld(frame, reflectLocal(woLocal, h));
        if (dot(world, hit.geometricNormal) <= 0.0) {
            return false;
        }
        scatter.direction = world;
        scatter.weight = choice < metal ? fresnelSchlick(baseColor, dot(woLocal, h)) * total
                                        : vec3(total);
        scatter.lobe = kLobeSpecular;
        return true;
    }
    vec3 direction = cosineHemisphere(hit.normal, rng);
    if (dot(direction, hit.geometricNormal) <= 0.0) {
        return false;
    }
    scatter.direction = direction;
    scatter.weight = baseColor * total;
    scatter.lobe = kLobeDiffuse;
    return true;
}

/**
 * State of a camera path. `scale` is the throughput the path started with
 * (paths split at window panes start below 1): Russian roulette and the
 * firefly clamp work relative to it.
 */
struct PathState
{
    vec3 radiance;
    vec3 throughput;
    // Scattering events so far (straight passes through window panes don't count).
    uint scatterings;
    // Last diffuse vertex, to MIS-weight lights found by BSDF sampling.
    bool fromDiffuse;
    vec3 diffusePosition;
    float diffusePdf;
    float scale;
};

PathState newPath()
{
    PathState path;
    path.radiance = vec3(0.0);
    path.throughput = vec3(1.0);
    path.scatterings = 0u;
    path.fromDiffuse = false;
    path.diffusePosition = vec3(0.0);
    path.diffusePdf = 0.0;
    path.scale = 1.0;
    return path;
}

/** Indirect light (two or more scattering events) is optionally clamped against fireflies. */
vec3 clampContribution(vec3 contribution, uint scatteringEvents, float scale)
{
    float limit = fireflyClamp();
    if (limit > 0.0 && scatteringEvents > 1u) {
        float peak = maxComponent(contribution) / scale;
        if (peak > limit) {
            contribution *= limit / peak;
        }
    }
    return contribution;
}

// pathVertex flags.
const uint kVertexSkipEmission = 1u;  // The caller accounts for the vertex's emission.
const uint kVertexReflectOnly = 2u;   // sampleReflection instead of sampleBsdf.

/**
 * One vertex of a path reached along `direction`: emission (MIS-weighted),
 * BSDF sampling, next event estimation and Russian roulette. Returns false
 * when the path ends, else the next ray.
 */
bool pathVertex(SurfaceHit hit, Material material, int lightIndex, vec3 direction, uint bounce,
                uint flags, inout PathState path, inout uint rng, out vec3 nextOrigin,
                out vec3 nextDirection)
{
    nextOrigin = vec3(0.0);
    nextDirection = vec3(0.0);
    bool nextEvent = nextEventEnabled() && lightCount() > 0u;

    vec3 emission = material.emission.rgb;
    if ((flags & kVertexSkipEmission) == 0u && maxComponent(emission) > 0.0) {
        float weight = 1.0;
        if (nextEvent && path.fromDiffuse && lightIndex >= 0) {
            weight = powerHeuristic(path.diffusePdf,
                                    directLightPdf(uint(lightIndex), path.diffusePosition));
        }
        path.radiance +=
            clampContribution(path.throughput * emission * weight, path.scatterings, path.scale);
    }

    vec3 wo = -normalize(direction);
    Scatter scatter;
    bool scattered = (flags & kVertexReflectOnly) != 0u
                         ? sampleReflection(material, wo, hit, rng, scatter)
                         : sampleBsdf(material, wo, hit, rng, scatter);
    if (!scattered) {
        return false;  // Absorbed.
    }
    if (scatter.lobe == kLobeDiffuse && nextEvent) {
        path.radiance += clampContribution(
            path.throughput * scatter.weight * sampleDirectLight(hit, rng),
            path.scatterings + 1u, path.scale);
    }
    path.throughput *= scatter.weight;
    path.scale *= scatter.scale;
    if (!scatter.passThrough) {
        ++path.scatterings;
    }
    if (scatter.lobe == kLobeDiffuse) {
        path.fromDiffuse = true;
        path.diffusePosition = hit.position;
        path.diffusePdf = max(0.0, dot(scatter.direction, hit.normal)) / kPi;
    } else if (!scatter.passThrough) {
        path.fromDiffuse = false;
    }

    if (bounce >= kRussianRouletteStart) {
        float survival = min(1.0, maxComponent(path.throughput) / path.scale);
        if (nextFloat(rng) >= survival) {
            return false;
        }
        path.throughput /= survival;
    }
    nextOrigin = offsetOrigin(hit.position, hit.geometricNormal, scatter.direction);
    nextDirection = scatter.direction;
    return true;
}

/**
 * Continue a path with a ray; `bounce` is the index of the vertex it finds,
 * `end` the index the path stops before.
 */
void continuePath(vec3 origin, vec3 direction, uint bounce, uint end, inout PathState path,
                  inout uint rng)
{
    for (; bounce < end; ++bounce) {
        SurfaceHit hit;
        if (!traceClosestHit(origin, direction, instanceMask(), hit)) {
            path.radiance +=
                clampContribution(path.throughput * skyColor(), path.scatterings, path.scale);
            return;
        }
        Instance instance = instances[hit.instanceIndex];
        if (!pathVertex(hit, materials[instance.materialIndex], instance.lightIndex, direction,
                        bounce, 0u, path, rng, origin, direction)) {
            return;
        }
    }
}

/** Radiance along a camera ray. */
vec3 tracePath(vec3 origin, vec3 direction, inout uint rng)
{
    PathState path = newPath();
    continuePath(origin, direction, 0u, maxBounces(), path, rng);
    return path.radiance;
}

/** RenderMode::Preview: direct light + ambient + headlight, deterministic. */
vec3 shadePreview(vec3 origin, vec3 direction)
{
    vec3 sky = skyColor();
    vec3 result = vec3(0.0);
    vec3 throughput = vec3(1.0);
    for (int layer = 0; layer < kMaxThinLayers; ++layer) {
        SurfaceHit hit;
        if (!traceClosestHit(origin, direction, instanceMask(), hit)) {
            return result + throughput * sky;
        }
        Material material = materialOf(hit.instanceIndex);

        vec3 irradiance = sky + vec3(kPreviewAmbient) +
                          vec3(kPreviewHeadlight * max(0.0, -dot(hit.normal, direction)));
        for (uint i = 0u; i < lightCount(); ++i) {
            Light light = lights[i];
            vec3 toCenter = light.center - hit.position;
            float distance2 = dot(toCenter, toCenter);
            if (distance2 <= light.radius * light.radius) {
                continue;
            }
            vec3 toLight = toCenter / sqrt(distance2);
            float cosine = dot(toLight, hit.normal);
            if (cosine <= 0.0) {
                continue;
            }
            irradiance += light.emission * (cosine * light.radius * light.radius / distance2) *
                          shadowTransmittance(
                              offsetOrigin(hit.position, hit.geometricNormal, toLight), toLight,
                              light.instanceIndex);
        }

        float transmitted = isThinTransmissive(material)
                                ? (1.0 - material.metallic) * material.transmission
                                : 0.0;
        result += throughput * (material.emission.rgb +
                                material.albedo.rgb * irradiance * (1.0 - transmitted));
        if (transmitted <= 0.0) {
            return result;
        }
        throughput *= material.albedo.rgb * transmitted;
        origin = offsetOrigin(hit.position, hit.geometricNormal, direction);
    }
    return result;
}

#endif  // STARCORE_PATH_GLSL
