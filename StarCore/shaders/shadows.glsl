// Shadow maps (src/gpu/shadow_maps.cpp) of the Raster pipeline and of the
// Hybrid one's direct light (DirectShadows::ShadowMaps): the scene's depth as
// seen from the center of the brightest sphere lights, each view in a tile of
// one depth atlas, and percentage-closer soft shadows looked up in them. The
// shadow pass (shadow.vert, shadow_sphere.frag) defines STARCORE_SHADOW_PASS:
// it renders the views and looks nothing up. The bindings are 13 and 14 unless
// STARCORE_SHADOW_RECORDS / STARCORE_SHADOW_ATLAS say otherwise.

#ifndef STARCORE_SHADOWS_GLSL
#define STARCORE_SHADOWS_GLSL

#include "common.glsl"

#ifndef STARCORE_SHADOW_RECORDS
#define STARCORE_SHADOW_RECORDS 13
#endif
#ifndef STARCORE_SHADOW_ATLAS
#define STARCORE_SHADOW_ATLAS 14
#endif

const uint kMaxShadowLights = 4u;
const uint kMaxShadowViews = 16u;

/**
 * gpu::ShadowViewRecord: a perspective view from a light's center. A point at
 * offset d from the center is at view depth z = dot(d, forward) and tangent
 * (dot(d, right), dot(d, up)) / z; the view covers center +- 1 / scale of the
 * tangents, its depth is near / z (reversed, 0: nothing).
 */
struct ShadowView
{
    vec4 position;  // xyz: the light's center, w: near plane
    vec4 forward;   // xyz: unit, w: scale (1 / tangent of half the field of view)
    vec4 right;     // xyz: unit, w: tangent of the view's center
    vec4 up;        // xyz: unit, w: tangent of the view's center
    vec4 tile;      // xy: atlas texel of the tile's corner, z: texels per side, w: 1 / atlas size
};

/** gpu::ShadowLightRecord: the views of a light. */
struct ShadowLight
{
    uint lightIndex;  // Into lights.
    uint firstView;
    uint viewCount;
    uint cube;   // 1: six views along +-axisX, +-axisY, +-axisZ; 0: nested, finest first.
    vec4 axisX;  // xyz: the cube's axes.
    vec4 axisY;
    vec4 axisZ;
};

// A uniform buffer: lookups read views from the constant cache.
layout(set = 0, binding = STARCORE_SHADOW_RECORDS, std140) uniform Shadows
{
    uvec4 shadowCount;  // x: lights with views
    ShadowLight shadowLights[kMaxShadowLights];
    ShadowView shadowViews[kMaxShadowViews];
};

/** Clip-space position of a world point in a view (the tile is the viewport). */
vec4 shadowClipPosition(ShadowView view, vec3 point)
{
    vec3 d = point - view.position.xyz;
    float z = dot(d, view.forward.xyz);
    vec2 t = vec2(dot(d, view.right.xyz), dot(d, view.up.xyz)) - vec2(view.right.w, view.up.w) * z;
    // Texel +y is down.
    return vec4(t.x * view.forward.w, -t.y * view.forward.w, view.position.w, z);
}

/**
 * Direction from a view's light (not normalized, unit view depth) through a
 * tile-local texel position.
 */
vec3 shadowDirection(ShadowView view, vec2 local)
{
    vec2 ndc = local / view.tile.z * 2.0 - 1.0;
    vec2 t = vec2(ndc.x, -ndc.y) / view.forward.w + vec2(view.right.w, view.up.w);
    return view.forward.xyz + view.right.xyz * t.x + view.up.xyz * t.y;
}

#ifndef STARCORE_SHADOW_PASS

layout(set = 0, binding = STARCORE_SHADOW_ATLAS) uniform sampler2D shadowAtlas;

// Percentage-closer soft shadows: the occluders' mean distance from the light
// (searched where they could shade the point) and the light's radius make the
// width of the penumbra, over which the shadow test is averaged.
const uint kBlockerTaps = 8u;
const uint kFilterTaps = 16u;
const float kMaxSearchTexels = 24.0;
const float kMinFilterTexels = 1.0;
const float kMaxFilterTexels = 24.0;
// Depth tolerance of the test, in texels (at the receiver's distance).
const float kShadowBiasTexels = 0.5;

/** Tangent of a texel of a view (at its center). */
float shadowTexel(ShadowView view)
{
    return 2.0 / (view.forward.w * view.tile.z);
}

/**
 * How far (tangent) around `direction` (from its light) a view sees, a few
 * texels from its edges; <= 0: not at all.
 */
