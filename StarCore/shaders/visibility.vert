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

// The 36 corners of a [-1, 1]^3 cube's 12 triangles (bit i: axis i positive).
const uint kCubeCorners[36] = uint[](0u, 2u, 1u, 1u, 2u, 3u,  // -z
                                     4u, 5u, 6u, 5u, 7u, 6u,  // +z
                                     0u, 1u, 4u, 1u, 5u, 4u,  // -y
                                     2u, 6u, 3u, 3u, 6u, 7u,  // +y
                                     0u, 4u, 2u, 2u, 4u, 6u,  // -x
                                     1u, 3u, 5u, 3u, 7u, 5u   // +x
);

void main()
{
    Instance instance = instances[draw.instanceIndex];
    vec3 p;
    if (instance.kind == kGeometrySphere) {
        uint corner = kCubeCorners[gl_VertexIndex];
        p = vec3((corner & 1u) != 0u ? 1.0 : -1.0, (corner & 2u) != 0u ? 1.0 : -1.0,
                 (corner & 4u) != 0u ? 1.0 : -1.0);
    } else {
        p = loadVec3(instance.positions, UintArray(instance.indices).values[gl_VertexIndex]);
    }
    vec3 d = transformPoint(transforms[draw.instanceIndex].objectToWorld, p) -
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
