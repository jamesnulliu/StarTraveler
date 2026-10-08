#pragma once
// Flattened, render-ready snapshot of a Scene.
//
// The layout mirrors what the planned Vulkan ray-query renderer needs:
//   - geometries  ~ BLAS (a shared MeshData with its BVH, or the procedural
//                   unit sphere = AABB [-1, 1]^3),
//   - instances   ~ TLAS entries (object<->world matrices, geometry/material/
//                   entity ids and an 8-bit visibility mask),
//   - tlas        ~ the top-level acceleration structure over the instances,
//   - materials   ~ a storage buffer indexed by instance.
// The CPU path tracer consumes exactly this data, so CPU and GPU results can
// be compared image-for-image later.

#include <cstdint>
#include <optional>
#include <vector>

#include "starcore/asset/mesh.hpp"
#include "starcore/math/bvh.hpp"
#include "starcore/math/geometry.hpp"
#include "starcore/math/types.hpp"

namespace starcore
{

class Scene;

/** @brief Ray/instance mask matching every instance (Vulkan cull masks are 8 bits). */
inline constexpr std::uint32_t kAllInstances = 0xFFU;

/** @brief RenderMaterial::flags bits. */
inline constexpr std::uint32_t kMaterialThinWalled = 1U << 0U;

struct RenderMaterial
{
    Vec4 albedo{0.8F, 0.8F, 0.8F, 1.0F};
    Vec4 emission{0.0F};  // rgb = color * power
    float roughness = 1.0F;
    float metallic = 0.0F;
    float transmission = 0.0F;
    float ior = 1.5F;
    std::uint32_t flags = 0;
    std::uint32_t padding[3] = {};
};
static_assert(sizeof(RenderMaterial) == 64, "std430-compatible layout");

/**
 * @brief An emissive sphere sampled directly by the path tracer (next event
 *        estimation). Other emitters (meshes, ellipsoids) are only found by
 *        BSDF sampling.
 */
struct RenderLight
{
    Vec3 center{0.0F};
    float radius = 0.0F;
    Vec3 emission{0.0F};
    std::uint32_t instanceIndex = 0;
};

enum class GeometryKind : std::uint32_t
{
    Triangles = 0,
    Sphere = 1,  // Unit sphere at the origin.
};

struct RenderGeometry
{
    GeometryKind kind = GeometryKind::Triangles;
    MeshHandle mesh;  // Triangles only.
    Aabb localBounds;
};

struct RenderInstance
{
    Mat4 objectToWorld{1.0F};
    Mat4 worldToObject{1.0F};
    Aabb worldBounds;
    std::uint32_t geometryIndex = 0;
    std::uint32_t materialIndex = 0;
    std::uint32_t entityId = 0;  // entt::to_integral of the source entity.
    std::uint32_t mask = kAllInstances;
};

struct RenderScene
{
    std::vector<RenderGeometry> geometries;  // Meshes are deduplicated.
    std::vector<RenderInstance> instances;
    std::vector<RenderMaterial> materials;
    std::vector<RenderLight> lights;
    // Per instance: index into `lights`, or -1.
    std::vector<std::int32_t> lightOfInstance;
    Bvh tlas;  // Over instances[i].worldBounds.
    Vec3 skyColor{0.0F};
    // Scene::version() at build time (to detect stale accumulation).
    std::uint64_t sceneVersion = 0;

    [[nodiscard]] auto triangleCount() const -> std::size_t;
};

/**
 * @brief Gather every entity with a SphereComponent or MeshComponent (using
 *        its world transform and MaterialComponent, or a default material)
 *        and build the TLAS.
 */
[[nodiscard]] auto buildRenderScene(const Scene& scene) -> RenderScene;

/**
 * @brief Hash of everything that affects ray tracing (geometry identities,
 *        instances, materials, sky), excluding sceneVersion. Assign it to
 *        sceneVersion to keep accumulating while only non-traced data (2D
 *        shapes, names) changes.
 */
[[nodiscard]] auto hashRenderScene(const RenderScene& scene) -> std::uint64_t;

struct SurfaceHit
{
    float t = 0.0F;
    Vec3 position{0.0F};
    // Shading normal (interpolated vertex normals when the mesh has them),
    // unit length, on the side of the incoming ray (surfaces are two-sided).
    Vec3 normal{0.0F};
    // Geometric (face) normal, unit length, on the side of the incoming ray.
    Vec3 geometricNormal{0.0F};
    // The ray hit the side the face/outward normal points to.
    bool frontFace = true;
    std::uint32_t instanceIndex = 0;
};

/**
 * @brief Closest hit in (tMin, tMax) among instances whose mask intersects
 *        `mask` -- the CPU equivalent of a Vulkan ray query (TLAS -> BLAS).
 */
[[nodiscard]] auto traceClosestHit(const RenderScene& scene, const Ray& ray, float tMin,
                                   float tMax, std::uint32_t mask = kAllInstances)
    -> std::optional<SurfaceHit>;

/** @brief Whether anything is hit in (tMin, tMax) (shadow rays). */
[[nodiscard]] auto traceAnyHit(const RenderScene& scene, const Ray& ray, float tMin,
                               float tMax, std::uint32_t mask = kAllInstances) -> bool;

}  // namespace starcore
