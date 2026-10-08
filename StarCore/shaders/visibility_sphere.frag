#version 460
// Visibility buffer, fragment stage for spheres: the bounding cube is drawn
// and each fragment intersects its camera ray with the analytic sphere,
// exactly as the compute passes will, writing the sphere's depth.

#extension GL_GOOGLE_include_directive : require

#define STARCORE_NO_RAY_QUERY
#include "hybrid.glsl"

layout(push_constant) uniform Draw
{
    uint instanceIndex;
}
draw;

layout(location = 0) flat in uint primitive;
layout(location = 0) out uvec2 visibility;
// Reversed depth: the sphere is behind its cube, so never nearer (larger).
layout(depth_less) out float gl_FragDepth;

void main()
{
    vec2 position = gl_FragCoord.xy - 0.5 + frameData.viewport.zw;
    vec3 direction = cameraDirection(position);
    vec3 origin = frameData.cameraPosition.xyz;
    float t;
    if (!intersectUnitSphere(transformPoint(transforms[draw.instanceIndex].worldToObject, origin),
                             transformVector(transforms[draw.instanceIndex].worldToObject,
                                             direction),
                             0.0, kFar, t)) {
        discard;
    }
    float z = t * dot(direction, frameData.cameraForward.xyz);
    float near = frameData.cameraPosition.w;
    if (z < near) {
        discard;
    }
    gl_FragDepth = near / z;
    visibility = uvec2(draw.instanceIndex + 1u, 0u);
}
