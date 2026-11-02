// The sphere lights' light on a surface's diffuse lobe in closed form, shadowed
// without noise: the hybrid renderer's direct layer (direct.comp, with shadow maps
// where STARCORE_SPHERE_SHADOW_MAPS is defined: shadows.glsl included first), its paths'
// hits (path.glsl's tracePathFrom; there also their glossy reflections') and its
// radiance cache (radiance_cache.glsl: at the hits it shades, rays). Include after
// path.glsl.

#ifndef STARCORE_SPHERE_LIGHT_GLSL
#define STARCORE_SPHERE_LIGHT_GLSL

#include "light_part.glsl"

// Lights giving less than this share of a point's light are not shadowed (as the
// raster pipeline's).
const float kFaintShare = 0.02;

/** The diffuse lobe's share of the light: what neither metal nor transmission take. */
float diffuseLobe(Material material)
{
    return (1.0 - material.metallic) * (1.0 - material.transmission);
}

/**
 * Illuminance factor of a sphere of angular radius asin(sqrt(sinSigma2)) whose
 * center is at angle acos(cosTheta) from the normal, clipped by the horizon
 * (Snyder; as Lagarde and de Rousiers, "Moving Frostbite to PBR", 2014): times its
 * radiance, the irradiance.
 */
float sphereIlluminance(float cosTheta, float sinSigma2)
{
    if (cosTheta * cosTheta > sinSigma2) {
        return kPi * sinSigma2 * max(cosTheta, 0.0);
    }
    float sinTheta = sqrt(max(1.0 - cosTheta * cosTheta, 1e-12));
    float x = sqrt(1.0 / sinSigma2 - 1.0);
    float y = -x * cosTheta / sinTheta;
    float sinThetaSqrtY = sinTheta * sqrt(max(1.0 - y * y, 0.0));
    return max((cosTheta * acos(clamp(y, -1.0, 1.0)) - x * sinThetaSqrtY) * sinSigma2 +
                   atan(sinThetaSqrtY / x),
               0.0);
}

/** The irradiance of sphere light i at a surface point, unshadowed, in closed form. */
vec3 sphereIrradiance(uint i, SurfaceHit hit)
{
    if (lights[i].instanceIndex == hit.instanceIndex) {
        return vec3(0.0);  // Its own light (as sampling the lights).
    }
    vec3 toCenter = lights[i].center - hit.position;
    float distance2 = dot(toCenter, toCenter);
    float radius = lights[i].radius;
    if (distance2 <= radius * radius) {
        return vec3(0.0);  // Inside it (as sampling the lights).
    }
    float cosTheta = dot(toCenter, hit.normal) * inversesqrt(distance2);
    return lights[i].emission *
           sphereIlluminance(cosTheta, min(radius * radius / distance2, 0.9999));
}

/**
 * The diffuse lobe's share of the irradiance, over the albedo: what the microfacets'
 * Fresnel reflection leaves, averaged over the visible normals as the paths draw them
 * (sampleBsdf) by a fixed quadrature (2 radii x 4 angles), so without noise. (Fresnel
 * at the normal instead made rough surfaces seen at grazing angles too dark: the
 * visible normals turn towards the eye.)
 */
float closedFormShare(Material material, vec3 wo, SurfaceHit hit)
{
    bool thin = (material.flags & kMaterialThinWalled) != 0u;
    float eta = thin || hit.frontFace ? 1.0 / material.ior : material.ior;
    vec3 woLocal = toLocal(makeFrame(hit.normal), wo);
    woLocal.z = max(woLocal.z, 1e-5);
    woLocal = normalize(woLocal);
    float alpha = roughnessToAlpha(material.roughness);
    float transmitted = 0.0;
    [[unroll]] for (int k = 0; k < 8; ++k) {
        vec3 h = sampleGgxVisibleNormal(woLocal, alpha, 0.25 + 0.5 * float(k & 1),
                                        0.125 + 0.25 * float(k >> 1));
        transmitted += 1.0 - fresnelDielectric(dot(woLocal, h), eta);
    }
    return diffuseLobe(material) / kPi * transmitted * 0.125;
}

