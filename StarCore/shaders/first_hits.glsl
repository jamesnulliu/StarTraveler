// Shared by the hybrid renderer's shaders that trace from the rasterized first
// hits (hybrid_trace.comp, direct.comp): path.glsl's parameters from the frame
// uniforms, and the hits of a pixel's camera ray. The including shader declares
// `visibility` and `glassVisibility` (the visibility buffers) and
// `previousTransforms` (GpuScene::previousTransformBuffer()) before including it.

#ifndef STARCORE_FIRST_HITS_GLSL
#define STARCORE_FIRST_HITS_GLSL

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

/**
 * Whether the light the primary surfaces get straight from the lights is a layer
 * of its own (direct.comp; their paths then skip it, kVertexSkipLights): when the
 * lights are sampled at all.
 */
bool directLayerEnabled()
{
    return nextEventEnabled() && lightCount() > 0u && (frameData.options.x & kFlagPreview) == 0u;
}

// Light samples the diffuse lobes of the hybrid renderer's paths pick one of
// (PathState::lightCandidates).
const uint kHybridLightCandidates = 4u;

/** A path of the hybrid renderer: its diffuse lobes' light resampled. */
PathState hybridPath()
{
    PathState path = newPath();
    path.lightCandidates = kHybridLightCandidates;
    return path;
}

/** pathVertex flags for a path's vertex on the primary surface: kVertexSkipLights if so. */
uint skipLightsFlag()
{
    return directLayerEnabled() ? kVertexSkipLights : 0u;
}

float viewDepth(SurfaceHit hit, vec3 direction)
{
    return hit.t * dot(direction, frameData.cameraForward.xyz);
}

vec4 motionOf(uint instanceIndex, vec3 objectPoint)
{
    return screenMotion(instanceIndex, previousTransforms[instanceIndex].objectToWorld,
                        objectPoint);
}

/** A world point that moves with an instance, where it was in the previous frame. */
vec3 previousPosition(uint instanceIndex, vec3 point)
{
    return transformPoint(previousTransforms[instanceIndex].objectToWorld,
                          transformPoint(transforms[instanceIndex].worldToObject, point));
}

bool movedSince(uint instanceIndex)
{
    for (int row = 0; row < 3; ++row) {
        if (any(notEqual(transforms[instanceIndex].objectToWorld[row],
                         previousTransforms[instanceIndex].objectToWorld[row]))) {
            return true;
        }
    }
    return false;
}

/** The rasterized first hits of a pixel's camera ray (with the pane only if in front). */
void firstHits(ivec2 pixel, vec3 origin, vec3 direction, out bool hasPrimary,
               out SurfaceHit primary, out vec3 primaryPoint, out bool hasPane,
               out SurfaceHit pane, out vec3 panePoint)
{
    uvec2 opaque = imageLoad(visibility, pixel).xy;
    hasPrimary = opaque.x != 0u &&
                 reconstructHit(opaque.x - 1u, opaque.y, origin, direction, primary,
                                primaryPoint);
    uvec2 glass = imageLoad(glassVisibility, pixel).xy;
    hasPane = glass.x != 0u &&
              reconstructHit(glass.x - 1u, glass.y, origin, direction, pane, panePoint) &&
              (!hasPrimary || pane.t < primary.t);
}

/**
 * What the primary surface (at distance primaryT) is seen through: the
 * expected transmittance of the panes in front of it, their emission, and the
 * path vertices they take (panes are vertices too: bounce limits as the
 * reference).
 */
void throughPanes(bool hasPane, SurfaceHit pane, float primaryT, vec3 direction,
                  out vec3 transmittance, out vec3 emission, out uint bounces)
{
    transmittance = vec3(1.0);
    emission = vec3(0.0);
    bounces = 0u;
    if (!hasPane) {
        return;
    }
    Material material = materialOf(pane.instanceIndex);
    emission += material.emission.rgb;
    transmittance = thinTransmittance(material, abs(dot(direction, pane.geometricNormal)));
    bounces = 1u;
    // Further panes before the primary surface (rare): their expected transmittance;
    // what they reflect is left out.
    vec3 position = offsetOrigin(pane.position, pane.geometricNormal, direction);
    float remaining = primaryT - pane.t;
    for (int layer = 1; layer < kMaxThinLayers && maxComponent(transmittance) > 0.0; ++layer) {
        SurfaceHit next;
        if (!traceClosestHit(position, direction, instanceMask(), remaining * 0.9999, next)) {
            break;
        }
        Material nextMaterial = materialOf(next.instanceIndex);
        if (!isGlassLayer(nextMaterial)) {
            break;  // The rasterizer and the ray disagree: trust the rasterizer.
        }
        emission += transmittance * nextMaterial.emission.rgb;
        transmittance *=
            thinTransmittance(nextMaterial, abs(dot(direction, next.geometricNormal)));
        position = offsetOrigin(next.position, next.geometricNormal, direction);
        remaining -= next.t;
        ++bounces;
    }
}

#endif  // STARCORE_FIRST_HITS_GLSL
