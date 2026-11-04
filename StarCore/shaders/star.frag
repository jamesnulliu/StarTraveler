#version 460
// The space sky's stars, fragment stage: the point spread function around a star's streak
// (star.vert) -- a core, (1 - d^2)^2 of the distance in core radii, and a halo, (1 +
// (d / scale)^2)^-1.5 less its value where it ends, so that no square shows -- added to the
// particle image where no surface is in front (the visibility buffer's depth, at the
// rendered size: lower than the particle image's when upscaled).

layout(location = 0) flat in vec2 head;
layout(location = 1) flat in vec2 trail;
layout(location = 2) flat in vec4 shape;  // Core radius, halo scale, halo radius, its falloff there.
layout(location = 3) flat in vec3 core;
layout(location = 4) flat in vec3 halo;

layout(set = 0, binding = 4) uniform sampler2D depth;  // Reversed: larger is nearer.

layout(push_constant) uniform Draw
{
    float shutter;
    float brightness;
    vec2 depthScale;  // The depth's size over the particle image's.
}
draw;

layout(location = 0) out vec4 result;

void main()
{
    ivec2 texel = ivec2(gl_FragCoord.xy * draw.depthScale);
    if (gl_FragCoord.z < texelFetch(depth, texel, 0).r) {
        discard;  // Behind a surface.
    }
    vec2 position = gl_FragCoord.xy;
    float lengthSquared = dot(trail, trail);
    float t = lengthSquared > 1e-6 ? clamp(dot(position - head, trail) / lengthSquared, 0.0, 1.0)
                                   : 0.0;
    float distance = length(position - (head + trail * t));
    float d = distance / shape.x;
    float coreFalloff = max(0.0, 1.0 - d * d);
    float h = distance / shape.y;
    float haloFalloff = distance < shape.z ? max(0.0, pow(1.0 + h * h, -1.5) - shape.w) : 0.0;
    result = vec4(core * (coreFalloff * coreFalloff) + halo * haloFalloff, 0.0);
}