/**
 * The light the diffuse lobe of a surface point would get unshadowed from the
 * sphere lights, in closed form, over its albedo.
 */
vec3 analyticLight(Material material, vec3 wo, SurfaceHit hit)
{
    vec3 sum = vec3(0.0);
    for (uint i = 0u; visitLight(i, kLightSpheres); ++i) {
        sum += sphereIrradiance(i, hit);
    }
    return closedFormShare(material, wo, hit) * sum;
}

/**
 * How much of a sphere light arrives at a surface point along a ray to a point of its
 * part above the surface's plane (light_part.glsl; `u`: where, uniform over it): in
 * expectation, the share of it past what is in the way (soft shadows, sampled).
 */
vec3 sphereTransmittance(Light light, SurfaceHit hit, vec2 u)
{
    Frame frame = makeFrame(normalize(hit.position - light.center));
    LightPart part =
        lightPartAbove(light.center, light.radius, hit.position, hit.geometricNormal, frame);
    float angle = kTwoPi * u.x;
    vec2 offset = onLightPart(part, sqrt(u.y) * vec2(cos(angle), sin(angle))).xy * light.radius;
    vec3 direction = normalize(light.center + frame.tangent * offset.x +
                               frame.bitangent * offset.y - hit.position);
    if (dot(direction, hit.geometricNormal) <= 0.0) {
        return vec3(1.0);  // All of it set (as the maps: not shaded by the surface itself).
    }
    // To the light's surface: the point of its disk is inside it.
    vec3 toCenter = light.center - hit.position;
    float along = dot(direction, toCenter);
    float distance =
        along - sqrt(max(along * along - dot(toCenter, toCenter) + light.radius * light.radius,
                         0.0));
    return shadowTransmittance(offsetOrigin(hit.position, hit.geometricNormal, direction),
                               direction, distance);
}

// sphereHighlight's directions: of a light's cap, this many (of equal solid angle); of the
// reflection lobe's microfacet normals, a grid of this many per side.
const int kCapPoints = 16;
const int kLobeGrid = 4;
const float kGoldenAngle = 2.39996323;

/**
 * The light sphere light i gives a surface's reflection lobe seen from `wo` (BSDF times
 * cosine, as evaluateReflection's), unshadowed, with little noise. Of a light smaller than
 * the lobe (its angular radius under twice alpha, about the directions' spread), the lobe
 * integrated over its cap: kCapPoints directions of equal solid angle, a sunflower turned
 * by u.x (stratified, so the mean converges; those under the surface's plane give none).
 * Of a light wider than the lobe, whose peak few of those would find, the paths' estimate
 * where they sample the reflection, over a grid of the lobe's microfacet normals (visible
 * normals, sampleGgxVisibleNormal; each cell's at an offset of its own, turned by u: a
 * fixed grid's steps showed as rings in a highlight) -- the share of the directions they
 * mirror `wo` into (folded above the surface, as sampleBsdf's) that meet the light, soft
 * at its edge, each times its reflectance. Between, both. Of a faint light, the lobe at
 * its center times the solid angle of its part above the surface's plane. (The paths
 * re-traced for the temporal gradients draw the same u: where nothing changed, the same
 * light.) Against the lobe integrated over the light, roughness 0.15 to 0.8, views to
 * grazing, lights low over the horizon: of a metal 1 % off, weighted by the light, of glass
 * 4 % (its reflectance changes fast at grazing views); noise over u 8 % of it. (The grid
 * alone: 30 %, most where the light is about as wide as the lobe -- a canopy reflecting a
 * cockpit flickered with it while the ship moved; a representative point, Karis 2013, was
 * 2.3 times the light at a grazing view of one low over the horizon.)
 */
