// Shared by the passes of the hybrid renderer (src/gpu/hybrid_renderer.cpp):
// frame uniforms, instance transforms, camera projection, surface guides.

#ifndef STARCORE_HYBRID_GLSL
#define STARCORE_HYBRID_GLSL

#include "scene.glsl"

/** gpu::HybridFrameUniforms. */
layout(set = 0, binding = 1, std140) uniform FrameUniforms
{
    vec4 cameraPosition;    // w: near plane of the visibility pass
    vec4 cameraForward;     // w: firefly clamp (0 = off)
    vec4 cameraRight;       // right * aspect * tan(fov / 2); w: exposure
    vec4 cameraUp;          // up * tan(fov / 2)
    vec4 previousPosition;  // The camera of the previous frame.
    vec4 previousForward;
    vec4 previousRight;
    vec4 previousUp;
    vec4 skyColor;
    vec4 viewport;   // xy: size in pixels, zw: sample position in the pixel ([0, 1), jitter)
    vec4 previousJitter;  // xy: the sample position of the previous frame
    uvec4 frame;     // random stream index, seed, samples per pixel, max bounces
    uvec4 options;   // flags, instance mask, light count, tone mapper
    uvec4 history;   // cap (samples while things move; 0 = none), kHistory* bits, view, -
    vec4 denoiser;   // color sigma, normal power, depth sigma, -
}
frameData;

// options.x
const uint kFlagPreview = 1u;
const uint kFlagNextEvent = 2u;
const uint kFlagSrgb = 4u;
const uint kFlagWriteIds = 8u;
// history.y
const uint kHistoryReset = 1u;   // Ignore earlier frames.
const uint kHistoryStatic = 2u;  // Nothing moved since the last frame.

/** gpu::InstanceTransformRecord: rows of 3x4 matrices. */
struct InstanceTransform
{
    vec4 objectToWorld[3];
    vec4 worldToObject[3];
    vec4 previousObjectToWorld[3];
};
layout(set = 0, binding = 2, std430) readonly buffer Transforms
{
    InstanceTransform transforms[];
};

vec3 transformPoint(vec4 rows[3], vec3 p)
{
    vec4 h = vec4(p, 1.0);
    return vec3(dot(rows[0], h), dot(rows[1], h), dot(rows[2], h));
}

vec3 transformVector(vec4 rows[3], vec3 v)
{
    return vec3(dot(rows[0].xyz, v), dot(rows[1].xyz, v), dot(rows[2].xyz, v));
}

mat3 linearPart(vec4 rows[3])
{
    return transpose(mat3(rows[0].xyz, rows[1].xyz, rows[2].xyz));
}

vec2 viewportSize()
{
    return frameData.viewport.xy;
}

/** Unit ray direction through a continuous pixel position (as path_trace.comp). */
vec3 cameraDirection(vec2 position)
{
    vec2 size = viewportSize();
    float ndcX = 2.0 * position.x / size.x - 1.0;
    float ndcY = 1.0 - 2.0 * position.y / size.y;  // Pixel +y is down.
    return normalize(frameData.cameraForward.xyz + frameData.cameraRight.xyz * ndcX +
                     frameData.cameraUp.xyz * ndcY);
}

/** Continuous pixel position (xy) and view depth (z) of a point seen by a camera. */
vec3 projectToPixel(vec3 point, vec3 position, vec3 forward, vec3 right, vec3 up)
{
    vec3 d = point - position;
    float z = dot(d, forward);
    float ndcX = dot(d, right) / (dot(right, right) * z);
    float ndcY = dot(d, up) / (dot(up, up) * z);
    vec2 size = viewportSize();
    return vec3((ndcX + 1.0) * 0.5 * size.x, (1.0 - ndcY) * 0.5 * size.y, z);
}

vec3 projectCurrent(vec3 point)
{
    return projectToPixel(point, frameData.cameraPosition.xyz, frameData.cameraForward.xyz,
                          frameData.cameraRight.xyz, frameData.cameraUp.xyz);
}

vec3 projectPrevious(vec3 point)
{
    return projectToPixel(point, frameData.previousPosition.xyz, frameData.previousForward.xyz,
                          frameData.previousRight.xyz, frameData.previousUp.xyz);
}

/**
 * Screen motion (xy, pixels) of an object-space point since the previous
 * frame and its view depth then (z); exactly 0 when neither the instance nor
 * the camera moved. Points behind the previous camera move off screen.
 */
vec4 motionOf(uint instanceIndex, vec3 objectPoint)
{
    vec3 current = projectCurrent(
        transformPoint(transforms[instanceIndex].objectToWorld, objectPoint));
    vec3 previous = projectPrevious(
        transformPoint(transforms[instanceIndex].previousObjectToWorld, objectPoint));
    if (previous.z <= 0.0) {
        return vec4(1e9, 1e9, 0.0, 0.0);
    }
    return vec4(previous.xy - current.xy, previous.z, 0.0);
}

/**
 * The hit of a camera ray (origin, unit direction) with a known primitive (from
 * the visibility buffer): instance `instanceIndex`, triangle `primitive`.
 * Triangles are intersected as planes (the rasterizer decided coverage).
 */
