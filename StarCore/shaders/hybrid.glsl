// Shared by the passes of the rasterizing pipelines, Hybrid and Raster
// (src/gpu/hybrid_renderer.cpp, raster_renderer.cpp): frame uniforms,
// instance transforms, camera projection, surface guides.

#ifndef STARCORE_HYBRID_GLSL
#define STARCORE_HYBRID_GLSL

#include "scene.glsl"

/** gpu::FrameUniforms. */
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
    uvec4 history;   // cap (frames while things move; 0 = none), kHistory* bits, view, traced pixels
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
// Pixels traced (history.w, gpu::TraceResolution): bits 0-7 the stride, 1
// for every pixel, 2 for one of each 2x2 per frame, in turn; bits 8-15 this
// frame's turn, 16-23 the previous frame's.
uint traceStride()
{
    return frameData.history.w & 0xFFu;
}

/** Which pixel of a block is traced on a turn: diagonals first, every pixel in 4 turns. */
ivec2 traceOffset(uint turn)
{
    const ivec2 kOffsets[4] = ivec2[](ivec2(0, 0), ivec2(1, 1), ivec2(1, 0), ivec2(0, 1));
    return traceStride() == 1u ? ivec2(0) : kOffsets[turn & 3u];
}

ivec2 currentTraceOffset()
{
    return traceOffset(frameData.history.w >> 8);
}

ivec2 previousTraceOffset()
{
    return traceOffset(frameData.history.w >> 16);
}

// The stride is a power of two: blocks are masks (not divisions, slow on GPUs).
ivec2 blockOf(ivec2 pixel)
{
    return pixel & ~int(traceStride() - 1u);
}

/** Whether a pixel's paths were traced this frame (the others borrow, temporal.comp). */
bool tracedThisFrame(ivec2 pixel)
{
    return all(equal(pixel - blockOf(pixel), currentTraceOffset()));
}

/** The pixel a thread of a dispatch over blocks traces this frame. */
ivec2 tracedPixel(ivec2 block)
{
    return block * int(traceStride()) + currentTraceOffset();
}

/** The pixel of a pixel's block traced this frame. */
ivec2 currentlyTraced(ivec2 pixel)
{
    return blockOf(pixel) + currentTraceOffset();
}

/** The pixel of a pixel's block that was traced in the previous frame. */
ivec2 previouslyTraced(ivec2 pixel)
{
    return blockOf(pixel) + previousTraceOffset();
}

/** Of a frame's samples per pixel, how many a pixel gets on average. */
float traceRate()
{
    float stride = float(traceStride());
    return 1.0 / (stride * stride);
}

/** gpu::InstanceTransformRecord: rows of 3x4 matrices. */
struct InstanceTransform
{
    vec4 objectToWorld[3];
    vec4 worldToObject[3];
};
layout(set = 0, binding = 2, std430) readonly buffer Transforms
{
    InstanceTransform transforms[];
};

mat3 linearPart(vec4 rows[3])
{
    return transpose(mat3(rows[0].xyz, rows[1].xyz, rows[2].xyz));
}

// The 36 corners of a [-1, 1]^3 cube's 12 triangles (bit i: axis i positive).
const uint kCubeCorners[36] = uint[](0u, 2u, 1u, 1u, 2u, 3u,  // -z
                                     4u, 5u, 6u, 5u, 7u, 6u,  // +z
                                     0u, 1u, 4u, 1u, 5u, 4u,  // -y
                                     2u, 6u, 3u, 3u, 6u, 7u,  // +y
                                     0u, 4u, 2u, 2u, 4u, 6u,  // -x
                                     1u, 3u, 5u, 3u, 7u, 5u   // +x
);

/**
 * Object-space vertex of a non-indexed draw of an instance (three vertices
 * per triangle): pulled from its mesh, or a corner of a sphere's bounding cube.
 */