float shadowReach(ShadowView view, vec3 direction)
{
    float z = dot(direction, view.forward.xyz);
    if (z <= 0.0) {
        return -1.0;
    }
    vec2 t = vec2(dot(direction, view.right.xyz), dot(direction, view.up.xyz)) / z -
             vec2(view.right.w, view.up.w);
    return (1.0 - 4.0 / view.tile.z) / view.forward.w - max(abs(t.x), abs(t.y));
}

/** The view of a light that holds `direction`: a cube's face, or nested view `level`. */
ShadowView shadowViewOf(ShadowLight light, vec3 direction, uint level)
{
    if (light.cube == 0u) {
        return shadowViews[light.firstView + level];
    }
    vec3 local = vec3(dot(direction, light.axisX.xyz), dot(direction, light.axisY.xyz),
                      dot(direction, light.axisZ.xyz));
    vec3 magnitude = abs(local);
    uint axis = magnitude.x >= magnitude.y && magnitude.x >= magnitude.z ? 0u
                : magnitude.y >= magnitude.z                             ? 1u
                                                                         : 2u;
    return shadowViews[light.firstView + 2u * axis + (local[axis] < 0.0 ? 1u : 0u)];
}

/**
 * Shadow test of a receiver plane (point, unit normal facing the light) along
 * `direction` (unit, from the light) against the four texels of `view` around
 * it, each along its own direction (exact for planes, whatever their slope):
 * x the bilinear weight of the texels whose occluder is nearer the light, y
 * the same sum weighting the occluders' distances from it.
 */
vec2 shadowTexels(ShadowView view, vec3 direction, vec3 point, vec3 normal, float bias)
{
    float z = dot(direction, view.forward.xyz);
    vec2 t = vec2(dot(direction, view.right.xyz), dot(direction, view.up.xyz)) / z -
             vec2(view.right.w, view.up.w);
    vec2 local = (t * view.forward.w * vec2(0.5, -0.5) + 0.5) * view.tile.z;
    local = clamp(local, vec2(1.0), vec2(view.tile.z - 1.0));
    vec2 corner = floor(local - 0.5);
    vec2 f = local - 0.5 - corner;
    // Texels corner + (0, 1), (1, 1), (1, 0), (0, 0): the gather's order.
    vec4 depths = textureGather(shadowAtlas, (view.tile.xy + local) * view.tile.w, 0);
    vec4 weights = vec4((1.0 - f.x) * f.y, f.x * f.y, f.x * (1.0 - f.y), (1.0 - f.x) * (1.0 - f.y));
    // Along a texel's direction w (of unit view depth) the occluder is at view
    // depth near / depth, the receiver's plane at plane / dot(w, normal), and
    // dot(w, normal) is linear in the texel's offset.
    vec3 w = shadowDirection(view, corner + 0.5);
    vec2 slope = vec2(dot(view.right.xyz, normal), -dot(view.up.xyz, normal)) * shadowTexel(view);
    vec4 cosines = dot(w, normal) + vec4(slope.y, slope.x + slope.y, slope.x, 0.0);
    float plane = dot(point - view.position.xyz, normal);  // < 0: the light is in front.
    vec4 receivers = mix(vec4(kFar), plane / cosines, lessThan(cosines, vec4(-1e-6)));
    vec4 occluders = view.position.w / max(depths, vec4(1e-30));
    vec4 occluded = weights * vec4(lessThan(occluders, receivers - bias));
    // Distances from the light: view depth times |w|.
    return vec2(dot(occluded, vec4(1.0)), dot(occluded, occluders) * length(w));
}

/** Point i of n on a unit disk, evenly spread (Vogel's spiral), turned by `rotation`. */
vec2 vogelDisk(uint i, uint n, float rotation)
{
    float r = sqrt((float(i) + 0.5) / float(n));
    float angle = float(i) * 2.39996323 + rotation;
    return r * vec2(cos(angle), sin(angle));
}

/** Shadow tests on `taps` points of a disk of `radius` (tangent) around `direction`. */
vec2 shadowDisk(ShadowLight light, uint level, vec3 direction, Frame frame, float radius,
                uint taps, vec3 position, vec3 facing, float bias, float rotation)
{
    ShadowView nested = shadowViews[light.firstView + level];  // Cubes: per tap.
    vec2 result = vec2(0.0);
    for (uint i = 0u; i < taps; ++i) {
        vec2 offset = vogelDisk(i, taps, rotation) * radius;
        vec3 tap = normalize(direction + frame.tangent * offset.x + frame.bitangent * offset.y);
        ShadowView view = light.cube != 0u ? shadowViewOf(light, tap, 0u) : nested;
        result += shadowTexels(view, tap, position, facing, bias);
    }
    return result;
}

