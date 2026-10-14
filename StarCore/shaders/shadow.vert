#version 460
// Shadow maps, vertex stage: one draw per instance and view (shadows.glsl),
// into the view's tile of the depth atlas; vertices pulled from the mesh
// buffers (spheres: their bounding cube, intersected in shadow_sphere.frag).
// Triangles need no fragment stage: the rasterizer writes their depth.

#extension GL_GOOGLE_include_directive : require

#define STARCORE_NO_RAY_QUERY
#define STARCORE_SHADOW_PASS
#include "hybrid.glsl"
#include "shadows.glsl"

layout(push_constant) uniform Draw
{
    uint instanceIndex;
    uint viewIndex;
}
draw;

void main()
{
    gl_Position = shadowClipPosition(
        shadowViews[draw.viewIndex],
        transformPoint(transforms[draw.instanceIndex].objectToWorld,
                       drawnVertex(draw.instanceIndex, uint(gl_VertexIndex))));
}
