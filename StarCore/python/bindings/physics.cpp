#include <format>
#include <optional>
#include <vector>

#include <nanobind/stl/optional.h>
#include <nanobind/stl/vector.h>

#include <starcore/physics/physics_world.hpp>
#include <starcore/scene/scene.hpp>

#include "common.hpp"

namespace starcore::python
{

namespace
{

auto makeFilter(std::uint32_t layerMask, std::optional<std::uint32_t> ignore, bool triggers)
    -> QueryFilter
{
    return {.layerMask = layerMask,
            .ignore = ignore ? static_cast<entt::entity>(*ignore) : entt::entity{entt::null},
            .triggers = triggers};
}

auto handleOf(entt::entity entity) -> std::uint32_t
{
    return entt::to_integral(entity);
}

}  // namespace

void bindPhysics(nb::module_& m)
{
    nb::class_<RaycastHit>(m, "RaycastHit")
        .def_prop_ro("entity", [](const RaycastHit& h) { return handleOf(h.entity); },
                     "Entity handle (see Scene.entity_from_handle).")
        .def_ro("distance", &RaycastHit::distance)
        .def_ro("point", &RaycastHit::point)
        .def_ro("normal", &RaycastHit::normal);
    nb::class_<SweepHit>(m, "SweepHit")
        .def_prop_ro("entity", [](const SweepHit& h) { return handleOf(h.entity); })
        .def_ro("fraction", &SweepHit::fraction, "Of the displacement, in [0, 1].")
        .def_ro("point", &SweepHit::point)
        .def_ro("normal", &SweepHit::normal);
    nb::class_<Contact>(m, "Contact")
        .def_prop_ro("entity", [](const Contact& c) { return handleOf(c.entity); })
        .def_ro("normal", &Contact::normal)
        .def_ro("depth", &Contact::depth)
        .def_ro("trigger", &Contact::trigger);
    nb::class_<MoveResult>(m, "MoveResult")
        .def_ro("position", &MoveResult::position)
        .def_ro("collided", &MoveResult::collided)
        .def_ro("normals", &MoveResult::normals)
        .def_prop_ro("touched", [](const MoveResult& r) {
            std::vector<std::uint32_t> handles;
            for (const entt::entity entity : r.touched) {
                handles.push_back(handleOf(entity));
            }
            return handles;
        });

    nb::class_<PhysicsWorld>(m, "PhysicsWorld",
                             "Snapshot of a scene's colliders with collision queries. Queries "
                             "take layer_mask (e.g. Scene.layers.collision_mask(layer)), ignore "
                             "(an entity handle) and triggers.")
        .def_static("build", &PhysicsWorld::build, "scene"_a)
        .def_prop_ro("collider_count", [](const PhysicsWorld& w) { return w.colliders().size(); })
        .def(
            "raycast",
            [](const PhysicsWorld& world, const Ray& ray, float maxDistance,
               std::uint32_t layerMask, std::optional<std::uint32_t> ignore, bool triggers) {
                return world.raycast(ray, maxDistance, makeFilter(layerMask, ignore, triggers));
            },
            "ray"_a, "max_distance"_a = kInfinity, nb::kw_only(), "layer_mask"_a = kAllLayers,
            "ignore"_a = nb::none(), "triggers"_a = false)
        .def(
            "sphere_cast",
            [](const PhysicsWorld& world, Vec3 center, float radius, Vec3 displacement,
               std::uint32_t layerMask, std::optional<std::uint32_t> ignore, bool triggers) {
                return world.sphereCast(center, radius, displacement,
                                        makeFilter(layerMask, ignore, triggers));
            },
            "center"_a, "radius"_a, "displacement"_a, nb::kw_only(), "layer_mask"_a = kAllLayers,
            "ignore"_a = nb::none(), "triggers"_a = false,
            "First contact of a moving sphere (continuous: no tunneling).")
        .def(
            "overlap_sphere",
            [](const PhysicsWorld& world, Vec3 center, float radius, std::uint32_t layerMask,
               std::optional<std::uint32_t> ignore, bool triggers) {
                return world.overlapSphere(center, radius, makeFilter(layerMask, ignore, triggers));
            },
            "center"_a, "radius"_a, nb::kw_only(), "layer_mask"_a = kAllLayers,
            "ignore"_a = nb::none(), "triggers"_a = false)
        .def(
            "move_sphere",
            [](const PhysicsWorld& world, Vec3 center, float radius, Vec3 displacement,
               std::uint32_t layerMask, std::optional<std::uint32_t> ignore, int iterations) {
                return world.moveSphere(center, radius, displacement,
                                        makeFilter(layerMask, ignore, false), iterations);
            },
            "center"_a, "radius"_a, "displacement"_a, nb::kw_only(), "layer_mask"_a = kAllLayers,
            "ignore"_a = nb::none(), "max_iterations"_a = 4,
            "Collide-and-slide movement of a sphere.");
}

}  // namespace starcore::python
