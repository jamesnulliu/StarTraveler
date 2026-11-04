// Planets in the sky: port of planetRadiance (starcore/render/planet.hpp, src/render/
// planet.cpp) -- keep them in sync. Each is shaded in its own frame (its spin axis along y)
// in units of its radius, from the world origin.

#ifndef STARCORE_PLANET_GLSL
#define STARCORE_PLANET_GLSL

#extension GL_EXT_control_flow_attributes : require

#include "common.glsl"
#include "noise.glsl"

const uint kMaxPlanets = 4u;

/** gpu::PlanetRecord (160 bytes). */
struct Planet
{
    vec4 frame[3];           // Rows of the rotation from world to the planet's frame.
    vec3 origin;             // The world origin in that frame, in radii (planetOrigin).
    uint kind;               // PlanetKind.
    vec3 sun;                // Towards its sun, in that frame.
    uint seed;
    vec3 sunIrradiance;
    float atmosphereHeight;  // Scale height, in radii.
    vec3 atmosphere;         // Optical depth straight up from the surface (planetAtmosphere).
    float haze;
    vec3 color;
    float ringInner;
    vec3 secondaryColor;
    float ringOuter;
    vec3 ringColor;
    float ringOpacity;
};

const uint kPlanetRocky = 0u;
const uint kPlanetTerrestrial = 1u;
const uint kPlanetGas = 2u;
const uint kPlanetIce = 3u;

const float kAtmosphereTop = 8.0;
const int kAtmosphereSteps = 16;
const float kHazeForward = 0.8;
const float kRingForward = 0.6;
const float kGlintExponent = 400.0;
const float kGlintReflectance = 0.05;
const float kRelief = 0.25;
const float kLn2 = 0.693147181;

float maxOf(vec3 v)
{
    return max(v.x, max(v.y, v.z));
}

float rayleighPhase(float cosine)
{
    return 3.0 / (16.0 * kPi) * (1.0 + cosine * cosine);
}

float henyeyGreenstein(float cosine, float g)
{
    float d = 1.0 + g * g - 2.0 * g * cosine;
    return (1.0 - g * g) / (4.0 * kPi * d * sqrt(d));
}

/** Schueler's approximation of Chapman's function times the density (planet.cpp's chapman). */
float chapman(float x, float h, float mu)
{
    float c = sqrt(x + h);
    if (mu >= 0.0) {
        return c / (c * mu + 1.0) * exp2(-h);
    }
    float x0 = sqrt(1.0 - mu * mu) * (x + h);
    float c0 = sqrt(x0);
    return 2.0 * c0 * exp2(min(x - x0, 100.0)) - c / (1.0 - c * mu) * exp2(-h);
}

vec3 sunTransmittance(vec3 depth, float height, vec3 p, vec3 sun)
{
    float r = length(p);
    float h = (r - 1.0) / (height * kLn2);
    return exp(-depth * chapman(1.0 / (height * kLn2), h, dot(p, sun) / r));
}

/** The rings at a radius: opacity (w) and color (xyz). */
vec4 ringAt(Planet planet, float r)
{
    float x = (r - planet.ringInner) / (planet.ringOuter - planet.ringInner);
    if (x <= 0.0 || x >= 1.0) {
        return vec4(0.0);
    }
    uint seed = planet.seed;
    float broad = gradientNoise(vec3(r * 9.0, 0.5, 0.5), seed + 11u);
    float ringlets = gradientNoise(vec3(r * 37.0, 0.5, 0.5), seed + 12u);
    float fine = gradientNoise(vec3(r * 140.0, 0.5, 0.5), seed + 13u);
    float density = clamp(0.6 + 0.9 * broad + 0.45 * ringlets + 0.25 * fine, 0.0, 1.0);
    float gap = 0.55 + 0.2 * float(pcgHash(seed + 14u) >> 8u) * (1.0 / 16777216.0);
    float offGap = (x - gap) / 0.02;
    density *= 1.0 - 0.92 * exp(-offGap * offGap);
    density *= 0.25 + 0.75 * smoothstep(0.05, 0.3, x);
    density *= smoothstep(0.0, 0.02, x) * (1.0 - smoothstep(0.98, 1.0, x));
    return vec4(planet.ringColor * (0.8 + 0.4 * smoothstep(-0.4, 0.4, broad)),
                planet.ringOpacity * density);
}

