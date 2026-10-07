#pragma once
// Flattened, render-ready snapshot of a Scene.
//
// The layout mirrors what the planned Vulkan ray-query renderer needs:
//   - geometries  ~ BLAS inputs (a triangle range in the shared vertex/index
//                   buffers, or a procedural unit sphere = AABB [-1, 1]^3),
//   - instances   ~ TLAS entries (object<->world matrices + geometry,
//                   material and entity ids),
//   - materials   ~ a storage buffer indexed by instance.
// The CPU path tracer consumes exactly this data, so CPU and GPU results can
// be compared image-for-image later.

#include <cstdint>
#include <optional>
#include <vector>

#include "starcore/math/geometry.hpp"
#include "starcore/math/types.hpp"

namespace starcore
{

class Scene;

struct RenderMaterial
{
    Vec4 albedo{0.8F, 0.8F, 0.8F, 1.0F};
    Vec4 emission{0.0F};  // rgb = color * power
    float roughness = 1.0F;
    float metallic = 0.0F;
    float padding0 = 0.0F;
    float padding1 = 0.0F;
};
static_assert(sizeof(RenderMaterial) == 48, "std430-compatible layout");

enum class GeometryKind : std::uint32_t
{
    Triangles = 0,
    Sphere = 1,  // Unit sphere at the origin.
};

struct RenderGeometry
{
    GeometryKind kind = GeometryKind::Triangles;
    std::uint32_t firstIndex = 0;  // Into RenderScene::indices.
    std::uint32_t indexCount = 0;  // 3 per triangle.
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
};

struct RenderScene
{
    std::vector<Vec3> positions;
    std::vector<std::uint32_t> indices;
    std::vector<RenderGeometry> geometries;
    std::vector<RenderInstance> instances;
    std::vector<RenderMaterial> materials;
    Vec3 skyColor{0.0F};
    // Scene::version() at build time (to detect stale accumulation).
    std::uint64_t sceneVersion = 0;
};

/**
 * @brief Gather every entity with a SphereComponent or MeshComponent (using
 *        its world transform and MaterialComponent, or a default material).
 */
[[nodiscard]] auto buildRenderScene(const Scene& scene) -> RenderScene;

/**
 * @brief Hash of everything that affects ray tracing (geometry, instances,
 *        materials, sky), excluding sceneVersion. Assign it to sceneVersion
 *        to keep accumulating while only non-traced data (2D shapes, names)
 *        changes.
 */
[[nodiscard]] auto hashRenderScene(const RenderScene& scene) -> std::uint64_t;

struct SurfaceHit
{
    float t = 0.0F;
    Vec3 position{0.0F};
    // Unit normal facing against the incoming ray (surfaces are two-sided).
    Vec3 normal{0.0F};
    bool frontFace = true;
    std::uint32_t instanceIndex = 0;
};

/**
 * @brief Closest hit in (tMin, tMax) -- the CPU equivalent of a Vulkan ray
 *        query against the TLAS. Brute force over instances with AABB culling.
 */
[[nodiscard]] auto traceClosestHit(const RenderScene& scene, const Ray& ray,
                                   float tMin, float tMax)
    -> std::optional<SurfaceHit>;

}  // namespace starcore
