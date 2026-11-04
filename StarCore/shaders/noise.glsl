// Gradient noise: port of src/render/noise.hpp -- keep them in sync (the same lattice
// hashes, so CPU and GPU planets match).

#ifndef STARCORE_NOISE_GLSL
#define STARCORE_NOISE_GLSL

#extension GL_EXT_control_flow_attributes : require

#include "common.glsl"

/** One of the 12 cube edge gradients (Perlin's improved noise) dotted with (x, y, z). */
float gradientDot(uint hash, float x, float y, float z)
{
    uint h = hash & 15u;
    float u = h < 8u ? x : y;
    float v = h < 4u ? y : (h == 12u || h == 14u ? x : z);
    return ((h & 1u) == 0u ? u : -u) + ((h & 2u) == 0u ? v : -v);
}

float noiseFade(float t)
{
    return t * t * t * (t * (t * 6.0 - 15.0) + 10.0);
}

/** a to b as noise.hpp's lerpFast (not mix: the same rounding). */
float lerpFast(float a, float b, float t)
{
    return a + t * (b - a);
}

/** Gradient noise, about -1 to 1 (standard deviation some 0.25). */
float gradientNoise(vec3 p, uint seed)
{
    vec3 cell = floor(p);
    ivec3 integer = ivec3(cell);
    uint x = uint(integer.x);  // Two's complement, as static_cast<std::uint32_t>.
    uint y = uint(integer.y);
    uint z = uint(integer.z);
    vec3 f = p - cell;
    uint seedHash = pcgHash(seed);
    uint z0 = pcgHash(z + seedHash);
    uint z1 = pcgHash(z + 1u + seedHash);
    uint y00 = pcgHash(y + z0);  // (dy, dz) = (0, 0)
    uint y10 = pcgHash(y + 1u + z0);
    uint y01 = pcgHash(y + z1);
    uint y11 = pcgHash(y + 1u + z1);
    float c000 = gradientDot(pcgHash(x + y00), f.x, f.y, f.z);
    float c100 = gradientDot(pcgHash(x + 1u + y00), f.x - 1.0, f.y, f.z);
    float c010 = gradientDot(pcgHash(x + y10), f.x, f.y - 1.0, f.z);
    float c110 = gradientDot(pcgHash(x + 1u + y10), f.x - 1.0, f.y - 1.0, f.z);
    float c001 = gradientDot(pcgHash(x + y01), f.x, f.y, f.z - 1.0);
    float c101 = gradientDot(pcgHash(x + 1u + y01), f.x - 1.0, f.y, f.z - 1.0);
    float c011 = gradientDot(pcgHash(x + y11), f.x, f.y - 1.0, f.z - 1.0);
    float c111 = gradientDot(pcgHash(x + 1u + y11), f.x - 1.0, f.y - 1.0, f.z - 1.0);
    float u = noiseFade(f.x);
    float v = noiseFade(f.y);
    float w = noiseFade(f.z);
    float x00 = lerpFast(c000, c100, u);
    float x10 = lerpFast(c010, c110, u);
    float x01 = lerpFast(c001, c101, u);
    float x11 = lerpFast(c011, c111, u);
    return lerpFast(lerpFast(x00, x10, v), lerpFast(x01, x11, v), w);
}

/** Fractal sum of octaves of noise, about -1 to 1 (most of it within +-0.5). */
float fbm(vec3 p, int octaves, uint seed)
{
    float sum = 0.0;
    float amplitude = 0.5;
    float total = 0.0;
    [[dont_unroll]] for (int octave = 0; octave < octaves; ++octave) {
        sum += amplitude * gradientNoise(p, seed + uint(octave) * 7919u);
        total += amplitude;
        p *= 2.03;
        amplitude *= 0.5;
    }
    return sum / total;
}

/** Sharp ridges (filaments) of octaves of noise, 0 to 1. */
float ridged(vec3 p, int octaves, uint seed)
{
    float sum = 0.0;
    float amplitude = 0.5;
    float total = 0.0;
    [[dont_unroll]] for (int octave = 0; octave < octaves; ++octave) {
        float ridge = 1.0 - abs(gradientNoise(p, seed + uint(octave) * 104729u));
        sum += amplitude * ridge * ridge * ridge;
        total += amplitude;
        p *= 2.03;
        amplitude *= 0.5;
    }
    return sum / total;
}

#endif  // STARCORE_NOISE_GLSL
