#version 460
// Particles, fragment stage: a smooth falloff around the streak (a segment
// in screen space), (1 - d^2)^2 of the distance in radii -- zero at the quad's
// edges, so no square shows even for very bright particles -- added to the
// particle image, where no surface is in front (the visibility buffer's depth,
// at the rendered size: lower than the particle image's when upscaled).

layout(location = 0) flat in vec2 head;
layout(location = 1) flat in vec2 trail;
layout(location = 2) flat in float radius;
layout(location = 3) flat in vec3 color;

layout(set = 0, binding = 4) uniform sampler2D depth;  // Reversed: larger is nearer.

layout(push_constant) uniform Draw
{
    float shutter;
    float padding;
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
    float distance = length(position - (head + trail * t)) / radius;
    float falloff = max(0.0, 1.0 - distance * distance);
    result = vec4(color * (falloff * falloff), 0.0);
}
