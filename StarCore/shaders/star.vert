#version 460
// The space sky's stars (SkyMap::stars), vertex stage: each star is a quad (6 vertices, no
// vertex buffer) around its streak in screen space, as particle.vert's but at infinity --
// from where it is now to where it was `shutter` of a frame ago, seen by the previous
// camera (turning streaks the stars; moving does not). A star is a point: its light, its
// flux over the solid angle of a pixel, is spread over a small disc (particle.frag's
// falloff) so that it hardly shimmers as it crosses pixels. At depth 0: drawn only where
// no surface is (particle.frag tests the visibility buffer's depth).

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

layout(push_constant) uniform Draw
{
    float shutter;     // Streak length in frames (0: round).
    float brightness;  // SpaceSky::brightness.
    vec2 depthScale;   // particle.frag's.
    vec4 galacticToWorld[3];  // Rows.
}
draw;

layout(location = 0) flat out vec2 outHead;     // Pixels.
layout(location = 1) flat out vec2 outTrail;    // Pixels, towards where it was.
layout(location = 2) flat out float outRadius;  // Pixels.
layout(location = 3) flat out vec3 outColor;

// (along the streak, across it) of the two triangles.
const vec2 kCorners[6] = vec2[](vec2(0.0, -1.0), vec2(1.0, -1.0), vec2(1.0, 1.0),
                                vec2(0.0, -1.0), vec2(1.0, 1.0), vec2(0.0, 1.0));
// The falloff (1 - d^2)^2 of particle.frag integrated over a disc of radius 1 (pi / 3) and
// across a line (16 / 15).
const float kDiscIntegral = 1.0472;
const float kLineIntegral = 1.0667;
// Of the disc a star is spread over, pixels: wide enough that the sum of its pixels changes
// by a few percent as it moves (crossing pixels), narrow enough to stay a point.
const float kRadius = 1.6;

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
        outRadius = 1.0;
        outColor = vec3(0.0);
        return;
    }
    vec3 tail = projectDirection(direction, frameData.previousForward.xyz,
                                 frameData.previousRight.xyz, frameData.previousUp.xyz);
    vec2 trail = tail.z > 1e-3 ? (tail.xy - head.xy) * draw.shutter : vec2(0.0);
    float streak = length(trail);

    // The solid angle of the pixel it falls in: a pixel's side on the view plane at depth 1
    // squared, times cos^3 off the axis.
    float side = 2.0 * length(frameData.cameraUp.xyz) / size.y;
    float solidAngle = side * side * head.z * head.z * head.z;
    // The falloff's peak such that its pixels sum to the flux over that solid angle.
    float area = kDiscIntegral * kRadius * kRadius + kLineIntegral * kRadius * streak;
    float peak = star.flux * draw.brightness / (solidAngle * area);

    vec2 along = streak > 1e-3 ? trail / streak : vec2(1.0, 0.0);
    vec2 across = vec2(-along.y, along.x);
    vec2 start = head.xy - along * kRadius;
    vec2 end = head.xy + trail + along * kRadius;
    vec2 pixel = mix(start, end, corner.x) + across * (kRadius * corner.y);
    // Pixels (+y down) to Vulkan NDC (+y down); depth 0: at infinity (reversed depth).
    gl_Position = vec4(pixel / size * 2.0 - 1.0, 0.0, 1.0);
    outHead = head.xy;
    outTrail = trail;
    outRadius = kRadius;
    outColor = star.color * peak;
}
