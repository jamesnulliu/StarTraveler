#pragma once
// Collision detection: a snapshot of a scene's colliders (like RenderScene
// for rendering) with ray, sphere-cast and overlap queries, and
// collide-and-slide movement for spherical bodies (e.g. a ship).

#include <cstdint>
#include <optional>
#include <vector>

#include <entt/entity/entity.hpp>

#include "starcore/asset/mesh.hpp"
#include "starcore/math/bvh.hpp"
#include "starcore/math/geometry.hpp"
#include "starcore/scene/components.hpp"
#include "starcore/scene/layers.hpp"

namespace starcore
{

class Scene;

/** @brief Which colliders a query sees. */
struct QueryFilter
{
    std::uint32_t layerMask = kAllLayers;  // Colliders on these layers.
    entt::entity ignore = entt::null;      // E.g. the moving body itself.
    bool triggers = false;                 // Include trigger colliders.
};

struct RaycastHit
{
    entt::entity entity{entt::null};
    float distance = 0.0F;  // In units of the ray direction's length.
    Vec3 point{0.0F};
    Vec3 normal{0.0F};  // Unit, facing the ray.
};

struct SweepHit
{
    entt::entity entity{entt::null};
    float fraction = 0.0F;  // Of the displacement, in [0, 1].
    Vec3 point{0.0F};       // Contact point on the collider.
    Vec3 normal{0.0F};      // Unit, from the collider towards the sphere.
};

struct Contact
{
    entt::entity entity{entt::null};
    Vec3 normal{0.0F};  // Unit, from the collider towards the sphere.
    float depth = 0.0F; // Penetration depth.
    bool trigger = false;
};

struct MoveResult
{
    Vec3 position{0.0F};
    bool collided = false;
    // Normals of the surfaces slid along (to remove velocity into them).
    std::vector<Vec3> normals;
    std::vector<entt::entity> touched;
};

/** @brief A collider in world space. */
struct ColliderInstance
{
    entt::entity entity{entt::null};
    ColliderShape shape = ColliderShape::Sphere;
    std::uint32_t layer = 0;
    bool trigger = false;
    Aabb worldBounds;
    // Sphere: world center and radius.
    Vec3 center{0.0F};
    float radius = 0.0F;
    // Box: world center (above), unit axes and half extents along them.
    Mat3 axes{1.0F};
    Vec3 halfExtents{0.0F};
    // Mesh: local-to-world transform and the mesh (with its BVH).
    Mat4 localToWorld{1.0F};
    Mat4 worldToLocal{1.0F};
    MeshHandle mesh;
};

class PhysicsWorld
{
public:
    /** @brief Snapshot of every ColliderComponent (mesh colliders need a MeshComponent). */
    [[nodiscard]] static auto build(const Scene& scene) -> PhysicsWorld;

    [[nodiscard]] auto colliders() const noexcept -> const std::vector<ColliderInstance>&
    {
        return m_colliders;
    }

    /** @brief Closest collider hit by a ray within maxDistance. */
    [[nodiscard]] auto raycast(const Ray& ray, float maxDistance, const QueryFilter& filter = {})
        const -> std::optional<RaycastHit>;

    /**
     * @brief First contact of a sphere moved by `displacement` (continuous:
     *        fast spheres do not tunnel through thin colliders). A sphere that
     *        already overlaps a collider only hits it when moving deeper.
     */
    [[nodiscard]] auto sphereCast(Vec3 center, float radius, Vec3 displacement,
                                  const QueryFilter& filter = {}) const
        -> std::optional<SweepHit>;

    /** @brief Colliders overlapping a sphere, with penetration normals/depths. */
    [[nodiscard]] auto overlapSphere(Vec3 center, float radius,
                                     const QueryFilter& filter = {}) const
        -> std::vector<Contact>;

    /**
     * @brief Collide-and-slide: move a sphere by `displacement`, stopping at
     *        colliders and sliding along them, then push it out of any
     *        remaining overlap. Triggers are ignored.
     */
    [[nodiscard]] auto moveSphere(Vec3 center, float radius, Vec3 displacement,
                                  const QueryFilter& filter = {}, int maxIterations = 4) const
        -> MoveResult;

private:
    template <typename Visit>
    void forCandidates(const Aabb& bounds, const QueryFilter& filter, Visit&& visit) const;

    std::vector<ColliderInstance> m_colliders;
    Bvh m_bvh;  // Over the colliders' world bounds.
};

}  // namespace starcore