bool reconstructHit(uint instanceIndex, uint primitive, vec3 origin, vec3 direction,
                    out SurfaceHit hit, out vec3 objectPoint)
{
    Instance instance = instances[instanceIndex];
    vec3 o = transformPoint(transforms[instanceIndex].worldToObject, origin);
    vec3 d = transformVector(transforms[instanceIndex].worldToObject, direction);
    float t = 0.0;
    vec2 barycentrics = vec2(0.0);
    objectPoint = vec3(0.0);
    if (instance.kind == kGeometrySphere) {
        if (!intersectUnitSphere(o, d, 0.0, kFar, t)) {
            return false;
        }
        objectPoint = o + t * d;
    } else {
        uvec3 i = triangleIndices(instance, primitive);
        vec3 v0 = loadVec3(instance.positions, i.x);
        vec3 e1 = loadVec3(instance.positions, i.y) - v0;
        vec3 e2 = loadVec3(instance.positions, i.z) - v0;
        vec3 p = cross(d, e2);
        float determinant = dot(e1, p);
        if (abs(determinant) < 1e-30) {
            return false;
        }
        float inverse = 1.0 / determinant;
        vec3 s = o - v0;
        vec3 q = cross(s, e1);
        barycentrics = vec2(dot(s, p), dot(d, q)) * inverse;
        t = dot(e2, q) * inverse;
        if (t <= 0.0) {
            return false;
        }
        objectPoint = v0 + barycentrics.x * e1 + barycentrics.y * e2;
    }
    vec3 objectGeometric;
    vec3 objectShading;
    objectNormals(instance, primitive, barycentrics, objectPoint, objectGeometric,
                  objectShading);
    hit = makeSurfaceHit(origin, direction, t, instanceIndex, objectGeometric, objectShading,
                         linearPart(transforms[instanceIndex].worldToObject));
    return true;
}

// Surfaces in the glass layer: smooth window panes, looked through.
const float kGlassMaxRoughness = 0.1;

bool isGlassLayer(Material m)
{
    return (m.flags & kMaterialThinWalled) != 0u && m.transmission * (1.0 - m.metallic) >= 0.5 &&
           m.roughness <= kGlassMaxRoughness;
}

/**
 * How much the spatial filter may blur a surface's lighting: 1 for diffuse
 * and rough surfaces, towards 0 for mirrors and clear glass, whose lighting
 * is a sharp image of something else.
 */
float filterStrength(Material m)
{
    float specular = clamp(m.metallic + (1.0 - m.metallic) * m.transmission, 0.0, 1.0);
    return mix(1.0, smoothstep(0.05, 0.4, m.roughness), specular);
}

// Guides (rgba32ui) --------------------------------------------------------------------------------
// x: view depth (float bits), y: octahedral shading normal (snorm16 x 2),
// z: instance index + 1 (0: no surface), w: filter strength (float bits).

vec2 octWrap(vec2 v)
{
    return (1.0 - abs(v.yx)) * vec2(v.x >= 0.0 ? 1.0 : -1.0, v.y >= 0.0 ? 1.0 : -1.0);
}

vec2 octahedral(vec3 n)
{
    n /= abs(n.x) + abs(n.y) + abs(n.z);
    return n.z >= 0.0 ? n.xy : octWrap(n.xy);
}

vec3 fromOctahedral(vec2 e)
{
    vec3 n = vec3(e, 1.0 - abs(e.x) - abs(e.y));
    float t = max(-n.z, 0.0);
    n.x += n.x >= 0.0 ? -t : t;
    n.y += n.y >= 0.0 ? -t : t;
    return normalize(n);
}

uint encodeNormal(vec3 n)
{
    return packSnorm2x16(octahedral(n));
}

vec3 decodeNormal(uint packed)
{
    return fromOctahedral(unpackSnorm2x16(packed));
}

uvec4 encodeGuide(float viewDepth, vec3 normal, uint instanceIndex, float strength)
{
    return uvec4(floatBitsToUint(viewDepth), encodeNormal(normal), instanceIndex + 1u,
                 floatBitsToUint(strength));
}

float guideDepth(uvec4 guide)
{
    return uintBitsToFloat(guide.x);
}

vec3 guideNormal(uvec4 guide)
{
    return decodeNormal(guide.y);
}

float guideStrength(uvec4 guide)
{
    return uintBitsToFloat(guide.w);
}

// Filter guides (rg32ui), compact for the many taps of the spatial filter:
// x: view depth (float bits), y: instance key (16 bits, 0: no surface) and
// the octahedral normal (8 + 8 bits) above it.

uvec2 encodeFilterGuide(float viewDepth, vec3 normal, uint instanceIndex)
{
    uint key = min(instanceIndex + 1u, 0xFFFFu);
    return uvec2(floatBitsToUint(viewDepth),
                 key | (packSnorm4x8(vec4(octahedral(normal), 0.0, 0.0)) << 16));
}

uint filterKey(uvec2 guide)
{
    return guide.y & 0xFFFFu;
}

vec3 filterNormal(uvec2 guide)
{
    return fromOctahedral(unpackSnorm4x8(guide.y >> 16).xy);
}

float luminance(vec3 color)
{
    return dot(color, vec3(0.2126, 0.7152, 0.0722));
}

/** Albedo the primary layer's lighting is divided by (never 0). */
vec3 demodulationAlbedo(vec3 albedo)
{
    return max(albedo, vec3(0.01));
}

#endif  // STARCORE_HYBRID_GLSL
