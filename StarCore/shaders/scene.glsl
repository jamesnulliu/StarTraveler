// Scene data of the GPU renderers (see starcore/gpu/gpu_scene.hpp) and the
// ray-query version of traceClosestHit (starcore/render/render_scene.cpp).
// Raster shaders define STARCORE_NO_RAY_QUERY before including it.

#ifndef STARCORE_SCENE_GLSL
#define STARCORE_SCENE_GLSL

#ifndef STARCORE_NO_RAY_QUERY
#extension GL_EXT_ray_query : require
#endif
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

#ifndef STARCORE_NO_RAY_QUERY
layout(set = 0, binding = 0) uniform accelerationStructureEXT tlas;
#endif
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

// Thin surfaces transmitting at least this much do not take the primary id.
const float kSeeThrough = 0.5;
// Preview and raster shading: ambient light and a headlight at the camera.
const float kPreviewAmbient = 0.04;
const float kPreviewHeadlight = 0.15;

Material materialOf(uint instanceIndex)
{
    return materials[instances[instanceIndex].materialIndex];
}

bool isThinTransmissive(Material m)
{
    return (m.flags & kMaterialThinWalled) != 0u && m.transmission > 0.0;
}

bool isSeeThrough(Material m)
{
    return (m.flags & kMaterialThinWalled) != 0u &&
           m.transmission * (1.0 - m.metallic) >= kSeeThrough;
}

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

/** Vertex indices of a triangle of a mesh instance. */
uvec3 triangleIndices(Instance instance, uint triangle)
{
    UintArray indices = UintArray(instance.indices);
    return uvec3(indices.values[3u * triangle], indices.values[3u * triangle + 1u],
                 indices.values[3u * triangle + 2u]);
}

/**
 * Object-space normals at a hit: geometric (face, not normalized) and shading
 * (interpolated vertex normals if the mesh has them). Spheres: `point` is the
 * object-space hit; triangles: barycentrics weight vertices 1 and 2.
 */
void objectNormals(Instance instance, uint triangle, vec2 barycentrics, vec3 point,
                   out vec3 geometric, out vec3 shading)
{
    if (instance.kind == kGeometrySphere) {
        // Unit sphere: the normal is the point.
        geometric = point;
        shading = point;
        return;
    }
    uvec3 i = triangleIndices(instance, triangle);
    vec3 v0 = loadVec3(instance.positions, i.x);
    geometric = cross(loadVec3(instance.positions, i.y) - v0,
                      loadVec3(instance.positions, i.z) - v0);
    shading = geometric;
    if (instance.normals != uvec2(0u)) {
        shading = (1.0 - barycentrics.x - barycentrics.y) * loadVec3(instance.normals, i.x) +
                  barycentrics.x * loadVec3(instance.normals, i.y) +
                  barycentrics.y * loadVec3(instance.normals, i.z);
    }
}

/**
 * SurfaceHit at distance t along a ray from object-space normals; normals
 * transform with the inverse transpose: n * W2O = transpose(W2O) n.
 */
SurfaceHit makeSurfaceHit(vec3 origin, vec3 direction, float t, uint instanceIndex,
                          vec3 objectGeometric, vec3 objectShading, mat3 worldToObject)
{
    vec3 geometric = normalize(objectGeometric * worldToObject);
    vec3 shading = objectShading * worldToObject;
    float shadingLength = length(shading);
    shading = shadingLength > 0.0 ? shading / shadingLength : geometric;

    SurfaceHit hit;
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
    return hit;
}

#ifndef STARCORE_NO_RAY_QUERY

/**
 * Closest hit in (0, tMax) among instances whose mask intersects `mask` (8
 * bits). Spheres are AABB geometry intersected here; triangles are opaque and
 * two-sided.
 */
bool traceClosestHit(vec3 origin, vec3 direction, uint mask, float tMax, out SurfaceHit hit)
{
    hit.t = 0.0;
    hit.position = vec3(0.0);
    hit.normal = vec3(0.0, 0.0, 1.0);
    hit.geometricNormal = vec3(0.0, 0.0, 1.0);
    hit.frontFace = true;
    hit.instanceIndex = 0u;

    rayQueryEXT query;
    rayQueryInitializeEXT(query, tlas, gl_RayFlagsOpaqueEXT, mask & 0xFFu, origin, 0.0, direction,
                          tMax);
    while (rayQueryProceedEXT(query)) {
        if (rayQueryGetIntersectionTypeEXT(query, false) ==
            gl_RayQueryCandidateIntersectionAABBEXT) {
            float closest = rayQueryGetIntersectionTypeEXT(query, true) ==
                                    gl_RayQueryCommittedIntersectionNoneEXT
                                ? tMax
                                : rayQueryGetIntersectionTEXT(query, true);
            float t;
            if (intersectUnitSphere(rayQueryGetIntersectionObjectRayOriginEXT(query, false),
                                    rayQueryGetIntersectionObjectRayDirectionEXT(query, false),
                                    0.0, closest, t)) {
                rayQueryGenerateIntersectionEXT(query, t);
            }
        }
    }
    if (rayQueryGetIntersectionTypeEXT(query, true) == gl_RayQueryCommittedIntersectionNoneEXT) {
        return false;
    }

    float t = rayQueryGetIntersectionTEXT(query, true);
    uint instanceIndex = uint(rayQueryGetIntersectionInstanceCustomIndexEXT(query, true));
    vec3 objectGeometric;
    vec3 objectShading;
    objectNormals(instances[instanceIndex],
                  uint(rayQueryGetIntersectionPrimitiveIndexEXT(query, true)),
                  rayQueryGetIntersectionBarycentricsEXT(query, true),
                  rayQueryGetIntersectionObjectRayOriginEXT(query, true) +
                      t * rayQueryGetIntersectionObjectRayDirectionEXT(query, true),
                  objectGeometric, objectShading);
    hit = makeSurfaceHit(origin, direction, t, instanceIndex, objectGeometric, objectShading,
                         mat3(rayQueryGetIntersectionWorldToObjectEXT(query, true)));
    return true;
}

bool traceClosestHit(vec3 origin, vec3 direction, uint mask, out SurfaceHit hit)
{
    return traceClosestHit(origin, direction, mask, kFar, hit);
}

#endif  // STARCORE_NO_RAY_QUERY

#endif  // STARCORE_SCENE_GLSL
