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
 * Fraction of the light of a point `distance` along the ray that arrives:
 * attenuated by thin panes, 0 if anything else is before it (see
 * shadowTransmittance in path_tracer.cpp). One ray query that ends at the
 * first opaque surface (not the nearest: any blocks the light): thin
 * see-through instances are not opaque in the TLAS (GpuScene), so they come
 * up as candidates, which only attenuate the light (in any order: it is a
 * product).
 */
vec3 shadowTransmittance(vec3 origin, vec3 direction, float distance)
{
    // Short of the point: not its own surface.
    float tMax = distance * (1.0 - kShadowShortening);
    vec3 transmittance = vec3(1.0);
    rayQueryEXT query;
    rayQueryInitializeEXT(query, tlas, gl_RayFlagsTerminateOnFirstHitEXT, instanceMask() & 0xFFu,
                          origin, 0.0, direction, tMax);
    while (rayQueryProceedEXT(query)) {
        uint instanceIndex = uint(rayQueryGetIntersectionInstanceCustomIndexEXT(query, false));
        Material material = materialOf(instanceIndex);
        bool thin = isThinTransmissive(material);
        mat3 worldToObject = mat3(rayQueryGetIntersectionWorldToObjectEXT(query, false));
        if (rayQueryGetIntersectionTypeEXT(query, false) == gl_RayQueryCandidateIntersectionAABBEXT) {
            // A sphere: crossed twice if see-through (as two surfaces).
            vec3 o = rayQueryGetIntersectionObjectRayOriginEXT(query, false);
            vec3 d = rayQueryGetIntersectionObjectRayDirectionEXT(query, false);
            float t;
            for (float tMin = 0.0; intersectUnitSphere(o, d, tMin, tMax, t); tMin = t) {
                if (!thin) {
                    rayQueryGenerateIntersectionEXT(query, t);
                    break;
                }
                vec3 normal = normalize((o + t * d) * worldToObject);
                transmittance *= thinTransmittance(material, abs(dot(direction, normal)));
            }
        } else if (!thin) {
            rayQueryConfirmIntersectionEXT(query);  // Not see-through after all: blocks.
        } else {
            vec3 objectGeometric;
            vec3 objectShading;
            objectNormals(instances[instanceIndex],
                          uint(rayQueryGetIntersectionPrimitiveIndexEXT(query, false)),
                          rayQueryGetIntersectionBarycentricsEXT(query, false), vec3(0.0),
                          objectGeometric, objectShading);
            vec3 normal = normalize(objectGeometric * worldToObject);
            transmittance *= thinTransmittance(material, abs(dot(direction, normal)));
        }
        if (maxComponent(transmittance) <= 0.0) {
            return vec3(0.0);
        }
    }
    if (rayQueryGetIntersectionTypeEXT(query, true) != gl_RayQueryCommittedIntersectionNoneEXT) {
        return vec3(0.0);
    }
    return transmittance;
}

/** How bright a light is (luminance of its emission), to pick it by. */
float lightBrightness(vec3 emission)
{
    return 0.2126 * emission.r + 0.7152 * emission.g + 0.0722 * emission.b;
}

// Kinds of lights (bits), to sample some of them: the hybrid renderer's direct
// layer takes the spheres' light at the primary surfaces (direct.comp), their
// paths the meshes'.
const uint kLightSpheres = 1u;
const uint kLightMeshes = 2u;
const uint kLightsAll = kLightSpheres | kLightMeshes;

uint lightKind(uint index)
{
    return lights[index].triangleCount == 0u ? kLightSpheres : kLightMeshes;
}

/**
 * How much light `index` may light a surface point, roughly: its brightness
 * times the solid angle it covers. 0 if it cannot: below the surface's
 * horizon, around the point (a sphere), the surface itself, or not of `kinds`.
 * (Reads only the fields it needs: it runs for every light.)
 */
