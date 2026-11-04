// The sky: what rays that escape the scene see. Port of skyRadiance and SkyMap::radiance
// (starcore/render/render_scene.hpp, sky.hpp) -- keep them in sync. The scene's sky record
// (gpu::SkyRecord, GpuScene::skyAddress()) holds a constant color; for a space sky, the
// address of its map: an equirectangular RGB9E5 map in the galactic frame; and the planets
// in front of them (planet.glsl), farthest first.

#ifndef STARCORE_SKY_GLSL
#define STARCORE_SKY_GLSL

#extension GL_EXT_buffer_reference : require
#extension GL_EXT_buffer_reference_uvec2 : require
#extension GL_EXT_control_flow_attributes : require

#include "common.glsl"
#include "planet.glsl"

/** gpu::SkyRecord (752 bytes). */
layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer SkyRecord
{
    vec4 color;        // rgb: constant radiance; w: the map's brightness
    vec4 ambient;      // rgb: the mean radiance over all directions
    vec4 galactic[3];  // Rows of the rotation from world to galactic directions.
    uvec2 map;         // Address of the map's texels (0: no map).
    uvec2 size;        // Of the map.
    uint planetCount;
    uint padding0;
    uint padding1;
    uint padding2;
    Planet planets[kMaxPlanets];
};

layout(buffer_reference, std430, buffer_reference_align = 4) readonly buffer SkyTexels
{
    uint values[];
};

vec3 decodeRgb9e5(uint packed)
{
    float scale = exp2(float(int(packed >> 27) - 15 - 9));
    return vec3(float(packed & 0x1FFu), float((packed >> 9) & 0x1FFu),
                float((packed >> 18) & 0x1FFu)) *
           scale;
}

/** Bilinear lookup of a sky map towards a unit galactic direction (SkyMap::radiance). */
vec3 skyMapRadiance(uvec2 address, uvec2 size, vec3 g)
{
    SkyTexels texels = SkyTexels(address);
    float longitude = abs(g.x) + abs(g.z) > 0.0 ? atan(g.z, g.x) : 0.0;
    float latitude = asin(clamp(g.y, -1.0, 1.0));
    vec2 position = vec2((longitude / kTwoPi + 0.5) * float(size.x) - 0.5,
                         (0.5 - latitude / kPi) * float(size.y) - 0.5);
    vec2 cell = floor(position);
    vec2 f = position - cell;
    int w = int(size.x);
    int h = int(size.y);
    int x0 = int(cell.x);
    if (x0 < 0) {
        x0 += w;
    }
    int x1 = x0 + 1 == w ? 0 : x0 + 1;
    int y0 = max(int(cell.y), 0);
    int y1 = min(int(cell.y) + 1, h - 1);
    vec3 t00 = decodeRgb9e5(texels.values[y0 * w + x0]);
    vec3 t10 = decodeRgb9e5(texels.values[y0 * w + x1]);
    vec3 t01 = decodeRgb9e5(texels.values[y1 * w + x0]);
    vec3 t11 = decodeRgb9e5(texels.values[y1 * w + x1]);
    return t00 * ((1.0 - f.x) * (1.0 - f.y)) + t10 * (f.x * (1.0 - f.y)) +
           t01 * ((1.0 - f.x) * f.y) + t11 * (f.x * f.y);
}

/** Radiance of the sky of a sky record towards a direction (skyRadiance). */
vec3 skyRadianceOf(uvec2 record, vec3 direction)
{
    SkyRecord sky = SkyRecord(record);
    vec3 radiance = sky.color.rgb;
    if (sky.map != uvec2(0u)) {
        vec3 g = normalize(vec3(dot(sky.galactic[0].xyz, direction),
                                dot(sky.galactic[1].xyz, direction),
                                dot(sky.galactic[2].xyz, direction)));
        radiance += skyMapRadiance(sky.map, sky.size, g) * sky.color.w;
    }
    uint planets = min(sky.planetCount, kMaxPlanets);
    [[dont_unroll]] for (uint i = 0u; i < planets; ++i) {
        radiance = planetRadiance(sky.planets[i], direction, radiance);
    }
    return radiance;
}

/** The sky's mean radiance over all directions (skyAmbient). */
vec3 skyAmbientOf(uvec2 record)
{
    return SkyRecord(record).ambient.rgb;
}

#endif  // STARCORE_SKY_GLSL