vec3 sphereHighlight(uint i, Material material, vec3 wo, SurfaceHit hit, vec2 u, bool faint)
{
    if (lights[i].instanceIndex == hit.instanceIndex) {
        return vec3(0.0);
    }
    vec3 toCenter = lights[i].center - hit.position;
    float distance2 = dot(toCenter, toCenter);
    float radius = lights[i].radius;
    if (distance2 <= radius * radius) {
        return vec3(0.0);
    }
    vec3 center = toCenter * inversesqrt(distance2);
    float sinRadius = radius * inversesqrt(distance2);
    float unusedPdf;
    if (faint) {
        // The lobe towards its center, times its solid angle above the surface's plane (of
        // its disk facing the point, the segment past the plane's chord).
        vec3 lobe = evaluateReflection(material, wo, hit, center, 0u, unusedPdf);
        float facing = dot(center, hit.normal);
        float chord = clamp(-facing / (sinRadius * sqrt(max(1.0 - facing * facing, 1e-12))),
                            -1.0, 1.0);
        float above = (acos(chord) - chord * sqrt(1.0 - chord * chord)) / kPi;
        return lights[i].emission * lobe * (kPi * sinRadius * sinRadius * above);
    }
    float angularRadius = asin(sinRadius);
    float alpha = roughnessToAlpha(material.roughness);
    Frame frame = makeFrame(hit.normal);
    vec3 woLocal = toLocal(frame, wo);
    woLocal.z = max(woLocal.z, 1e-5);
    woLocal = normalize(woLocal);
    vec3 centerLocal = toLocal(frame, center);
    bool thin = (material.flags & kMaterialThinWalled) != 0u;
    float eta = thin || hit.frontFace ? 1.0 / material.ior : material.ior;
    // The grid's share: of a light wider than the lobe.
    float narrow = smoothstep(1.0, 2.0, angularRadius / (2.0 * alpha));
    vec3 result = vec3(0.0);
    if (narrow < 1.0) {
        // evaluateReflection per direction: of each microfacet mirroring `wo` into it (and
        // the folded one), the visible normals' density mirrored -- D(h) G1(wo) / (4 wo.z)
        // -- times its reflectance.
        Frame cap = makeFrame(centerLocal);
        float oneMinusCos = coneOneMinusCos(radius, distance2);
        vec3 sum = vec3(0.0);
        [[dont_unroll]] for (int k = 0; k < kCapPoints; ++k) {
            float down = (float(k) + 0.5) / float(kCapPoints) * oneMinusCos;  // 1 - cosine.
            float sine = sqrt(down * (2.0 - down));
            float angle = float(k) * kGoldenAngle + kTwoPi * u.x;
            vec3 wi = (1.0 - down) * centerLocal +
                      sine * (cos(angle) * cap.tangent + sin(angle) * cap.bitangent);
            for (int folded = 0; folded < 2; ++folded) {
                if (wi.z <= 0.0 || (folded == 1 && wi.z >= woLocal.z)) {
                    break;
                }
                vec3 h = normalize(woLocal + vec3(wi.xy, folded == 1 ? -wi.z : wi.z));
                float cosine = dot(woLocal, h);
                sum += ggxDistribution(h, alpha) *
                       (material.metallic * fresnelSchlick(material.albedo.rgb, cosine) +
                        vec3((1.0 - material.metallic) * fresnelDielectric(cosine, eta)));
            }
        }
        float visible = 1.0 / (1.0 + smithLambda(woLocal, alpha));  // G1(wo).
        result = (1.0 - narrow) *
                 (kTwoPi * oneMinusCos / float(kCapPoints) * visible / (4.0 * woLocal.z)) * sum;
    }
    if (narrow > 0.0) {
        // The light's edge as the chord from its center (unit sphere of directions), soft
        // over two cells' directions, at most a quarter of its radius (wider, it took in
        // more than the light).
        float edge = 2.0 * sin(0.5 * angularRadius);
        float soft = min(4.0 * alpha / float(kLobeGrid), 0.25 * angularRadius);
        vec3 sum = vec3(0.0);
        [[dont_unroll]] for (int k = 0; k < kLobeGrid * kLobeGrid; ++k) {
            vec2 cell = fract(u + vec2(0.7548777, 0.5698403) * float(k + 1));
            vec3 h = sampleGgxVisibleNormal(woLocal, alpha,
                                            (float(k % kLobeGrid) + cell.x) / float(kLobeGrid),
                                            (float(k / kLobeGrid) + cell.y) / float(kLobeGrid));
            float meets =
                clamp((edge - distance(reflectLocal(woLocal, h), centerLocal)) / soft + 0.5, 0.0,
                      1.0);
            float cosine = dot(woLocal, h);
            sum += meets * (material.metallic * fresnelSchlick(material.albedo.rgb, cosine) +
                            vec3((1.0 - material.metallic) * fresnelDielectric(cosine, eta)));
        }
        result += narrow / float(kLobeGrid * kLobeGrid) * sum;
    }
    return lights[i].emission * result;
}

