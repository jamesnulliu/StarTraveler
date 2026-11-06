// What a sun does inside a lens, on its way to the film (post.comp). These are drawn, not
// derived from the image: the sun's place on it is known (PostProcess), and shapes as crisp as
// a real lens's would not survive being blurred out of a bright image. How much of the sun is
// seen comes from the image (lens_visibility.comp), so a rock passing in front of it puts all
// of this out.
//
// Positions are in image heights from the center of the image (x scaled by the aspect ratio).

#ifndef LENS_GLSL
#define LENS_GLSL


float lensSquare(float x)
{
    return x * x;
}

vec3 lensSquare(vec3 x)
{
    return x * x;
}

/** Diffraction on the struts of the aperture: thin rays, not all as long. */
vec3 lensStarburst(vec2 fromSun)
{
    vec3 sum = vec3(0.0);
    for (int i = 0; i < 3; ++i) {
        float angle = 0.35 + float(i) * kPi / 3.0;
        vec2 direction = vec2(cos(angle), sin(angle));
        float along = abs(dot(fromSun, direction));
        float across = abs(fromSun.x * direction.y - fromSun.y * direction.x);
        float width = 0.0004 + 0.0015 * exp(-along / 0.3);
        float reach = i == 0 ? 0.24 : 0.14;
        // Red reaches farther than blue, as diffraction does.
        sum += exp(-along / (reach * vec3(1.14, 1.0, 0.86))) * exp(-lensSquare(across / width));
    }
    return sum * 0.8;
}

/** The long blue line of an anamorphic lens, through the sun and across the image. */
vec3 lensAnamorphic(vec2 fromSun)
{
    float across = abs(fromSun.y);
    float line = exp(-lensSquare(across / 0.0021)) + 0.14 * exp(-lensSquare(across / 0.014));
    return vec3(0.16, 0.42, 1.0) * (line * exp(-abs(fromSun.x) / 0.7) * 0.5);
}

/** A ring around the sun with the colors of its light split, faint and uneven. */
vec3 lensHalo(vec2 fromSun)
{
    const float radius = 0.34;
    float distanceToSun = length(fromSun);
    vec3 ring = exp(-lensSquare((vec3(distanceToSun) - radius * vec3(1.025, 1.0, 0.975)) / 0.012));
    ring += 0.3 * exp(-lensSquare((distanceToSun - radius) / 0.055));
    float angle = atan(fromSun.y, fromSun.x);
    return ring * (0.07 * (0.62 + 0.38 * sin(angle * 3.0 + 0.7) * sin(angle * 5.0 + 2.1)));
}

float lensHexagon(vec2 p, float radius)
{
    p = abs(p);
    return max(dot(p, vec2(0.8660254, 0.5)), p.y) - radius;
}

/**
 * Images of the aperture from reflections between the elements: strung along the line from
 * the sun through the center of the image, each a color of the coating that made it.
 */
vec3 lensGhosts(vec2 position, vec2 sun)
{
    // Where along the line (1: mirrored through the center), its size.
    const vec2 kPlaces[7] = vec2[7](vec2(-0.34, 0.028), vec2(0.27, 0.034), vec2(0.52, 0.088),
                                    vec2(0.74, 0.044), vec2(1.06, 0.135), vec2(1.42, 0.058),
                                    vec2(1.92, 0.15));
    const vec3 kTints[7] = vec3[7](vec3(0.35, 0.8, 1.0), vec3(0.3, 0.95, 0.6), vec3(0.22, 0.48, 1.0),
                                   vec3(0.72, 0.34, 1.0), vec3(0.12, 0.68, 0.82),
                                   vec3(1.0, 0.52, 0.22), vec3(0.34, 0.34, 1.0));
    vec3 sum = vec3(0.0);
    for (int i = 0; i < 7; ++i) {
        vec2 p = position + sun * kPlaces[i].x;
        float size = kPlaces[i].y;
        vec3 edges = vec3(lensHexagon(p, size * 1.03), lensHexagon(p, size),
                          lensHexagon(p, size * 0.97));
        float soft = 0.0025 + 0.012 * size;
        vec3 inside = vec3(1.0) - smoothstep(vec3(-soft), vec3(soft), edges);
        // Brighter towards the rim; the smallest are the strongest.
        vec3 rim = exp(-lensSquare(edges / (0.0035 + 0.02 * size)));
        float fill = 0.3 + 0.7 * smoothstep(0.0, size, length(p));
        sum += kTints[i] * (inside * fill + 0.45 * rim) * (0.0035 / (size + 0.05));
    }
    return sum * 1.8;
}

/** Specks of a layer: soft dots of random sizes, some of them in each cell. */
float lensSpecks(vec2 position, float cells, uint seed)
{
    vec2 scaled = position * cells;
    vec2 cell = floor(scaled);
    vec2 inCell = scaled - cell;
    float sum = 0.0;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            ivec2 at = ivec2(cell) + ivec2(x, y);
            uint hash = pcgHash(uint(at.x + 8192) + uint(at.y + 8192) * 16384u + seed);
            float u = float(hash & 1023u) / 1023.0;
            float v = float((hash >> 10u) & 1023u) / 1023.0;
            float w = float((hash >> 20u) & 1023u) / 1023.0;
            float radius = 0.04 + 0.26 * w * w * w;
            float d = length(inCell - vec2(float(x) + u, float(y) + v));
            sum += (0.25 + 0.75 * u) * (1.0 - smoothstep(radius * 0.5, radius, d));
        }
    }
    return sum;
}

/** Dust on the canopy, lit up by the sun near it: it can only be seen against the light. */
float lensDirt(vec2 position, vec2 fromSun)
{
    float distanceToSun = length(fromSun);
    float glow = exp(-lensSquare(distanceToSun / 0.3)) +
                 0.12 * exp(-lensSquare(distanceToSun / 0.8));
    if (glow < 0.004) {
        return 0.0;
    }
    float dust = 0.5 * lensSpecks(position, 11.0, 11u) + lensSpecks(position, 31.0, 29u);
    return glow * dust * 0.05;
}

/**
 * All of it: what is added to the exposed radiance at uv.
 * @param sun Its place: uv in xy.
 * @param aspect The image's width over its height.
 * @param strengths Ghosts, starburst, anamorphic streak, dirt.
 * @param halo The halo's.
 */
vec3 lensLight(vec2 uv, vec2 sun, float aspect, vec4 strengths, float halo)
{
    vec2 scale = vec2(aspect, 1.0);
    vec2 position = (uv - 0.5) * scale;
    vec2 sunPosition = (sun - 0.5) * scale;
    vec2 fromSun = position - sunPosition;
    vec3 sum = vec3(0.0);
    if (strengths.x > 0.0) {
        sum += strengths.x * lensGhosts(position, sunPosition);
    }
    if (strengths.y > 0.0) {
        sum += strengths.y * lensStarburst(fromSun);
    }
    if (strengths.z > 0.0) {
        sum += strengths.z * lensAnamorphic(fromSun);
    }
    if (halo > 0.0) {
        sum += halo * lensHalo(fromSun);
    }
    if (strengths.w > 0.0) {
        sum += vec3(strengths.w * lensDirt(position, fromSun));
    }
    return sum;
}

#endif
