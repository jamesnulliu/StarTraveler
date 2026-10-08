#version 460
// Visibility buffer, fragment stage for triangles: instance + 1 and triangle.

#extension GL_GOOGLE_include_directive : require

layout(push_constant) uniform Draw
{
    uint instanceIndex;
}
draw;

layout(location = 0) flat in uint primitive;
layout(location = 0) out uvec2 visibility;

void main()
{
    visibility = uvec2(draw.instanceIndex + 1u, primitive);
}