/**
 * analyticLight times `albedo` -- with `glossy` also each light's sphereHighlight --
 * each light's share shadowed: by its shadow maps (soft; STARCORE_SPHERE_SHADOW_MAPS),
 * else by a ray to a point of it (sphereTransmittance: soft in expectation); a faint one
 * not at all. Both of its part above the surface's plane, as the closed form. `u`:
 * uniform, of the maps' taps' rotation (x) and of the point of the ray (turned every
 * frame: the recent mean, or the upscaler, averages them).
 */
vec3 shadowedLight(Material material, vec3 wo, SurfaceHit hit, vec2 u, vec3 albedo, bool glossy)
{
    vec3 total = vec3(0.0);
    for (uint i = 0u; visitLight(i, kLightSpheres); ++i) {
        total += sphereIrradiance(i, hit);
    }
    if (maxComponent(total) <= 0.0) {
        return vec3(0.0);
    }
    float faint = kFaintShare * luminance(total);
    vec3 diffuse = albedo * closedFormShare(material, wo, hit);
    vec3 sum = vec3(0.0);
    for (uint i = 0u; visitLight(i, kLightSpheres); ++i) {
        vec3 irradiance = sphereIrradiance(i, hit);
        vec3 light = diffuse * irradiance;
        bool bright = luminance(irradiance) > faint;
        if (glossy) {
            light += sphereHighlight(i, material, wo, hit, u, !bright);
        }
        if (bright) {
            Light sphere = lights[i];
#ifdef STARCORE_SPHERE_SHADOW_MAPS
            if (hasShadowMaps(i)) {
                light *= shadowVisibility(i, sphere.center, sphere.radius, hit.position,
                                          hit.geometricNormal, kTwoPi * u.x);
            } else
#endif
            {
                light *= sphereTransmittance(sphere, hit, u);
            }
        }
        sum += light;
    }
    return sum;
}

/**
 * Whether the paths' hits past their start are lit by the sphere lights in closed form
 * (path.glsl's tracePathFrom; the hybrid pipeline with DirectShadows::ShadowMaps).
 */
bool closedFormHitLights()
{
    return (frameData.options.x & kFlagDirectShadowMaps) != 0u;
}

/**
 * The light the sphere lights give a surface's diffuse lobe, and with `glossy` its
 * reflection, shadowed (by rays, unless shadow maps), seen from `wo`; `u` as
 * shadowedLight's.
 */
vec3 closedFormHitLight(SurfaceHit hit, Material material, vec3 wo, vec2 u, bool glossy)
{
    return shadowedLight(material, wo, hit, u, material.albedo.rgb, glossy);
}

#endif  // STARCORE_SPHERE_LIGHT_GLSL
