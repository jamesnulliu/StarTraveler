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

// Shadow rays to a point of a mesh light stop this fraction of the way short of it.
const float kShadowShortening = 1e-3;

/**
 * Light from `lightInstance` arriving along the ray: attenuated by thin panes,
 * 0 if blocked. `distance`: to the point of a mesh light (anything before it
 * blocks it, the mesh too); kFar for a sphere light, which the ray must hit.
 */
vec3 shadowTransmittance(vec3 origin, vec3 direction, uint lightInstance, float distance)
{
    bool toPoint = distance < kFar;
    // Short of the point: not its own triangle.
    float remaining = toPoint ? distance * (1.0 - kShadowShortening) : kFar;
    vec3 transmittance = vec3(1.0);
    for (int layer = 0; layer < kMaxThinLayers; ++layer) {
        SurfaceHit hit;
        if (!traceClosestHit(origin, direction, instanceMask(), remaining, hit)) {
            return toPoint ? transmittance : vec3(0.0);
        }
        if (!toPoint && hit.instanceIndex == lightInstance) {
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
        remaining -= hit.t;
    }
    return vec3(0.0);
}

/** How bright a light is (luminance of its emission), to pick it by. */
float lightBrightness(vec3 emission)
{
    return 0.2126 * emission.r + 0.7152 * emission.g + 0.0722 * emission.b;
}

/**
 * How much light `index` may light a surface point, roughly: its brightness
 * times the solid angle it covers. 0 if it cannot: below the surface's
 * horizon, around the point (a sphere), or the surface itself. (Reads only
 * the fields it needs: it runs for every light.)
 */
float lightWeight(uint index, vec3 position, vec3 normal, uint surfaceInstance)
{
    if (lights[index].instanceIndex == surfaceInstance) {
        return 0.0;
    }
    vec3 toCenter = lights[index].center - position;
    float radius = lights[index].radius;
    if (dot(toCenter, normal) < -radius) {
        return 0.0;
    }
    float distance2 = dot(toCenter, toCenter);
    float brightness = lightBrightness(lights[index].emission);
    if (lights[index].triangleCount == 0u) {
        if (distance2 <= radius * radius) {
            return 0.0;
        }
        return brightness * kTwoPi * coneOneMinusCos(radius, distance2);
    }
    // A mesh: its area over the squared distance, at most a hemisphere.
    return brightness * min(lights[index].area / max(distance2, 1e-12), kTwoPi);
}

/** Sum of the lights' weights at a surface point. */
float totalLightWeight(vec3 position, vec3 normal, uint surfaceInstance)
{
    float total = 0.0;
    for (uint i = 0u; i < lightCount(); ++i) {
        total += lightWeight(i, position, normal, surfaceInstance);
    }
    return total;
}

/**
 * Pick a light for a surface point by the lights' weights, `u` in [0, 1);
 * false if none may light it. `probability`: of the pick. One pass (weighted
 * reservoir sampling, `u` rescaled at each light).
 */
bool pickLight(vec3 position, vec3 normal, uint surfaceInstance, float u, out uint picked,
               out float probability)
{
    picked = 0u;
    probability = 0.0;
    float total = 0.0;
    float weight = 0.0;
    for (uint i = 0u; i < lightCount(); ++i) {
        float w = lightWeight(i, position, normal, surfaceInstance);
        if (w <= 0.0) {
            continue;
        }
        total += w;
        float keep = w / total;
        if (u < keep) {
            picked = i;
            weight = w;
            u /= keep;
        } else {
            u = (u - keep) / (1.0 - keep);
        }
    }
    if (total <= 0.0) {
        return false;
    }
    probability = weight / total;
    return true;
}

/** Vertex `corner` of triangle `triangle` of a mesh light, in world space. */
vec3 lightVertex(Light light, Instance instance, uvec3 indices, int corner)
{
    return transformPoint(light.objectToWorld, loadVec3(instance.positions, indices[corner]));
}

/**
 * Solid-angle pdf, from `position`, of sampling `point` on triangle `triangle`
 * of a mesh light (picked by area, the point uniformly).
 */
float meshLightPdf(Light light, uint triangle, vec3 position, vec3 point)
{
    Instance instance = instances[light.instanceIndex];
    uvec3 indices = triangleIndices(instance, triangle);
    vec3 v0 = lightVertex(light, instance, indices, 0);
    vec3 normal = cross(lightVertex(light, instance, indices, 1) - v0,
                        lightVertex(light, instance, indices, 2) - v0);
    float doubleArea = length(normal);
    vec3 toPoint = point - position;
    float distance2 = dot(toPoint, toPoint);
    if (doubleArea <= 0.0 || distance2 <= 0.0) {
        return 0.0;
    }
    float cosine = abs(dot(toPoint, normal)) / (doubleArea * sqrt(distance2));
    if (cosine <= 0.0) {
        return 0.0;
    }
    FloatArray cdf = FloatArray(light.triangleCdf);
    float picked = cdf.values[triangle] - (triangle > 0u ? cdf.values[triangle - 1u] : 0.0);
    return picked * (2.0 / doubleArea) * distance2 / cosine;
}

/**
 * A direction to a light from `position`: to a point of it (meshes, at
 * `distance`; spheres: kFar), and its solid-angle pdf. False if none.
 */
bool sampleLight(Light light, vec3 position, inout uint rng, out vec3 direction,
                 out float distance, out float pdf)
{
    direction = vec3(0.0);
    distance = kFar;
    pdf = 0.0;
    if (light.triangleCount == 0u) {
        vec3 toCenter = light.center - position;
        float distance2 = dot(toCenter, toCenter);
        float u1 = nextFloat(rng);
        float u2 = nextFloat(rng);
        if (distance2 <= light.radius * light.radius) {
            return false;
        }
        float oneMinusCos = coneOneMinusCos(light.radius, distance2);
        direction = sampleCone(toCenter / sqrt(distance2), oneMinusCos, u1, u2);
        pdf = conePdf(oneMinusCos);
        return true;
    }
    // The first triangle whose cumulative area exceeds u.
    float u = nextFloat(rng);
    FloatArray cdf = FloatArray(light.triangleCdf);
    uint low = 0u;
    uint high = light.triangleCount - 1u;
    while (low < high) {
        uint middle = (low + high) / 2u;
        if (u < cdf.values[middle]) {
            high = middle;
        } else {
            low = middle + 1u;
        }
    }
    float u1 = nextFloat(rng);
    float u2 = nextFloat(rng);
    Instance instance = instances[light.instanceIndex];
    uvec3 indices = triangleIndices(instance, low);
    float s = sqrt(u1);
    vec3 point = lightVertex(light, instance, indices, 0) * (1.0 - s) +
                 lightVertex(light, instance, indices, 1) * (s * (1.0 - u2)) +
                 lightVertex(light, instance, indices, 2) * (s * u2);
    vec3 toPoint = point - position;
    distance = length(toPoint);
    pdf = meshLightPdf(light, low, position, point);
    if (distance <= 0.0 || pdf <= 0.0) {
        return false;
    }
    direction = toPoint / distance;
    return true;
}

/**
 * Solid-angle pdf of next event estimation at a surface point (picking light
 * `index` and its point `lightHit`), for MIS against BSDF sampling.
 */
float directLightPdf(uint index, vec3 position, vec3 normal, uint surfaceInstance,
                     SurfaceHit lightHit)
{
    float weight = lightWeight(index, position, normal, surfaceInstance);
    if (weight <= 0.0) {
        return 0.0;
    }
    Light light = lights[index];
    float probability = weight / totalLightWeight(position, normal, surfaceInstance);
    if (light.triangleCount == 0u) {
        vec3 toCenter = light.center - position;
        return probability * conePdf(coneOneMinusCos(light.radius, dot(toCenter, toCenter)));
    }
    return probability * meshLightPdf(light, lightHit.primitive, position, lightHit.position);
}

/** Next event estimation for the diffuse lobe (its weight applied by the caller). */
vec3 sampleDirectLight(SurfaceHit hit, inout uint rng)
{
    uint index;
    float probability;
    if (!pickLight(hit.position, hit.normal, hit.instanceIndex, nextFloat(rng), index,
                   probability)) {
        return vec3(0.0);
    }
    Light light = lights[index];
    vec3 direction;
    float distance;
    float pdf;
    if (!sampleLight(light, hit.position, rng, direction, distance, pdf)) {
        return vec3(0.0);
    }
    float cosine = dot(direction, hit.normal);
    if (cosine <= 0.0 || dot(direction, hit.geometricNormal) <= 0.0) {
        return vec3(0.0);
    }
    vec3 transmittance = shadowTransmittance(
        offsetOrigin(hit.position, hit.geometricNormal, direction), direction,
        light.instanceIndex, distance);
    if (maxComponent(transmittance) <= 0.0) {
        return vec3(0.0);
    }
    float lightPdf = probability * pdf;
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
    // Probability of the sampled set of lobes (sampleLobes), else 1.
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

// Sets of lobes for sampleLobes (bits).
const uint kLobesReflection = 2u;  // Off the surface: metal and dielectric reflection.
const uint kLobesDiffuse = 4u;
const uint kLobesTransmission = 8u;
const uint kLobesAll = kLobesReflection | kLobesDiffuse | kLobesTransmission;

/**
 * sampleBsdf restricted to a set of lobes, the others followed separately by
 * the hybrid renderer: one of them, chosen by their probabilities relative to
 * each other and weighted by their sum (`scale`). Metal and dielectric
 * reflection are one lobe here: both mirror about the sampled microfacet
 * normal, so their weights add up.
 */
bool sampleLobes(Material material, vec3 wo, SurfaceHit hit, uint lobes, inout uint rng,
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
    bool thin = (material.flags & kMaterialThinWalled) != 0u;
    float eta = thin || hit.frontFace ? 1.0 / material.ior : material.ior;
    float reflectance = fresnelDielectric(dot(woLocal, h), eta);
    float metal = material.metallic;
    // The probabilities of sampleBsdf's lobes.
    float rest = (1.0 - metal) * (1.0 - reflectance);
    float reflection = (lobes & kLobesReflection) != 0u ? metal + (1.0 - metal) * reflectance : 0.0;
    float transmission = (lobes & kLobesTransmission) != 0u ? rest * material.transmission : 0.0;
    float diffuse = (lobes & kLobesDiffuse) != 0u ? rest * (1.0 - material.transmission) : 0.0;
    float total = reflection + transmission + diffuse;
    if (total <= 0.0) {
        return false;
    }
    scatter.scale = total;

    float choice = nextFloat(rng) * total;
    if (choice < reflection) {
        vec3 world = toWorld(frame, reflectLocal(woLocal, h));
        if (dot(world, hit.geometricNormal) <= 0.0) {
            return false;
        }
        scatter.direction = world;
        scatter.weight = (metal * fresnelSchlick(baseColor, dot(woLocal, h)) +
                          vec3((1.0 - metal) * reflectance)) *
                         (total / reflection);
        scatter.lobe = kLobeSpecular;
        return true;
    }
    if (choice < reflection + transmission) {
        vec3 transmitted;
        if (thin) {
            transmitted = reflectLocal(woLocal, h);
        } else if (!refractDirection(-woLocal, h, eta, transmitted)) {
            return false;  // Unreachable: the Fresnel term was 1.
        }
        transmitted.z = -abs(transmitted.z);
        vec3 world = normalize(toWorld(frame, transmitted));
        if (dot(world, hit.geometricNormal) > 0.0) {
            return false;
        }
        scatter.direction = world;
        scatter.weight = baseColor * total;
        scatter.lobe = kLobeTransmission;
        scatter.passThrough = thin;
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
    vec3 diffuseNormal;
    uint diffuseInstance;
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
    path.diffuseNormal = vec3(0.0);
    path.diffuseInstance = 0u;
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

// pathVertex flags, and kLobes* bits: only those lobes (sampleLobes; none: sampleBsdf).
const uint kVertexSkipEmission = 1u;  // The caller accounts for the vertex's emission.

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
                                    directLightPdf(uint(lightIndex), path.diffusePosition,
                                                   path.diffuseNormal, path.diffuseInstance, hit));
        }
        path.radiance +=
            clampContribution(path.throughput * emission * weight, path.scatterings, path.scale);
    }

    vec3 wo = -normalize(direction);
    Scatter scatter;
    uint lobes = flags & kLobesAll;
    bool scattered = lobes != 0u ? sampleLobes(material, wo, hit, lobes, rng, scatter)
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
        path.diffuseNormal = hit.normal;
        path.diffuseInstance = hit.instanceIndex;
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
            if (light.triangleCount > 0u) {
                continue;  // Meshes: their own emission only.
            }
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
                              light.instanceIndex, kFar);
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