float lightWeight(uint index, vec3 position, vec3 normal, uint surfaceInstance, uint kinds)
{
    if (lights[index].instanceIndex == surfaceInstance || (lightKind(index) & kinds) == 0u) {
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

/**
 * Lights to visit for `kinds`: the spheres come first (RenderScene::lights), so of
 * them alone, up to the first mesh.
 */
bool visitLight(uint index, uint kinds)
{
    return index < lightCount() && (kinds != kLightSpheres || lights[index].triangleCount == 0u);
}

/** Sum of the weights of the lights of `kinds` at a surface point. */
float totalLightWeight(vec3 position, vec3 normal, uint surfaceInstance, uint kinds)
{
    float total = 0.0;
    for (uint i = 0u; visitLight(i, kinds); ++i) {
        total += lightWeight(i, position, normal, surfaceInstance, kinds);
    }
    return total;
}

/**
 * Pick a light for a surface point by the lights' weights, `u` in [0, 1);
 * false if none may light it. `probability`: of the pick. One pass (weighted
 * reservoir sampling, `u` rescaled at each light).
 */
bool pickLight(vec3 position, vec3 normal, uint surfaceInstance, uint kinds, float u,
               out uint picked, out float probability)
{
    picked = 0u;
    probability = 0.0;
    float total = 0.0;
    float weight = 0.0;
    for (uint i = 0u; visitLight(i, kinds); ++i) {
        float w = lightWeight(i, position, normal, surfaceInstance, kinds);
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
        // To the sphere's near surface that way (the cone's directions all meet it).
        float along = dot(toCenter, direction);
        float offAxis2 = max(0.0, distance2 - along * along);
        distance = along - sqrt(max(0.0, light.radius * light.radius - offAxis2));
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
float directLightPdf(uint index, vec3 position, vec3 normal, uint surfaceInstance, uint kinds,
                     SurfaceHit lightHit)
{
    float weight = lightWeight(index, position, normal, surfaceInstance, kinds);
    if (weight <= 0.0) {
        return 0.0;
    }
    Light light = lights[index];
    float probability = weight / totalLightWeight(position, normal, surfaceInstance, kinds);
    if (light.triangleCount == 0u) {
        vec3 toCenter = light.center - position;
        return probability * conePdf(coneOneMinusCos(light.radius, dot(toCenter, toCenter)));
    }
    return probability * meshLightPdf(light, lightHit.primitive, position, lightHit.position);
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
    // Probability that the sampled microfacet chose this lobe (reflection: metal or
    // dielectric), among the sampled set.
    float probability;
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
    scatter.probability = 1.0;

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
    // n_incident / n_transmitted: entering the volume through its front face.
    float eta = thin || hit.frontFace ? 1.0 / material.ior : material.ior;
    float reflectance = fresnelDielectric(dot(woLocal, h), eta);
    float reflection = material.metallic + (1.0 - material.metallic) * reflectance;
    if (nextFloat(rng) < material.metallic) {
        vec3 world = toWorld(frame, reflectLocal(woLocal, h));
        if (dot(world, hit.geometricNormal) <= 0.0) {
            return false;
        }
        scatter.direction = world;
        scatter.weight = fresnelSchlick(baseColor, dot(woLocal, h));
        scatter.lobe = kLobeSpecular;
        scatter.probability = reflection;
        return true;
    }

    if (nextFloat(rng) < reflectance) {
        vec3 world = toWorld(frame, reflectLocal(woLocal, h));
        if (dot(world, hit.geometricNormal) <= 0.0) {
            return false;
        }
        scatter.direction = world;
        scatter.weight = vec3(1.0);
        scatter.lobe = kLobeSpecular;
        scatter.probability = reflection;
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
    scatter.probability = 1.0;

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
        scatter.probability = reflection / total;
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

// Reflections at least this rough also sample the lights (smoother ones are too
// sharp for it: their lights are found by sampling the reflection).
const float kGlossyLightRoughness = 0.15;

/**
 * The reflection lobe as sampleBsdf / sampleLobes (`lobes`, 0: all) sample it,
 * for a direction `wi` (see evaluateReflection in path_tracer.cpp): BSDF
 * times cosine (`value`), and the density with which sampling returns `wi`
 * from the reflection lobe (of the lobes sampled).
 */
vec3 evaluateReflection(Material material, vec3 wo, SurfaceHit hit, vec3 wi, uint lobes,
                        out float pdf)
{
    pdf = 0.0;
    Frame frame = makeFrame(hit.normal);
    vec3 woLocal = toLocal(frame, wo);
    woLocal.z = max(woLocal.z, 1e-5);
    woLocal = normalize(woLocal);
    vec3 wiLocal = toLocal(frame, wi);
    vec3 value = vec3(0.0);
    if (wiLocal.z <= 0.0) {
        return value;
    }
    float alpha = roughnessToAlpha(material.roughness);
    bool thin = (material.flags & kMaterialThinWalled) != 0u;
    float eta = thin || hit.frontFace ? 1.0 / material.ior : material.ior;
    float metal = material.metallic;
    for (int folded = 0; folded < 2; ++folded) {
        if (folded == 1 && wiLocal.z >= woLocal.z) {
            break;  // The microfacet would face down.
        }
        vec3 h = normalize(woLocal + vec3(wiLocal.xy, folded == 1 ? -wiLocal.z : wiLocal.z));
        float density = ggxVisibleNormalPdf(woLocal, h, alpha);
        if (density <= 0.0) {
            continue;
        }
        float cosine = dot(woLocal, h);
        float perDirection = density / (4.0 * cosine);  // Mirroring's Jacobian.
        float reflectance = fresnelDielectric(cosine, eta);
        float reflection = metal + (1.0 - metal) * reflectance;
        // Of the lobes sampled, the reflection's share for this microfacet.
        float total = 1.0;
        if (lobes != 0u) {
            float rest = (1.0 - metal) * (1.0 - reflectance);
            total = reflection +
                    ((lobes & kLobesTransmission) != 0u ? rest * material.transmission : 0.0) +
                    ((lobes & kLobesDiffuse) != 0u ? rest * (1.0 - material.transmission) : 0.0);
        }
        pdf += reflection / total * perDirection;
        value += (metal * fresnelSchlick(material.albedo.rgb, cosine) +
                  vec3((1.0 - metal) * reflectance)) *
                 perDirection;
    }
    return value;
}

/**
 * Next event estimation for the lobe a vertex sampled (see sampleDirectLight
 * and sampleReflectedLight in path_tracer.cpp): radiance of one sampled light
 * times, for the diffuse lobe, cos / pi (MIS-weighted against cosine sampling;
 * the caller applies the albedo), for the reflection, the lobe (BSDF times
 * cosine; MIS-weighted against sampling it). One function for both: shaders
 * inline every call, and a second copy of the shadow ray costs registers.
 */
vec3 sampleLobeLight(Material material, vec3 wo, SurfaceHit hit, bool reflection, uint lobes,
                     uint kinds, inout uint rng)
{
    uint index;
    float probability;
    if (!pickLight(hit.position, hit.normal, hit.instanceIndex, kinds, nextFloat(rng), index,
                   probability)) {
        return vec3(0.0);
    }
    Light light = lights[index];
    vec3 direction;
    float distance;
    float pdf;
    if (!sampleLight(light, hit.position, rng, direction, distance, pdf) ||
        dot(direction, hit.geometricNormal) <= 0.0) {
        return vec3(0.0);
    }
    vec3 value;
    float bsdfPdf;
    if (reflection) {
        value = evaluateReflection(material, wo, hit, direction, lobes, bsdfPdf);
    } else {
        bsdfPdf = max(0.0, dot(direction, hit.normal)) / kPi;
        value = vec3(bsdfPdf);
    }
    if (maxComponent(value) <= 0.0) {
        return vec3(0.0);
    }
    vec3 transmittance = shadowTransmittance(
        offsetOrigin(hit.position, hit.geometricNormal, direction), direction, distance);
    if (maxComponent(transmittance) <= 0.0) {
        return vec3(0.0);
    }
    float lightPdf = probability * pdf;
    return light.emission * transmittance * value *
           (powerHeuristic(lightPdf, bsdfPdf) / lightPdf);
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
    // The last vertex that also sampled the lights (diffuse or glossy), to MIS-weight
    // lights found by BSDF sampling from it: where it was and the density of the
    // direction sampled there (diffuse: given the lobe; glossy: of the lobes sampled).
    bool mis;
    vec3 misPosition;
    vec3 misNormal;
    uint misInstance;
    float misPdf;
    uint misLights;  // The kinds of lights it sampled.
    float scale;
    // The kinds of lights whose light the last vertex left to someone else
    // (kVertexSkipLights): those its lobe finds are not the path's.
    uint skipLights;
    // How far the directions the path may have taken spread (radians, roughly: a diffuse
    // lobe 1, a glossy one its alpha): the width of its footprint per unit of distance
    // (the radiance cache is looked up where it is as wide as the cache's cells).
    float spread;
    uint lobe;  // The lobe the last vertex sampled (kNoLobe: none, absorbed).
};

const uint kNoLobe = 0xFFu;

PathState newPath()
{
    PathState path;
    path.radiance = vec3(0.0);
    path.throughput = vec3(1.0);
    path.scatterings = 0u;
    path.mis = false;
    path.misPosition = vec3(0.0);
    path.misNormal = vec3(0.0);
    path.misInstance = 0u;
    path.misPdf = 0.0;
    path.misLights = kLightsAll;
    path.scale = 1.0;
    path.skipLights = 0u;
    path.spread = 0.0;
    path.lobe = kNoLobe;
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
// The caller accounts for the light the vertex gets straight from the sphere lights
// through its diffuse lobe: there, its next event estimation samples the meshes only,
// and the spheres its sampled direction finds next are not counted (the hybrid
// renderer's direct layer, direct.comp).
const uint kVertexSkipLights = 16u;
// And through its glossy reflection (kGlossyLightRoughness and rougher; the hybrid
// renderer's path hits, sphere_light.glsl).
const uint kVertexSkipGlossyLights = 32u;

/**
 * The emission of a vertex a path reached, MIS-weighted against the lights sampled at
 * the vertex before (pathVertex adds it, unless kVertexSkipEmission).
 */
void addVertexEmission(SurfaceHit hit, Material material, int lightIndex, inout PathState path)
{
    bool nextEvent = nextEventEnabled() && lightCount() > 0u;
    vec3 emission = material.emission.rgb;
    if (maxComponent(emission) > 0.0 &&
        !(lightIndex >= 0 && (lightKind(uint(lightIndex)) & path.skipLights) != 0u)) {
        float weight = 1.0;
        if (nextEvent && path.mis && lightIndex >= 0) {
            weight = powerHeuristic(path.misPdf,
                                    directLightPdf(uint(lightIndex), path.misPosition,
                                                   path.misNormal, path.misInstance,
                                                   path.misLights, hit));
        }
        path.radiance +=
            clampContribution(path.throughput * emission * weight, path.scatterings, path.scale);
    }
}

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

    if ((flags & kVertexSkipEmission) == 0u) {
        addVertexEmission(hit, material, lightIndex, path);
    }

    vec3 wo = -normalize(direction);
    Scatter scatter;
    uint lobes = flags & kLobesAll;
    bool scattered = lobes != 0u ? sampleLobes(material, wo, hit, lobes, rng, scatter)
                                 : sampleBsdf(material, wo, hit, rng, scatter);
    path.lobe = scattered ? scatter.lobe : kNoLobe;
    if (!scattered) {
        return false;  // Absorbed.
    }
    // The lights for the lobe sampled: diffuse, or glossy reflection (not near mirrors:
    // their lights are better found by sampling the lobe).
    bool glossy = scatter.lobe == kLobeSpecular && material.roughness >= kGlossyLightRoughness;
    // The density of the direction sampled, for MIS at the next vertex (before the
    // shadow ray: what it needs is not kept across it).
    float misPdf = max(0.0, dot(scatter.direction, hit.normal)) / kPi;
    if (glossy) {
        evaluateReflection(material, wo, hit, scatter.direction, lobes, misPdf);
    }
    // The kinds of lights sampled here (the others' light is the caller's).
    uint skipped = scatter.lobe == kLobeDiffuse ? kVertexSkipLights : kVertexSkipGlossyLights;
    uint sampled = nextEvent && (scatter.lobe == kLobeDiffuse || glossy) && (flags & skipped) != 0u
                       ? kLightMeshes
                       : kLightsAll;
    if (nextEvent && (scatter.lobe == kLobeDiffuse || glossy)) {
        // The reflection's, when the microfacet picked reflection: weighted up by that
        // chance.
        vec3 weight = glossy ? vec3(1.0 / scatter.probability) : scatter.weight;
        path.radiance += clampContribution(path.throughput * weight *
                                               sampleLobeLight(material, wo, hit, glossy, lobes,
                                                               sampled, rng),
                                           path.scatterings + 1u, path.scale);
    }
    path.throughput *= scatter.weight;
    path.scale *= scatter.scale;
    if (!scatter.passThrough) {
        ++path.scatterings;
        path.spread = scatter.lobe == kLobeDiffuse
                          ? 1.0
                          : path.spread + roughnessToAlpha(material.roughness);
    }
    if (scatter.lobe == kLobeDiffuse || glossy) {
        path.mis = true;
        path.misPosition = hit.position;
        path.misNormal = hit.normal;
        path.misInstance = hit.instanceIndex;
        path.misPdf = misPdf;
        path.misLights = sampled;
    } else if (!scatter.passThrough) {
        path.mis = false;
    }
    if (!scatter.passThrough) {
        path.skipLights =
            (scatter.lobe == kLobeDiffuse || glossy) ? kLightsAll & ~sampled : 0u;
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

#ifdef STARCORE_HYBRID_PATHS
// The hybrid pipeline's (sphere_light.glsl, radiance_cache.glsl).
bool closedFormHitLights();
float diffuseLobe(Material material);
vec3 closedFormHitLight(SurfaceHit hit, Material material, vec3 wo, vec2 u, bool glossy);
bool radianceCacheEnabled();
bool cachedReflection(SurfaceHit hit, Material material, vec3 wo, float footprint, vec2 u,
                      out vec3 reflected);
#endif

/**
 * A path from a vertex already found (`hit`, reached along `direction`;
 * `flags` for it): that vertex, then further ones, traced, until `end` (see
 * continuePath). In the hybrid pipeline (STARCORE_HYBRID_PATHS), with the radiance cache,
 * until a vertex past the first whose light the cache knows; with DirectShadows::
 * ShadowMaps, its second vertex lit by the sphere lights in closed form. One call of
 * pathVertex for all of them: shaders inline every
 * call, and each copy of a vertex (light sampling, shadow rays) takes
 * registers. `firstRadiance`: the path's radiance after the first vertex;
 * `firstDirection`: the ray it continued with (0: none); `secondRadiance`:
 * the radiance after the second vertex (or the sky it found instead).
 *
 * Each vertex after the first draws its random numbers from a stream of its
 * own, seeded from the path's: the hybrid pipeline's temporal gradients
 * re-trace a path's first vertices with the numbers of the previous frame,
 * and the second's must not depend on how many the first one's light sample
 * took (a sphere's and a mesh's take different counts).
 */
void tracePathFrom(SurfaceHit hit, vec3 direction, uint bounce, uint end, uint flags,
                   inout PathState path, inout uint rng, out vec3 firstRadiance,
                   out vec3 firstDirection, out vec3 secondRadiance)
{
    firstRadiance = path.radiance;
    firstDirection = vec3(0.0);
    secondRadiance = path.radiance;
    Instance instance = instances[hit.instanceIndex];
    vec3 origin = vec3(0.0);
    uint stream = rng;
    for (uint vertex = 0u; bounce < end; ++bounce, ++vertex) {
        if (vertex > 0u) {
            if (!traceClosestHit(origin, direction, instanceMask(), hit)) {
                path.radiance +=
                    clampContribution(path.throughput * skyColor(), path.scatterings, path.scale);
                if (vertex < 2u) {
                    secondRadiance = path.radiance;
                }
                return;
            }
            instance = instances[hit.instanceIndex];
            flags = 0u;
            rng = pcgHash(stream + vertex * 0x9E3779B9u);
#ifdef STARCORE_HYBRID_PATHS
            Material material = materials[instance.materialIndex];
            vec3 reflected;
            // Of the sphere lights' shadow rays (sphere_light.glsl): numbers of a stream of
            // their own, the vertex's as it was.
            uint lightStream = pcgHash(rng ^ 0x85EBCA6Bu);
            vec2 u = vec2(nextFloat(lightStream), nextFloat(lightStream));
            if (radianceCacheEnabled() &&
                cachedReflection(hit, material, -direction, path.spread * hit.t, u, reflected)) {
                // Its emission, and the light it reflects as the cache knows it.
                addVertexEmission(hit, material, instance.lightIndex, path);
                path.radiance += clampContribution(path.throughput * reflected,
                                                   path.scatterings + 1u, path.scale);
                if (vertex < 2u) {
                    secondRadiance = path.radiance;
                }
                return;
            }
            // The first vertex past the path's start (where reflections find what they show,
            // where bounced light comes from), as engines light it: the light the sphere
            // lights give its diffuse lobe and its glossy reflection in closed form,
            // shadowed, its lights sampled only the meshes there -- without the noise of a
            // sun found at random (as the direct layer's primary surfaces). Not deeper:
            // little of the light, each a few rays. (The glossy one over the light's cap or a
            // grid of the lobe's directions, sphereHighlight. Sampled, it was most of the
            // noise of a canopy reflecting a sunlit cockpit, and re-traced for the temporal
            // gradients it came out different as a ship flew nearer a light: the lights'
            // weights changed which one it picked.)
            if (vertex == 1u && closedFormHitLights()) {
                bool diffuse = diffuseLobe(material) > 0.0;
                bool glossy = material.roughness >= kGlossyLightRoughness;
                if (diffuse || glossy) {
                    path.radiance += clampContribution(
                        path.throughput * closedFormHitLight(hit, material, -direction, u, glossy),
                        path.scatterings + 1u, path.scale);
                    flags = (diffuse ? kVertexSkipLights : 0u) |
                            (glossy ? kVertexSkipGlossyLights : 0u);
                }
            }
#endif
        }
        bool more = pathVertex(hit, materials[instance.materialIndex], instance.lightIndex,
                               direction, bounce, flags, path, rng, origin, direction);
        if (vertex == 0u) {
            firstRadiance = path.radiance;
            firstDirection = direction;
        }
        if (vertex < 2u) {
            secondRadiance = path.radiance;
        }
        if (!more) {
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
                              sqrt(distance2) - light.radius);
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