/** Whether a light (index into lights) has shadow maps this frame. */
bool hasShadowMaps(uint lightIndex)
{
    for (uint i = 0u; i < shadowCount.x; ++i) {
        if (shadowLights[i].lightIndex == lightIndex) {
            return true;
        }
    }
    return false;
}

/**
 * How much of a sphere light (center, radius) reaches a surface point past
 * the occluders its shadow maps saw: 1 for all of it (or no maps), 0 none.
 * `normal`: the surface's geometric normal; `rotation`: of the taps' pattern.
 */
float shadowVisibility(uint lightIndex, vec3 center, float radius, vec3 position, vec3 normal,
                       float rotation)
{
    int found = -1;
    for (uint i = 0u; i < shadowCount.x; ++i) {
        if (shadowLights[i].lightIndex == lightIndex) {
            found = int(i);
        }
    }
    if (found < 0) {
        return 1.0;
    }
    ShadowLight light = shadowLights[found];
    vec3 toPoint = position - center;
    float distance = length(toPoint);
    vec3 direction = toPoint / distance;
    vec3 facing = dot(normal, direction) <= 0.0 ? normal : -normal;
    Frame frame = makeFrame(direction);
    // Occluders may shade the point from as far as this around it (tangents
    // from the light's center): those nearer the point than the light.
    float lightTangent = radius / distance;

    // Cubes are one level that sees all around; nested views from the
    // finest that sees the point.
    uint levels = light.cube != 0u ? 1u : light.viewCount;
    uint first = 0u;
    if (light.cube == 0u) {
        while (first < levels &&
               shadowReach(shadowViews[light.firstView + first], direction) <= 0.0) {
            ++first;
        }
        if (first == levels) {
            return 1.0;  // Beyond the shadows' distance.
        }
    }

    // Occluders: searched over a few texels of the finest view first (thin
    // ones), then as wide as they may be in the coarsest (big lights' wide
    // penumbras).
    uint level = first;
    ShadowView view = shadowViewOf(light, direction, level);
    float texel = shadowTexel(view);
    float search = min(min(lightTangent, kMaxSearchTexels * texel),
                       light.cube != 0u ? kFar : shadowReach(view, direction));
    vec2 blockers = shadowDisk(light, level, direction, frame, search, kBlockerTaps, position,
                               facing, kShadowBiasTexels * texel * distance, rotation);
    if (blockers.x <= 0.0 && search < lightTangent) {
        uint coarsest = levels - 1u;
        if (light.cube == 0u &&
            shadowReach(shadowViews[light.firstView + coarsest], direction) <= 0.0) {
            coarsest = level;
        }
        view = shadowViewOf(light, direction, coarsest);
        float wider =
            min(lightTangent, light.cube != 0u ? kFar : shadowReach(view, direction));
        if (wider > search) {
            level = coarsest;
            texel = shadowTexel(view);
            blockers = shadowDisk(light, level, direction, frame, wider, kBlockerTaps, position,
                                  facing, kShadowBiasTexels * texel * distance, rotation);
        }
    }
    if (blockers.x <= 0.0) {
        return 1.0;
    }
    // The penumbra: what the light's disk spans, seen past the occluders.
    // Filtered in the finest view whose texels resolve it, else where found.
    float occluder = blockers.y / blockers.x;
    float penumbra = radius * max(distance - occluder, 0.0) / (distance * occluder);
    for (uint i = first; i < levels; ++i) {
        ShadowView view = shadowViewOf(light, direction, i);
        if (penumbra <= kMaxFilterTexels * shadowTexel(view) &&
            (light.cube != 0u || shadowReach(view, direction) >= penumbra)) {
            level = i;
            break;
        }
    }
    view = shadowViewOf(light, direction, level);
    texel = shadowTexel(view);
    float reach = light.cube != 0u ? kFar : shadowReach(view, direction);
    penumbra = clamp(penumbra, kMinFilterTexels * texel, max(reach, kMinFilterTexels * texel));
    vec2 occluded = shadowDisk(light, level, direction, frame, penumbra, kFilterTaps, position,
                               facing, kShadowBiasTexels * texel * distance, rotation);
    return 1.0 - occluded.x / float(kFilterTaps);
}

#endif  // STARCORE_SHADOW_PASS

#endif  // STARCORE_SHADOWS_GLSL