struct PlanetSurface
{
    vec3 albedo;
    float water;  // Open ocean (its glint).
    vec3 slope;   // Of its relief (craters): tilts the shading normal.
};

float unitFloat(uint hash)
{
    return float(hash >> 8u) * (1.0 / 16777216.0);
}

/** Craters of one size at unit point p (planet.cpp's addCraters). */
void addCraters(vec3 p, float frequency, uint seed, float strength, inout PlanetSurface surface)
{
    vec3 q = p * frequency;
    vec3 cell = floor(q);
    [[dont_unroll]] for (int i = 0; i < 27; ++i) {
        vec3 corner = cell + vec3(float(i % 3 - 1), float(i / 3 % 3 - 1), float(i / 9 - 1));
        ivec3 integer = ivec3(corner);
        uint h0 = pcgHash(seed + pcgHash(uint(integer.x) +
                                         pcgHash(uint(integer.y) + pcgHash(uint(integer.z)))));
        uint h1 = pcgHash(h0);
        uint h2 = pcgHash(h1);
        uint h3 = pcgHash(h2);
        vec3 center =
            corner + vec3(0.15) + 0.7 * vec3(unitFloat(h0), unitFloat(h1), unitFloat(h2));
        float size = unitFloat(h3);
        float radius = 0.12 + 0.3 * size * size;
        vec3 toPoint = q - center;
        float dist = length(toPoint);
        float d = dist / radius;
        if (d >= 1.6 || dist <= 0.0) {
            continue;
        }
        float offRim = (d - 1.0) / 0.18;
        float rim = exp(-offRim * offRim);
        bool fresh = (h3 & 0xFFu) < 30u;
        float albedo = mix(0.86, 1.0, smoothstep(0.55, 1.0, d)) * (1.0 + 0.2 * rim);
        if (fresh) {
            albedo *= 1.0 + 0.6 * exp(-0.8 * d * d);
        }
        surface.albedo *= mix(1.0, albedo, strength);
        float rise = (d < 1.0 ? 1.2 * d : 0.0) - 0.35 * rim * 2.0 * offRim / 0.18;
        surface.slope += (strength * rise / dist) * toPoint;
    }
}

PlanetSurface rockySurface(Planet planet, vec3 p)
{
    float regions = fbm(p * 1.3, 4, planet.seed);
    float detail = fbm(p * 6.0, 4, planet.seed + 1u);
    float highlands = smoothstep(-0.06, 0.06, regions + 0.2 * detail);
    PlanetSurface surface = PlanetSurface(
        mix(planet.secondaryColor, planet.color, highlands) * (1.0 + 0.3 * detail), 0.0,
        vec3(0.0));
    float strength = mix(0.35, 1.0, highlands);
    addCraters(p, 5.0, planet.seed + 20u, strength, surface);
    addCraters(p, 11.0, planet.seed + 21u, strength, surface);
    addCraters(p, 23.0, planet.seed + 22u, strength, surface);
    return surface;
}

PlanetSurface terrestrialSurface(Planet planet, vec3 p)
{
    uint seed = planet.seed;
    vec3 warp = vec3(gradientNoise(p * 1.5, seed + 3u),
                     gradientNoise(p * 1.5 + vec3(17.1), seed + 3u),
                     gradientNoise(p * 1.5 + vec3(31.7), seed + 3u));
    float height = fbm(p * 1.7 + 0.5 * warp, 6, seed);
    float land = smoothstep(0.04, 0.06, height);
    float dry = smoothstep(-0.15, 0.25, gradientNoise(p * 2.5, seed + 4u));
    vec3 ground = mix(planet.color, min(planet.color * vec3(1.6, 1.3, 0.85), vec3(1.0)), dry);
    ground *= 0.85 + 0.8 * max(height - 0.05, 0.0);
    vec3 sea = planet.secondaryColor * (0.8 + 0.8 * smoothstep(-0.12, 0.05, height));
    float ice = smoothstep(0.76, 0.84, abs(p.y) + 0.08 * fbm(p * 4.0, 3, seed + 5u));
    float belts = 0.05 * cos(p.y * 9.0);
    float cloud = smoothstep(0.03, 0.15, fbm(p * 3.4 + 0.6 * warp, 6, seed + 6u) + belts);
    vec3 albedo = mix(mix(sea, ground, land), vec3(0.82, 0.85, 0.88), ice);
    albedo = mix(albedo, vec3(0.9), cloud);
    return PlanetSurface(albedo, (1.0 - land) * (1.0 - ice) * (1.0 - cloud), vec3(0.0));
}

