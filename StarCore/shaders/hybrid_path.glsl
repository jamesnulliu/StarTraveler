// path.glsl's parameters from the frame uniforms of the hybrid renderer (hybrid.glsl), for
// the shaders tracing paths or rays with its estimator: those tracing from the rasterized
// first hits (first_hits.glsl) and the irradiance probes' (probe_trace.comp). Include after
// path.glsl.

#ifndef STARCORE_HYBRID_PATH_GLSL
#define STARCORE_HYBRID_PATH_GLSL

#include "hybrid.glsl"
#include "path.glsl"
#include "sky.glsl"

uint instanceMask()
{
    return frameData.options.y;
}

uint lightCount()
{
    return frameData.options.z;
}

vec3 skyRadiance(vec3 direction)
{
    return skyRadianceOf(frameData.sky.xy, direction);
}

vec3 skyAmbient()
{
    return skyAmbientOf(frameData.sky.xy);
}

uint maxBounces()
{
    return frameData.frame.w;
}

bool directOnly()
{
    return (frameData.options.x & kFlagDirectOnly) != 0u;
}

bool nextEventEnabled()
{
    return (frameData.options.x & kFlagNextEvent) != 0u;
}

float fireflyClamp()
{
    return frameData.cameraForward.w;
}

// The light samples the paths' diffuse lobes pick one of (path.glsl's lightCandidates): the
// including shader may define STARCORE_LIGHT_CANDIDATES as a specialization constant
// (hybrid_trace.comp: GpuRenderOptions::lightCandidates).
#ifndef STARCORE_LIGHT_CANDIDATES
#define STARCORE_LIGHT_CANDIDATES 1u
#endif

uint lightCandidates()
{
    return STARCORE_LIGHT_CANDIDATES;
}

#endif  // STARCORE_HYBRID_PATH_GLSL