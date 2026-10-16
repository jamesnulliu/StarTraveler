#version 460
// Shadow maps, fragment stage for spheres: the bounding cube is drawn and each
// fragment intersects the ray from the light through it with the analytic
// sphere, writing the depth of the middle of the ray's chord through it (as
// Woo's midpoint shadow maps), not of its near side: its silhouette is around
// its middle, as the points it shades see it. The penumbras' width follows
// from the occluders' depth (shadows.glsl), and that of the near side made a
// ball's shadow too wide and too light, without the umbra under it.

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
    ShadowView view = shadowViews[draw.viewIndex];
    // Unit view depth along it: the ray's parameter is the view depth.
    vec3 direction = shadowDirection(view, gl_FragCoord.xy - view.tile.xy);
    float near = view.position.w;
    vec3 origin = transformPoint(transforms[draw.instanceIndex].worldToObject, view.position.xyz);
    vec3 objectDirection = transformVector(transforms[draw.instanceIndex].worldToObject, direction);
    float z;
    if (!intersectUnitSphere(origin, objectDirection, near, kFar, z)) {
        discard;
    }
    // The chord's middle: the point of the ray nearest the center.
    float middle = -dot(origin, objectDirection) / dot(objectDirection, objectDirection);
    gl_FragDepth = near / max(middle, z);
}