PlanetSurface gasSurface(Planet planet, vec3 p)
{
    uint seed = planet.seed;
    float swirl = fbm(p * 3.0, 4, seed + 5u);
    float latitude = p.y + 0.04 * swirl;
    float belts = gradientNoise(vec3(0.5, latitude * 7.0, 0.5), seed);
    float streaks = fbm(vec3(p.x * 1.5, latitude * 22.0, p.z * 1.5) + vec3(1.5 * swirl), 4,
                        seed + 6u);
    float t = smoothstep(-0.25, 0.25, belts + 0.45 * streaks);
    vec3 albedo = mix(planet.secondaryColor, planet.color, t);
    albedo *= 1.0 + 0.3 * gradientNoise(vec3(0.5, latitude * 45.0, 0.5), seed + 7u);
    float polar = smoothstep(0.7, 0.97, abs(p.y));
    return PlanetSurface(mix(albedo, planet.secondaryColor * 0.75, 0.6 * polar), 0.0,
                         vec3(0.0));
}

PlanetSurface iceSurface(Planet planet, vec3 p)
{
    float base = fbm(p * 2.0, 4, planet.seed);
    float cracks = ridged(p * 3.5, 4, planet.seed + 1u);
    vec3 albedo = mix(planet.color, mix(planet.color, planet.secondaryColor, 0.4),
                      smoothstep(-0.1, 0.3, base));
    albedo = mix(albedo, planet.secondaryColor, 0.85 * smoothstep(0.75, 0.92, cracks));
    PlanetSurface surface = PlanetSurface(
        albedo * (0.92 + 0.16 * (0.5 + fbm(p * 12.0, 2, planet.seed + 2u))), 0.0, vec3(0.0));
    addCraters(p, 9.0, planet.seed + 20u, 0.3, surface);
    return surface;
}

PlanetSurface surfaceAt(Planet planet, vec3 p)
{
    PlanetSurface surface;
    if (planet.kind == kPlanetTerrestrial) {
        surface = terrestrialSurface(planet, p);
    } else if (planet.kind == kPlanetGas) {
        surface = gasSurface(planet, p);
    } else if (planet.kind == kPlanetIce) {
        surface = iceSurface(planet, p);
    } else {
        surface = rockySurface(planet, p);
    }
    surface.albedo = min(surface.albedo, vec3(1.0));
    return surface;
}

float ringShadow(Planet planet, vec3 p, vec3 sun)
{
    if (planet.ringOuter <= planet.ringInner || abs(sun.y) < 1e-4) {
        return 1.0;
    }
    float t = -p.y / sun.y;
    if (t <= 0.0) {
        return 1.0;
    }
    vec3 crossing = p + t * sun;
    return 1.0 - ringAt(planet, sqrt(crossing.x * crossing.x + crossing.z * crossing.z)).w;
}

vec3 planetSurfaceRadiance(Planet planet, vec3 n, vec3 d, vec3 sun)
{
    float muSun = dot(n, sun);
    if (muSun <= 0.0) {
        return vec3(0.0);
    }
    vec3 light = planet.sunIrradiance * ringShadow(planet, n, sun);
    if (maxOf(planet.atmosphere) > 0.0) {
        light *= sunTransmittance(planet.atmosphere, planet.atmosphereHeight, n, sun);
    }
    PlanetSurface surface = surfaceAt(planet, n);
    vec3 normal = normalize(n - kRelief * (surface.slope - dot(surface.slope, n) * n));
    float muLit = max(dot(normal, sun), 0.0);
    float muView = max(-dot(normal, d), 0.0);
    bool regolith = planet.kind == kPlanetRocky || planet.kind == kPlanetIce;
    float shading = regolith ? 2.0 * muLit / (muLit + muView + 1e-4) : muLit;
    vec3 radiance = surface.albedo / kPi * light * shading;
    vec3 halfway = sun - d;
    if (surface.water > 0.0 && dot(halfway, halfway) > 1e-12) {
        float glint = (kGlintExponent + 8.0) / (8.0 * kPi) *
                      pow(max(dot(n, normalize(halfway)), 0.0), kGlintExponent);
        radiance += light * (surface.water * kGlintReflectance * glint * muSun);
    }
    return radiance;
}

