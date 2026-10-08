#include <algorithm>
#include <cmath>

#include <glm/geometric.hpp>
#include <glm/matrix.hpp>

#include "starcore/physics/physics_world.hpp"
#include "starcore/scene/scene.hpp"

namespace starcore
{

namespace
{

constexpr float kContactTolerance = 1e-4F;
constexpr float kSkin = 1e-3F;  // Gap kept between a moved sphere and what it hit.
constexpr int kMaxAdvancementSteps = 64;

struct ShapeHit
{
    float fraction = 0.0F;
    Vec3 point{0.0F};
    Vec3 normal{0.0F};
};

/**
 * @brief Time of impact of a moving sphere with a convex shape by conservative
 *        advancement: the distance along a straight path to a convex set is
 *        convex in t, so Newton steps from the start never overshoot -- fast
 *        spheres cannot tunnel. `closest(p)` returns the shape's closest point.
 */
template <typename Closest>
auto sweepConvex(Vec3 start, float radius, Vec3 displacement, float maxFraction,
                 Closest&& closest) -> std::optional<ShapeHit>
{
    const float length = glm::length(displacement);
    float t = 0.0F;
    for (int step = 0; step < kMaxAdvancementSteps; ++step) {
        const Vec3 p = start + displacement * t;
        const Vec3 q = closest(p);
        const Vec3 v = p - q;
        const float distance = glm::length(v);
        if (distance <= radius + kContactTolerance) {
            const Vec3 normal = distance > 1e-6F ? v / distance : -displacement / length;
            // Touching or overlapping but not moving into it: free to go.
            if (glm::dot(displacement, normal) >= -1e-6F * length) {
                return std::nullopt;
            }
            return ShapeHit{t, q, normal};
        }
        const float approach = -glm::dot(displacement, v) / distance;
        if (approach <= 1e-9F) {
            return std::nullopt;  // Moving away: the distance never decreases again.
        }
        t += (distance - radius) / approach;
        if (t > maxFraction) {
            return std::nullopt;
        }
    }
    return std::nullopt;  // Grazing without converging: no contact.
}

auto boxBounds(Vec3 center, const Mat3& axes, Vec3 halfExtents) -> Aabb
{
    Vec3 extent{0.0F};
    for (int i = 0; i < 3; ++i) {
        extent += glm::abs(axes[i]) * halfExtents[i];
    }
    return {center - extent, center + extent};
}

auto sweptBounds(Vec3 center, float radius, Vec3 displacement) -> Aabb
{
    Aabb bounds;
    bounds.expand(center - Vec3(radius));
    bounds.expand(center + Vec3(radius));
    bounds.expand(center + displacement - Vec3(radius));
    bounds.expand(center + displacement + Vec3(radius));
    return bounds;
}

/** @brief Calls visit(a, b, c) for the world-space triangles of a mesh collider near `bounds`. */
template <typename Visit>
void forTriangles(const ColliderInstance& collider, const Aabb& bounds, Visit&& visit)
{
    const MeshData& mesh = *collider.mesh;
    mesh.bvh.query(bounds.transformed(collider.worldToLocal), [&](std::uint32_t triangle) {
        visit(Vec3(collider.localToWorld * Vec4(mesh.vertex(triangle, 0), 1.0F)),
              Vec3(collider.localToWorld * Vec4(mesh.vertex(triangle, 1), 1.0F)),
              Vec3(collider.localToWorld * Vec4(mesh.vertex(triangle, 2), 1.0F)));
    });
}

}  // namespace

auto PhysicsWorld::build(const Scene& scene) -> PhysicsWorld
{
    PhysicsWorld world;
    const auto& registry = scene.registry();
    for (const auto [entity, collider] : registry.view<const ColliderComponent>().each()) {
        const Mat4 transform = scene.worldTransform(entity);
        ColliderInstance instance{
            .entity = entity,
            .shape = collider.shape,
            .layer = scene.layerOf(entity),
            .trigger = collider.trigger,
        };
        instance.center = Vec3(transform[3]);
        const Vec3 scale{glm::length(Vec3(transform[0])), glm::length(Vec3(transform[1])),
                         glm::length(Vec3(transform[2]))};
        switch (collider.shape) {
        case ColliderShape::Sphere:
            instance.radius = collider.radius * std::max({scale.x, scale.y, scale.z});
            instance.worldBounds = {instance.center - Vec3(instance.radius),
                                    instance.center + Vec3(instance.radius)};
            break;
        case ColliderShape::Box:
            for (int i = 0; i < 3; ++i) {
                instance.axes[i] = scale[i] > 0.0F ? Vec3(transform[i]) / scale[i]
                                                   : Vec3(i == 0, i == 1, i == 2);
            }
            instance.halfExtents = collider.halfExtents * scale;
            instance.worldBounds = boxBounds(instance.center, instance.axes, instance.halfExtents);
            break;
        case ColliderShape::Mesh: {
            const auto* mesh = registry.try_get<MeshComponent>(entity);
            if (mesh == nullptr || !mesh->mesh || mesh->mesh->triangleCount() == 0) {
                continue;  // Nothing to collide with.
            }
            instance.mesh = mesh->mesh;
            instance.localToWorld = transform;
            instance.worldToLocal = glm::inverse(transform);
            instance.worldBounds = mesh->mesh->bounds.transformed(transform);
            break;
        }
        }
        world.m_colliders.push_back(std::move(instance));
    }

    std::vector<Aabb> bounds;
    bounds.reserve(world.m_colliders.size());
    for (const ColliderInstance& collider : world.m_colliders) {
        bounds.push_back(collider.worldBounds);
    }
    world.m_bvh = Bvh::build(bounds, {.maxLeafSize = 2});
    return world;
}

template <typename Visit>
void PhysicsWorld::forCandidates(const Aabb& bounds, const QueryFilter& filter,
                                 Visit&& visit) const
{
    m_bvh.query(bounds, [&](std::uint32_t index) {
        const ColliderInstance& collider = m_colliders[index];
        if (!overlaps(collider.worldBounds, bounds) || collider.entity == filter.ignore ||
            (filter.layerMask & layerBit(collider.layer)) == 0U ||
            (collider.trigger && !filter.triggers)) {
            return;
        }
        visit(collider);
    });
}

auto PhysicsWorld::raycast(const Ray& ray, float maxDistance, const QueryFilter& filter) const
    -> std::optional<RaycastHit>
{
    std::optional<RaycastHit> best;
    float closest = maxDistance;
    m_bvh.traverse(ray, 0.0F, closest, [&](std::uint32_t index, float& tMax) {
        const ColliderInstance& collider = m_colliders[index];
        if (collider.entity == filter.ignore ||
            (filter.layerMask & layerBit(collider.layer)) == 0U ||
            (collider.trigger && !filter.triggers)) {
            return;
        }
        auto accept = [&](float t, Vec3 normal) {
            tMax = t;
            normal = glm::normalize(normal);
            best = RaycastHit{collider.entity, t, ray.at(t),
                              glm::dot(normal, ray.direction) > 0.0F ? -normal : normal};
        };
        switch (collider.shape) {
        case ColliderShape::Sphere:
            if (glm::distance(ray.origin, collider.center) < collider.radius) {
                return;  // Rays starting inside a collider do not hit it.
            }
            if (const auto t = intersectSphere(ray, collider.center, collider.radius, 0.0F, tMax)) {
                accept(*t, ray.at(*t) - collider.center);
            }
            break;
        case ColliderShape::Box: {
            const Mat3 toBox = glm::transpose(collider.axes);
            const Ray local{toBox * (ray.origin - collider.center), toBox * ray.direction};
            const Vec3& h = collider.halfExtents;
            if (glm::all(glm::lessThanEqual(glm::abs(local.origin), h))) {
                return;  // Starts inside.
            }
            const Vec3 inverse = 1.0F / local.direction;
            const Vec3 t0 = (-h - local.origin) * inverse;
            const Vec3 t1 = (h - local.origin) * inverse;
            const Vec3 tNear = glm::min(t0, t1);
            const Vec3 tFar = glm::max(t0, t1);
            const float entry = std::max(tNear.x, std::max(tNear.y, tNear.z));
            const float exit = std::min(tFar.x, std::min(tFar.y, tFar.z));
            if (entry <= exit && entry > 0.0F && entry < tMax) {
                const int axis = entry == tNear.x ? 0 : (entry == tNear.y ? 1 : 2);
                accept(entry, collider.axes[axis] * -std::copysign(1.0F, local.direction[axis]));
            }
            break;
        }
        case ColliderShape::Mesh: {
            const Ray local{Vec3(collider.worldToLocal * Vec4(ray.origin, 1.0F)),
                            Vec3(collider.worldToLocal * Vec4(ray.direction, 0.0F))};
            const MeshData& mesh = *collider.mesh;
            std::optional<std::uint32_t> hitTriangle;
            mesh.bvh.traverse(local, 0.0F, tMax, [&](std::uint32_t triangle, float& t) {
                if (const auto hit = intersectTriangle(local, mesh.vertex(triangle, 0),
                                                       mesh.vertex(triangle, 1),
                                                       mesh.vertex(triangle, 2), 0.0F, t)) {
                    t = hit->t;
                    hitTriangle = triangle;
                }
            });
            if (hitTriangle) {
                const Vec3 a = mesh.vertex(*hitTriangle, 0);
                const Vec3 normal = glm::transpose(Mat3(collider.worldToLocal)) *
                                    glm::cross(mesh.vertex(*hitTriangle, 1) - a,
                                               mesh.vertex(*hitTriangle, 2) - a);
                accept(tMax, normal);
            }
            break;
        }
        }
    });
    return best;
}

auto PhysicsWorld::sphereCast(Vec3 center, float radius, Vec3 displacement,
                              const QueryFilter& filter) const -> std::optional<SweepHit>
{
    if (glm::dot(displacement, displacement) <= 0.0F) {
        return std::nullopt;
    }
    std::optional<SweepHit> best;
    auto consider = [&](const ColliderInstance& collider, const std::optional<ShapeHit>& hit) {
        if (hit && (!best || hit->fraction < best->fraction)) {
            best = SweepHit{collider.entity, hit->fraction, hit->point, hit->normal};
        }
    };
    const Aabb bounds = sweptBounds(center, radius, displacement);
    forCandidates(bounds, filter, [&](const ColliderInstance& collider) {
        const float maxFraction = best ? best->fraction : 1.0F;
        switch (collider.shape) {
        case ColliderShape::Sphere:
            // Exact solid sphere: closest point = center + direction * radius (or p inside).
            consider(collider,
                     sweepConvex(center, radius, displacement, maxFraction, [&](Vec3 p) {
                         const Vec3 offset = p - collider.center;
                         const float distance = glm::length(offset);
                         return distance <= collider.radius
                                    ? p
                                    : collider.center + offset * (collider.radius / distance);
                     }));
            break;
        case ColliderShape::Box: {
            const Mat3 toBox = glm::transpose(collider.axes);
            consider(collider,
                     sweepConvex(center, radius, displacement, maxFraction, [&](Vec3 p) {
                         return collider.center +
                                collider.axes * closestPointOnBox(toBox * (p - collider.center),
                                                                  collider.halfExtents);
                     }));
            break;
        }
        case ColliderShape::Mesh:
            forTriangles(collider, bounds, [&](Vec3 a, Vec3 b, Vec3 c) {
                consider(collider, sweepConvex(center, radius, displacement,
                                               best ? best->fraction : 1.0F, [&](Vec3 p) {
                                                   return closestPointOnTriangle(p, a, b, c);
                                               }));
            });
            break;
        }
    });
    return best;
}

auto PhysicsWorld::overlapSphere(Vec3 center, float radius, const QueryFilter& filter) const
    -> std::vector<Contact>
{
    std::vector<Contact> contacts;
    const Aabb bounds{center - Vec3(radius), center + Vec3(radius)};
    forCandidates(bounds, filter, [&](const ColliderInstance& collider) {
        std::optional<Contact> contact;
        switch (collider.shape) {
        case ColliderShape::Sphere: {
            const Vec3 offset = center - collider.center;
            const float distance = glm::length(offset);
            if (distance < radius + collider.radius) {
                contact = Contact{.normal = distance > 1e-6F ? offset / distance : Vec3(0, 1, 0),
                                  .depth = radius + collider.radius - distance};
            }
            break;
        }
        case ColliderShape::Box: {
            const Vec3 local = glm::transpose(collider.axes) * (center - collider.center);
            const Vec3& h = collider.halfExtents;
            const Vec3 q = closestPointOnBox(local, h);
            const Vec3 v = local - q;
            const float distance = glm::length(v);
            if (distance > 1e-6F) {
                if (distance < radius) {
                    contact = Contact{.normal = collider.axes * (v / distance),
                                      .depth = radius - distance};
                }
            } else {
                // Center inside the box: leave through the nearest face.
                const Vec3 gap = h - glm::abs(local);
                const int axis = gap.x <= gap.y && gap.x <= gap.z ? 0 : (gap.y <= gap.z ? 1 : 2);
                contact = Contact{
                    .normal = collider.axes[axis] * std::copysign(1.0F, local[axis]),
                    .depth = radius + gap[axis]};
            }
            break;
        }
        case ColliderShape::Mesh:
            forTriangles(collider, bounds, [&](Vec3 a, Vec3 b, Vec3 c) {
                const Vec3 q = closestPointOnTriangle(center, a, b, c);
                const Vec3 v = center - q;
                const float distance = glm::length(v);
                if (distance >= radius || (contact && radius - distance <= contact->depth)) {
                    return;
                }
                Vec3 normal = distance > 1e-6F ? v / distance
                                               : glm::normalize(glm::cross(b - a, c - a));
                contact = Contact{.normal = normal, .depth = radius - distance};
            });
            break;
        }
        if (contact) {
            contact->entity = collider.entity;
            contact->trigger = collider.trigger;
            contacts.push_back(*contact);
        }
    });
    return contacts;
}

auto PhysicsWorld::moveSphere(Vec3 center, float radius, Vec3 displacement,
                              const QueryFilter& filter, int maxIterations) const -> MoveResult
{
    QueryFilter solid = filter;
    solid.triggers = false;
    MoveResult result{.position = center};
    Vec3 remaining = displacement;
    for (int iteration = 0; iteration < maxIterations; ++iteration) {
        if (glm::dot(remaining, remaining) <= 1e-12F) {
            break;
        }
        const auto hit = sphereCast(result.position, radius, remaining, solid);
        if (!hit) {
            result.position += remaining;
            break;
        }
        result.collided = true;
        result.normals.push_back(hit->normal);
        if (std::ranges::find(result.touched, hit->entity) == result.touched.end()) {
            result.touched.push_back(hit->entity);
        }
        // Advance to the contact, keep a small gap, slide along the surface.
        result.position += remaining * hit->fraction + hit->normal * kSkin;
        remaining *= 1.0F - hit->fraction;
        if (const float into = glm::dot(remaining, hit->normal); into < 0.0F) {
            remaining -= hit->normal * into;
        }
    }

    // Resolve what is left of overlaps (e.g. a collider moved onto the sphere).
    for (int iteration = 0; iteration < 4; ++iteration) {
        const auto contacts = overlapSphere(result.position, radius, solid);
        const auto deepest = std::ranges::max_element(contacts, {}, &Contact::depth);
        if (deepest == contacts.end() || deepest->depth <= kContactTolerance) {
            break;
        }
        result.position += deepest->normal * (deepest->depth + kSkin);
        result.collided = true;
        result.normals.push_back(deepest->normal);
        if (std::ranges::find(result.touched, deepest->entity) == result.touched.end()) {
            result.touched.push_back(deepest->entity);
        }
    }
    return result;
}

}  // namespace starcore
