#version 460
// The space sky's stars (SkyMap::stars), vertex stage: each star is a quad (6 vertices, no
// vertex buffer) around its streak in screen space, as particle.vert's but at infinity --
// from where it is now to where it was `shutter` of a frame ago, seen by the previous
// camera (turning streaks the stars; moving does not). A star is a point seen through a
// lens: its light, its flux over the solid angle of a pixel, spread by the lens's point
// spread function (star.frag) -- most of it over a small disc, so that it hardly shimmers
// as it crosses pixels, the rest over a halo whose wings fall as the cube of the distance,
// drawn as far as it shows at the exposure (bright stars look larger, faint ones are
// points). At depth 0: drawn only where no surface is (star.frag tests the visibility
// buffer's depth).

#extension GL_GOOGLE_include_directive : require

#define STARCORE_NO_RAY_QUERY
#include "hybrid.glsl"

/** SkyStar (32 bytes). */
struct Star
{
    vec3 direction;  // Galactic frame.
    float flux;      // Radiance x steradians.
    vec3 color;      // Luminance 1.
    float magnitude;
};
layout(set = 0, binding = 3, std430) readonly buffer Stars
{
    Star stars[];
};
layout(set = 0, binding = 2, std430) readonly buffer Exposure  // exposure_adapt.comp's.
{
    float exposure;
}
exposureState;

layout(push_constant) uniform Draw
{
    float shutter;     // Streak length in frames (0: round).
    float brightness;  // SpaceSky::brightness.
    vec2 depthScale;   // particle.frag's.
    vec4 galacticToWorld[3];  // Rows.
}
draw;

layout(location = 0) flat out vec2 outHead;   // Pixels.
layout(location = 1) flat out vec2 outTrail;  // Pixels, towards where it was.
// The core's radius, the halo's scale, how far it is drawn (pixels), its falloff there.
layout(location = 2) flat out vec4 outShape;
layout(location = 3) flat out vec3 outCore;  // Radiance at the core's middle.
layout(location = 4) flat out vec3 outHalo;  // The halo's (at its middle, before the core's).

// (along the streak, across it) of the two triangles.
const vec2 kCorners[6] = vec2[](vec2(0.0, -1.0), vec2(1.0, -1.0), vec2(1.0, 1.0),
                                vec2(0.0, -1.0), vec2(1.0, 1.0), vec2(0.0, 1.0));
// The core's falloff (1 - d^2)^2 integrated over a disc of radius 1 (pi / 3) and across a
// line (16 / 15).
const float kDiscIntegral = 1.0472;
const float kLineIntegral = 1.0667;
// Of the core, pixels: wide enough that the sum of its pixels changes by a few percent as it
// moves (crossing pixels), narrow enough to stay a point.
const float kRadius = 1.6;
// The halo, (1 + (d / scale)^2)^-1.5 (Moffat's profile, beta 1.5): its share of the light,
// its scale, the farthest it is drawn (pixels).
const float kHaloShare = 0.12;
const float kHaloScale = 1.0;
const float kMaxHalo = 48.0;
// Exposed radiance below which nothing shows (AgX's black, some 2^-12.5).
const float kInvisible = 2e-4;

/** Continuous pixel position (xy) and depth (z, the cosine to the view axis) of a direction. */
vec3 projectDirection(vec3 direction, vec3 forward, vec3 right, vec3 up)
{
    float z = dot(direction, forward);
    float ndcX = dot(direction, right) / (dot(right, right) * z);
    float ndcY = dot(direction, up) / (dot(up, up) * z);
    vec2 size = viewportSize();
    return vec3((ndcX + 1.0) * 0.5 * size.x, (1.0 - ndcY) * 0.5 * size.y, z);
}

void main()
{
    Star star = stars[gl_VertexIndex / 6];
    vec2 corner = kCorners[gl_VertexIndex % 6];
    vec3 direction = normalize(vec3(dot(draw.galacticToWorld[0].xyz, star.direction),
                                    dot(draw.galacticToWorld[1].xyz, star.direction),
                                    dot(draw.galacticToWorld[2].xyz, star.direction)));
    vec3 head = projectDirection(direction, frameData.cameraForward.xyz,
                                 frameData.cameraRight.xyz, frameData.cameraUp.xyz);
    vec2 size = viewportSize();
    const float kMargin = 64.0;
    if (head.z <= 1e-3 || any(lessThan(head.xy, vec2(-kMargin))) ||
        any(greaterThan(head.xy, size + kMargin))) {
        gl_Position = vec4(2.0, 2.0, 0.0, 1.0);  // Off screen, zero area.
        outHead = vec2(0.0);
        outTrail = vec2(0.0);
        outShape = vec4(1.0, 1.0, 1.0, 0.0);
        outCore = vec3(0.0);
        outHalo = vec3(0.0);
        return;
    }
    vec3 tail = projectDirection(direction, frameData.previousForward.xyz,
                                 frameData.previousRight.xyz, frameData.previousUp.xyz);
    vec2 trail = tail.z > 1e-3 ? (tail.xy - head.xy) * draw.shutter : vec2(0.0);
    float streak = length(trail);

    // The solid angle of the pixel it falls in: a pixel's side on the view plane at depth 1
    // squared, times cos^3 off the axis. Its light over it: what its pixels sum to.
    float side = 2.0 * length(frameData.cameraUp.xyz) / size.y;
    float solidAngle = side * side * head.z * head.z * head.z;
    float light = star.flux * draw.brightness / solidAngle;
    float coreArea = kDiscIntegral * kRadius * kRadius + kLineIntegral * kRadius * streak;
    float corePeak = light * (1.0 - kHaloShare) / coreArea;
    // The halo as far as its wings (some (scale / d)^3 of its peak) show, exposed.
    float s2 = kHaloScale * kHaloScale;
    float exposedHalo = light * kHaloShare / (kTwoPi * s2) * exposureState.exposure;
    float haloRadius =
        clamp(kHaloScale * pow(max(exposedHalo / kInvisible, 1.0), 1.0 / 3.0), kRadius, kMaxHalo);
    // Its integral over a disc of that radius, and across a line as wide.
    float edge = inversesqrt(1.0 + haloRadius * haloRadius / s2);
    float haloArea = kTwoPi * s2 * (1.0 - edge) + 2.0 * haloRadius * edge * streak;
    float haloPeak = light * kHaloShare / haloArea;

    vec2 along = streak > 1e-3 ? trail / streak : vec2(1.0, 0.0);
    vec2 across = vec2(-along.y, along.x);
    float reach = max(kRadius, haloRadius);
    vec2 start = head.xy - along * reach;
    vec2 end = head.xy + trail + along * reach;
    vec2 pixel = mix(start, end, corner.x) + across * (reach * corner.y);
    // Pixels (+y down) to Vulkan NDC (+y down); depth 0: at infinity (reversed depth).
    gl_Position = vec4(pixel / size * 2.0 - 1.0, 0.0, 1.0);
    outHead = head.xy;
    outTrail = trail;
    outShape = vec4(kRadius, kHaloScale, haloRadius, edge * edge * edge);
    outCore = star.color * corePeak;
    outHalo = star.color * haloPeak;
}
