// The path tracer's estimator (path.glsl) with its parameters from the hybrid
// pipeline's frame uniforms, and the irradiance probes (probes.glsl): for the
// passes tracing paths, hybrid_trace.comp and probe_trace.comp. The including
// shader defines PROBE_BINDING.

#ifndef STARCORE_HYBRID_PATH_GLSL
#define STARCORE_HYBRID_PATH_GLSL

#include "hybrid.glsl"
#include "path.glsl"

uint instanceMask()
{
    return frameData.options.y;
}

uint lightCount()
{
    return frameData.options.z;
}

vec3 skyColor()
{
    return frameData.skyColor.rgb;
}

uint maxBounces()
{
    return frameData.frame.w;
}

bool nextEventEnabled()
{
    return (frameData.options.x & kFlagNextEvent) != 0u;
}

float fireflyClamp()
{
    return frameData.cameraForward.w;
}

bool probesEnabled()
{
    return (frameData.options.x & kFlagProbes) != 0u;
}

#include "probes.glsl"

#endif  // STARCORE_HYBRID_PATH_GLSL
