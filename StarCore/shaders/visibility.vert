#version 460
// Visibility buffer, vertex stage: one draw per instance, vertices pulled from
// the mesh buffers (spheres: the corners of their bounding cube). The
// projection is jittered so the rasterizer samples pixel + jitter, the same
// point the compute passes shoot their camera ray through.

#extension GL_GOOGLE_include_directive : require

#define STARCORE_NO_RAY_QUERY
#include "hybrid.glsl"

layout(push_constant) uniform Draw
{
    uint instanceIndex;
}
draw;

layout(location = 0) flat out uint outPrimitive;

void main()
{
    vec3 d = transformPoint(transforms[draw.instanceIndex].objectToWorld,
                            drawnVertex(draw.instanceIndex, uint(gl_VertexIndex))) -
             frameData.cameraPosition.xyz;

    // x/w and y/w: NDC of the camera ray convention, shifted by half a pixel
    // minus the jitter; reversed depth: z/w = near / view depth.
    vec3 right = frameData.cameraRight.xyz;
    vec3 up = frameData.cameraUp.xyz;
    float z = dot(d, frameData.cameraForward.xyz);
    vec2 shift = (0.5 - frameData.viewport.zw) * 2.0 / viewportSize();
    gl_Position = vec4(dot(d, right) / dot(right, right) + shift.x * z,
                       -dot(d, up) / dot(up, up) + shift.y * z, frameData.cameraPosition.w, z);
    // Non-indexed draws: three vertices per triangle (flat: the provoking vertex's).
    outPrimitive = uint(gl_VertexIndex) / 3u;
}
