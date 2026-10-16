// The sphere lights' light on a surface's diffuse lobe in closed form, shadowed
// without noise: the hybrid renderer's direct layer (direct.comp, with shadow maps
// where STARCORE_SPHERE_SHADOW_MAPS is defined: shadows.glsl included first) and its
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

/**
 * analyticLight, each light's share shadowed: by its shadow maps (soft;
 * STARCORE_SPHERE_SHADOW_MAPS), else by a ray to a point of it (sphereTransmittance:
 * soft in expectation); a faint one not at all. Both of its part above the surface's
 * plane, as the closed form. `u`: uniform, of the maps' taps' rotation (x) and of the
 * point of the ray (turned every frame: the recent mean, or the upscaler, averages
 * them).
 */
vec3 shadowedLight(Material material, vec3 wo, SurfaceHit hit, vec2 u)
{
    vec3 total = vec3(0.0);
    for (uint i = 0u; visitLight(i, kLightSpheres); ++i) {
        total += sphereIrradiance(i, hit);
    }
    if (maxComponent(total) <= 0.0) {
        return vec3(0.0);
    }
    float faint = kFaintShare * luminance(total);
    vec3 sum = vec3(0.0);
    for (uint i = 0u; visitLight(i, kLightSpheres); ++i) {
        vec3 irradiance = sphereIrradiance(i, hit);
        if (luminance(irradiance) > faint) {
            Light light = lights[i];
#ifdef STARCORE_SPHERE_SHADOW_MAPS
            if (hasShadowMaps(i)) {
                irradiance *= shadowVisibility(i, light.center, light.radius, hit.position,
                                               hit.geometricNormal, kTwoPi * u.x);
            } else
#endif
            {
                irradiance *= sphereTransmittance(light, hit, u);
            }
        }
        sum += irradiance;
    }
    return closedFormShare(material, wo, hit) * sum;
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
 * The light the sphere lights give a surface's diffuse lobe, shadowed (by rays, unless
 * shadow maps), seen from `wo`; `u` as shadowedLight's.
 */
vec3 closedFormHitLight(SurfaceHit hit, Material material, vec3 wo, vec2 u)
{
    return material.albedo.rgb * shadowedLight(material, wo, hit, u);
}

#endif  // STARCORE_SPHERE_LIGHT_GLSL
