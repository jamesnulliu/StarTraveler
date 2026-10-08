// Scene data of the GPU renderers (see starcore/gpu/gpu_scene.hpp) and the
// ray-query version of traceClosestHit (starcore/render/render_scene.cpp).

#ifndef STARCORE_SCENE_GLSL
#define STARCORE_SCENE_GLSL

#extension GL_EXT_ray_query : require
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_buffer_reference_uvec2 : require

#include "common.glsl"

const uint kGeometryTriangles = 0u;
const uint kGeometrySphere = 1u;
const uint kMaterialThinWalled = 1u;

/** gpu::InstanceRecord (40 bytes). */
struct Instance
{
    uvec2 positions;  // Buffer device addresses (0 for spheres / meshes without normals).
    uvec2 normals;
    uvec2 indices;
    uint materialIndex;
    uint entityId;
    int lightIndex;  // Index into lights, or -1.
    uint kind;
};

/** RenderMaterial (64 bytes). */
struct Material
{
    vec4 albedo;
    vec4 emission;  // rgb = color * power
    float roughness;
    float metallic;
    float transmission;
    float ior;
    uint flags;
    uint padding0;
    uint padding1;
    uint padding2;
};

/** RenderLight (32 bytes): an emissive sphere sampled by next event estimation. */
struct Light
{
    vec3 center;
    float radius;
    vec3 emission;
    uint instanceIndex;
};

layout(set = 0, binding = 0) uniform accelerationStructureEXT tlas;
layout(set = 0, binding = 4, std430) readonly buffer Instances
{
    Instance instances[];
};
layout(set = 0, binding = 5, std430) readonly buffer Materials
{
    Material materials[];
};
layout(set = 0, binding = 6, std430) readonly buffer Lights
{
    Light lights[];
};

layout(buffer_reference, std430, buffer_reference_align = 4) readonly buffer FloatArray
{
    float values[];
};
layout(buffer_reference, std430, buffer_reference_align = 4) readonly buffer UintArray
{
    uint values[];
};

struct SurfaceHit
{
    float t;
    vec3 position;
    // Shading normal, unit length, on the side of the incoming ray.
    vec3 normal;
    // Geometric normal, unit length, on the side of the incoming ray.
    vec3 geometricNormal;
    bool frontFace;
    uint instanceIndex;
};

/** Nearest root in (tMin, tMax) of a ray (direction not normalized) and the unit sphere. */
bool intersectUnitSphere(vec3 origin, vec3 direction, float tMin, float tMax, out float t)
{
    float a = dot(direction, direction);
    float halfB = dot(origin, direction);
    float c = dot(origin, origin) - 1.0;
    float discriminant = halfB * halfB - a * c;
    t = 0.0;
    if (discriminant < 0.0) {
        return false;
    }
    float sqrtD = sqrt(discriminant);
    t = (-halfB - sqrtD) / a;
    if (t <= tMin || t >= tMax) {
        t = (-halfB + sqrtD) / a;
        if (t <= tMin || t >= tMax) {
            return false;
        }
    }
    return true;
}

vec3 loadVec3(uvec2 address, uint index)
{
    FloatArray array = FloatArray(address);
    return vec3(array.values[3u * index], array.values[3u * index + 1u],
                array.values[3u * index + 2u]);
}

/**
 * Closest hit among instances whose mask intersects `mask` (8 bits). Spheres
 * are AABB geometry intersected here; triangles are opaque and two-sided.
 */
bool traceClosestHit(vec3 origin, vec3 direction, uint mask, out SurfaceHit hit)
{
    hit.t = 0.0;
    hit.position = vec3(0.0);
    hit.normal = vec3(0.0, 0.0, 1.0);
    hit.geometricNormal = vec3(0.0, 0.0, 1.0);
    hit.frontFace = true;
    hit.instanceIndex = 0u;

    rayQueryEXT query;
    rayQueryInitializeEXT(query, tlas, gl_RayFlagsOpaqueEXT, mask & 0xFFu, origin, 0.0, direction,
                          kFar);
    while (rayQueryProceedEXT(query)) {
        if (rayQueryGetIntersectionTypeEXT(query, false) ==
            gl_RayQueryCandidateIntersectionAABBEXT) {
            float tMax = rayQueryGetIntersectionTypeEXT(query, true) ==
                                 gl_RayQueryCommittedIntersectionNoneEXT
                             ? kFar
                             : rayQueryGetIntersectionTEXT(query, true);
            float t;
            if (intersectUnitSphere(rayQueryGetIntersectionObjectRayOriginEXT(query, false),
                                    rayQueryGetIntersectionObjectRayDirectionEXT(query, false),
                                    0.0, tMax, t)) {
                rayQueryGenerateIntersectionEXT(query, t);
            }
        }
    }
    if (rayQueryGetIntersectionTypeEXT(query, true) == gl_RayQueryCommittedIntersectionNoneEXT) {
        return false;
    }

    float t = rayQueryGetIntersectionTEXT(query, true);
    uint instanceIndex = uint(rayQueryGetIntersectionInstanceCustomIndexEXT(query, true));
    Instance instance = instances[instanceIndex];

    vec3 objectGeometric;
    vec3 objectShading;
    if (instance.kind == kGeometrySphere) {
        // Unit sphere: the normal is the point.
        objectGeometric = rayQueryGetIntersectionObjectRayOriginEXT(query, true) +
                          t * rayQueryGetIntersectionObjectRayDirectionEXT(query, true);
        objectShading = objectGeometric;
    } else {
        uint triangle = uint(rayQueryGetIntersectionPrimitiveIndexEXT(query, true));
        vec2 barycentrics = rayQueryGetIntersectionBarycentricsEXT(query, true);
        UintArray indices = UintArray(instance.indices);
        uint i0 = indices.values[3u * triangle];
        uint i1 = indices.values[3u * triangle + 1u];
        uint i2 = indices.values[3u * triangle + 2u];
        vec3 v0 = loadVec3(instance.positions, i0);
        objectGeometric = cross(loadVec3(instance.positions, i1) - v0,
                                loadVec3(instance.positions, i2) - v0);
        objectShading = objectGeometric;
        if (instance.normals != uvec2(0u)) {
            objectShading = (1.0 - barycentrics.x - barycentrics.y) *
                                loadVec3(instance.normals, i0) +
                            barycentrics.x * loadVec3(instance.normals, i1) +
                            barycentrics.y * loadVec3(instance.normals, i2);
        }
    }

    // Normals transform with the inverse transpose: n * W2O = transpose(W2O) n.
    mat3 worldToObject = mat3(rayQueryGetIntersectionWorldToObjectEXT(query, true));
    vec3 geometric = normalize(objectGeometric * worldToObject);
    vec3 shading = objectShading * worldToObject;
    float shadingLength = length(shading);
    shading = shadingLength > 0.0 ? shading / shadingLength : geometric;

    hit.t = t;
    hit.position = origin + direction * t;
    hit.instanceIndex = instanceIndex;
    hit.frontFace = dot(direction, geometric) < 0.0;
    if (!hit.frontFace) {
        geometric = -geometric;
        shading = -shading;
    }
    // Interpolated normals may face away from the ray near silhouettes.
    if (dot(shading, direction) >= 0.0) {
        shading = geometric;
    }
    hit.geometricNormal = geometric;
    hit.normal = shading;
    return true;
}

#endif  // STARCORE_SCENE_GLSL
