#version 460
// Particles, vertex stage: each particle is a quad (6 vertices, no vertex
// buffer) around its streak in screen space -- from where it is now to where
// it was `shutter` of a frame ago, seen by the previous camera -- at its
// depth, tested against the visibility buffer's depth. A streak spreads the
// particle's light over its length, like motion blur would.

#extension GL_GOOGLE_include_directive : require

#define STARCORE_NO_RAY_QUERY
#include "hybrid.glsl"

/** RenderParticle (48 bytes). */
struct Particle
{
    vec3 position;
    float size;  // Sprite radius, world units.
    vec3 previousPosition;
    float padding;
    vec4 color;  // rgb: emitted radiance.
};
layout(set = 0, binding = 3, std430) readonly buffer Particles
{
    Particle particles[];
};

layout(push_constant) uniform Draw
{
    float shutter;  // Streak length in frames (0: round sprites).
    float padding;
    vec2 depthScale;  // particle.frag's.
}
draw;

layout(location = 0) flat out vec2 outHead;     // Pixels.
layout(location = 1) flat out vec2 outTrail;    // Pixels, towards where it was.
layout(location = 2) flat out float outRadius;  // Pixels.
layout(location = 3) flat out vec3 outColor;

// (along the streak, across it) of the two triangles.
const vec2 kCorners[6] = vec2[](vec2(0.0, -1.0), vec2(1.0, -1.0), vec2(1.0, 1.0),
                                vec2(0.0, -1.0), vec2(1.0, 1.0), vec2(0.0, 1.0));
// Sprites fall off as (1 - d^2)^2 of the distance in radii (particle.frag);
// their integral over a disc (pi / 3 r^2) and across a line (16 / 15 r).
const float kDiscIntegral = 1.0472;
const float kLineIntegral = 1.0667;

void main()
{
    Particle particle = particles[gl_VertexIndex / 6];
    vec2 corner = kCorners[gl_VertexIndex % 6];
    float near = frameData.cameraPosition.w;
    vec3 head = projectCurrent(particle.position);
    if (head.z <= near || particle.size <= 0.0) {
        gl_Position = vec4(2.0, 2.0, 0.5, 1.0);  // Off screen, zero area.
        outHead = vec2(0.0);
        outTrail = vec2(0.0);
        outRadius = 1.0;
        outColor = vec3(0.0);
        return;
    }
    vec3 tail = projectPrevious(particle.previousPosition);
    vec2 trail = tail.z > near ? (tail.xy - head.xy) * draw.shutter : vec2(0.0);

    // Pixels per world unit at the particle's depth (cameraUp = up * tan(fov / 2)).
    float scale = 0.5 * viewportSize().y / (length(frameData.cameraUp.xyz) * head.z);
    float trueRadius = particle.size * scale;
    // At least a pixel or so across: tiny particles get dimmer rather than smaller.
    float radius = max(trueRadius, 1.5);
    float coverage = trueRadius * trueRadius / (radius * radius);
    float streak = length(trail);
    float energy = kDiscIntegral * radius / (kDiscIntegral * radius + kLineIntegral * streak);

    vec2 along = streak > 1e-3 ? trail / streak : vec2(1.0, 0.0);
    vec2 across = vec2(-along.y, along.x);
    vec2 start = head.xy - along * radius;
    vec2 end = head.xy + trail + along * radius;
    vec2 pixel = mix(start, end, corner.x) + across * (radius * corner.y);
    // Pixels (+y down) to Vulkan NDC (+y down); reversed depth as the visibility pass.
    gl_Position = vec4(pixel / viewportSize() * 2.0 - 1.0, near / head.z, 1.0);
    outHead = head.xy;
    outTrail = trail;
    outRadius = radius;
    outColor = particle.color.rgb * (coverage * energy);
}