vec3 drawnVertex(uint instanceIndex, uint vertexIndex)
{
    Instance instance = instances[instanceIndex];
    if (instance.kind == kGeometrySphere) {
        uint corner = kCubeCorners[vertexIndex];
        return vec3((corner & 1u) != 0u ? 1.0 : -1.0, (corner & 2u) != 0u ? 1.0 : -1.0,
                    (corner & 4u) != 0u ? 1.0 : -1.0);
    }
    return loadVec3(instance.positions, UintArray(instance.indices).values[vertexIndex]);
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

/** cameraDirection of the previous frame's camera (bit for bit what it was then). */
vec3 previousCameraDirection(vec2 position)
{
    vec2 size = viewportSize();
    float ndcX = 2.0 * position.x / size.x - 1.0;
    float ndcY = 1.0 - 2.0 * position.y / size.y;
    return normalize(frameData.previousForward.xyz + frameData.previousRight.xyz * ndcX +
                     frameData.previousUp.xyz * ndcY);
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
 * Screen motion (xy, pixels) of an object-space point of an instance since
 * the previous frame, given the instance's previous object-to-world rows, and
 * its view depth then (z); exactly 0 when neither the instance nor the camera
 * moved. Points behind the previous camera move off screen.
 */
vec4 screenMotion(uint instanceIndex, vec4 previousObjectToWorld[3], vec3 objectPoint)
{
    vec3 current = projectCurrent(
        transformPoint(transforms[instanceIndex].objectToWorld, objectPoint));
    vec3 previous = projectPrevious(transformPoint(previousObjectToWorld, objectPoint));
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
    hit = makeSurfaceHit(origin, direction, t, instanceIndex,
                         instance.kind == kGeometrySphere ? 0u : primitive, objectGeometric,
                         objectShading, linearPart(transforms[instanceIndex].worldToObject));
    return true;
}

// Layers of the Hybrid pipeline, each reprojected and filtered on its own
// (temporal.comp, atrous.comp).
const uint kLayerPrimary = 0u;   // Lighting of the primary surfaces (divided by their albedo).
const uint kLayerGlass = 1u;     // What the smooth window panes in front reflect.
const uint kLayerSpecular = 2u;  // What the primary surfaces reflect (kLobesReflection).

// Surfaces in the glass layer: smooth window panes, looked through.
const float kGlassMaxRoughness = 0.1;

bool isGlassLayer(Material m)
{
    return (m.flags & kMaterialThinWalled) != 0u && m.transmission * (1.0 - m.metallic) >= 0.5 &&
           m.roughness <= kGlassMaxRoughness;
}

// Primary surfaces at most this rough reflect into the specular layer: their
// reflections move unlike them (rougher ones blur them into their lighting).
const float kSpecularMaxRoughness = 0.5;

bool hasSpecularLayer(Material m)
{
    return m.roughness < kSpecularMaxRoughness;
}

/**
 * How much the spatial filter may blur what a surface of this roughness
 * reflects: none for mirrors (nor where the specular layer is empty).
 */
float reflectionStrength(float roughness)
{
    return roughness < kSpecularMaxRoughness ? smoothstep(0.05, 0.4, roughness) : 0.0;
}

/**
 * How sharp the reflections of a surface of this roughness are, so how
 * strictly their history is checked (temporal.comp): glossy ones are blurred
 * images, which the checks (of what the mirror direction sees, of this
 * frame's noisy samples) would mistake for changes.
 */
float reflectionSharpness(float roughness)
{
    return 1.0 - smoothstep(0.1, 0.3, roughness);
}

/**
 * How much the spatial filter may blur a primary surface's lighting: 1 for
 * diffuse light, towards 0 for what smooth surfaces reflect or let through,
 * which is a sharp image of something else (reflections leave the lighting
 * for the specular layer, see hasSpecularLayer).
 */
float lightingStrength(Material m)
{
    float transmission = (1.0 - m.metallic) * m.transmission;
    float specular = hasSpecularLayer(m) ? transmission : m.metallic + transmission;
    return mix(1.0, smoothstep(0.05, 0.4, m.roughness), clamp(specular, 0.0, 1.0));
}

// Guides (rgba32ui) --------------------------------------------------------------------------------
// x: view depth (float bits), y: octahedral shading normal (snorm16 x 2),
// z: instance index + 1 (0: no surface), w: half floats, the filter strength
// of the surface's lighting (or of what a pane reflects) and its roughness.

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

uvec4 encodeGuide(float viewDepth, vec3 normal, uint instanceIndex, float strength,
                  float roughness)
{
    return uvec4(floatBitsToUint(viewDepth), encodeNormal(normal), instanceIndex + 1u,
                 packHalf2x16(vec2(strength, roughness)));
}

float guideDepth(uvec4 guide)
{
    return uintBitsToFloat(guide.x);
}

vec3 guideNormal(uvec4 guide)
{
    return decodeNormal(guide.y);
}

float guideRoughness(uvec4 guide)
{
    return unpackHalf2x16(guide.w).y;
}

/** Filter strength of a layer on a guide's surface (the primary guide's for kLayerSpecular). */
float guideStrength(uvec4 guide, uint layer)
{
    return layer == kLayerSpecular ? reflectionStrength(guideRoughness(guide))
                                   : unpackHalf2x16(guide.w).x;
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

// Temporal gradients (A-SVGF, hybrid_trace.comp gradientAt): one per
// kGradientStratum^2 pixels, rgba32f: x how much the luminance of the primary
// surface's direct light changed since the previous frame, y the larger of the
// two luminances, z the weight (0: none), w the instance key of the surface
// (float bits).
const int kGradientStratum = 3;
// A change of 1/kGradientGain of the lighting per frame drops all of the
// history: it would lag half the change of the frames it keeps (a soft shadow
// passing changes each point by a little every frame). No change drops
// nothing, whatever the gain.
const float kGradientGain = 16.0;

/**
 * How much of the primary lighting changed (0-1) by a (filtered) gradient on
 * surface `key`, relative to the lighting there (`lighting`, its history's
 * luminance; or the direct light, if more): a change of a dim light is a
 * little of a well lit surface's lighting.
 */
float gradientChange(vec4 gradient, uint key, float lighting)
{
    if (gradient.z <= 0.0 || floatBitsToUint(gradient.w) != key) {
        return 0.0;
    }
    return clamp(kGradientGain * gradient.x / max(max(gradient.y, lighting), 1e-6), 0.0, 1.0);
}

// Reflection guides (two uints per reflecting layer): what the mirror
// direction of the surface hits, x: its distance (float bits), y: instance + 1
// (0: the sky).

uvec2 encodeReflection(float distance, uint key)
{
    return uvec2(floatBitsToUint(distance), key);
}

/** Whether two reflection guides saw the same: the same instance, at a similar distance. */
bool sameReflection(uvec2 a, uvec2 b)
{
    float distanceA = uintBitsToFloat(a.x);
    float distanceB = uintBitsToFloat(b.x);
    return a.y == b.y && abs(distanceA - distanceB) <= 0.2 * max(distanceA, distanceB);
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
