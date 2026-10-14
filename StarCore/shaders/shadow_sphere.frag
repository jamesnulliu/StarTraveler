#version 460
// Shadow maps, fragment stage for spheres: the bounding cube is drawn and each
// fragment intersects the ray from the light through it with the analytic
// sphere, writing the sphere's depth.

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
    float z;
    if (!intersectUnitSphere(
            transformPoint(transforms[draw.instanceIndex].worldToObject, view.position.xyz),
            transformVector(transforms[draw.instanceIndex].worldToObject, direction), near, kFar,
            z)) {
        discard;
    }
    gl_FragDepth = near / z;
}