vec3 planetRingRadiance(Planet planet, vec4 ring, vec3 p, vec3 d, vec3 sun)
{
    float along = dot(p, sun);
    float offAxis = sqrt(max(dot(p, p) - along * along, 0.0));
    float shadow = along < 0.0 ? smoothstep(0.98, 1.02, offAxis) : 1.0;
    vec3 light = planet.sunIrradiance * shadow;
    float muSun = abs(sun.y);
    float muView = abs(d.y);
    float shading = 2.0 * muSun / (muSun + muView + 1e-4);
    bool lit = sun.y * d.y < 0.0;
    float alpha = ring.w;
    float seen = lit ? shading / kPi
                     : (1.0 - alpha) *
                           (shading / kPi + 0.6 * henyeyGreenstein(dot(d, sun), kRingForward));
    return ring.xyz * light * (alpha * seen);
}

/** What is seen from the origin towards a unit world direction: the planet over `behind`. */
vec3 planetRadiance(Planet planet, vec3 direction, vec3 behind)
{
    vec3 o = planet.origin;
    vec3 d = vec3(dot(planet.frame[0].xyz, direction), dot(planet.frame[1].xyz, direction),
                  dot(planet.frame[2].xyz, direction));
    vec3 sun = planet.sun;
    vec3 depth = planet.atmosphere;
    float height = planet.atmosphereHeight;
    bool atmosphere = maxOf(depth) > 0.0;
    bool rings = planet.ringOuter > planet.ringInner;
    float top = atmosphere ? 1.0 + kAtmosphereTop * height : 1.0;
    float bound = max(top, rings ? planet.ringOuter : 0.0);

    float b = dot(o, d);
    vec3 q = o - b * d;
    float q2 = dot(q, q);
    if (q2 >= bound * bound || (b > 0.0 && dot(o, o) > bound * bound)) {
        return behind;
    }
    bool surface = q2 < 1.0 && b < 0.0;
    float front = surface ? -sqrt(1.0 - q2) : 0.0;

    vec3 radiance = behind;
    if (surface) {
        radiance = planetSurfaceRadiance(planet, normalize(q + front * d), d, sun);
    }
    vec4 ring = vec4(0.0);
    vec3 ringPoint = vec3(0.0);
    float ringAlong = 0.0;
    if (rings && abs(d.y) > 1e-6) {
        float t = -o.y / d.y;
        if (t > 0.0) {
            ringPoint = o + t * d;
            ring = ringAt(planet, sqrt(ringPoint.x * ringPoint.x + ringPoint.z * ringPoint.z));
            ringAlong = t + b;
        }
    }
    bool ringBehind = ring.w > 0.0 && ringAlong > front;
    if (ringBehind && !surface) {
        radiance = planetRingRadiance(planet, ring, ringPoint, d, sun) + (1.0 - ring.w) * radiance;
    }
    if (atmosphere && q2 < top * top) {
        float enter = max(-sqrt(top * top - q2), b);
        float exit = surface ? front : -enter;
        float stepLength = (exit - enter) / float(kAtmosphereSteps);
        float cosine = dot(d, sun);
        float phase =
            mix(rayleighPhase(cosine), henyeyGreenstein(cosine, kHazeForward), planet.haze);
        vec3 transmittance = vec3(1.0);
        vec3 scattered = vec3(0.0);
        [[dont_unroll]] for (int i = 0; i < kAtmosphereSteps; ++i) {
            vec3 p = q + (enter + (float(i) + 0.5) * stepLength) * d;
            float altitude = (length(p) - 1.0) / height;
            vec3 stepTransmittance = exp(-depth * (exp(-altitude) * stepLength / height));
            scattered += transmittance * (vec3(1.0) - stepTransmittance) *
                         sunTransmittance(depth, height, p, sun);
            transmittance *= stepTransmittance;
        }
        radiance = radiance * transmittance + planet.sunIrradiance * (scattered * phase);
    }
    if (ring.w > 0.0 && !ringBehind) {
        radiance = planetRingRadiance(planet, ring, ringPoint, d, sun) + (1.0 - ring.w) * radiance;
    }
    return radiance;
}

#endif  // STARCORE_PLANET_GLSL
