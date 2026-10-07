#include <filesystem>
#include <format>
#include <optional>
#include <ranges>
#include <string>
#include <unordered_map>
#include <vector>

#include <nanobind/stl/filesystem.h>
#include <nanobind/stl/optional.h>
#include <nanobind/stl/string.h>
#include <nanobind/stl/vector.h>
#include <nanobind/typing.h>

#include <starcore/scene/entity.hpp>
#include <starcore/scene/serializer.hpp>

#include "common.hpp"

namespace starcore::python
{

namespace
{

/**
 * @brief Python-side entity: the C++ handle plus a reference to the Python
 *        Scene object, so a scene lives as long as any of its entities.
 */
struct PyEntity
{
    Entity entity;
    nb::object owner;

    [[nodiscard]] auto checked() const -> const Entity&
    {
        if (!entity.isValid()) {
            throw nb::value_error("the entity was destroyed");
        }
        return entity;
    }
};

auto wrap(Scene& scene, Entity entity) -> std::optional<PyEntity>
{
    if (!entity.isValid()) {
        return std::nullopt;
    }
    return PyEntity{entity, nb::find(scene)};
}

auto unwrapOptional(Scene& scene, const std::optional<PyEntity>& entity) -> Entity
{
    if (!entity) {
        return {};
    }
    if (entity->entity.scene() != &scene) {
        throw nb::value_error("the entity belongs to another scene");
    }
    return entity->checked();
}

// Generic component access: Python type object -> operations on that component type.
struct ComponentOps
{
    auto (*has)(const Entity&) -> bool;
    auto (*get)(const Entity&) -> nb::object;  // A copy, or None.
    void (*set)(const Entity&, nb::handle);    // Add or replace.
    void (*remove)(const Entity&);
};

auto componentRegistry() -> std::vector<std::pair<PyObject*, ComponentOps>>&
{
    static std::vector<std::pair<PyObject*, ComponentOps>> registry;
    return registry;
}

template <typename T>
void registerComponent()
{
    componentRegistry().emplace_back(
        nb::type<T>().ptr(),
        ComponentOps{
            .has = [](const Entity& e) { return e.has<T>(); },
            .get = [](const Entity& e) -> nb::object {
                const T* component = e.tryGet<T>();
                return component != nullptr ? nb::cast(*component, nb::rv_policy::copy)
                                            : nb::none();
            },
            .set = [](const Entity& e,
                      nb::handle value) { e.addOrReplace<T>(nb::cast<const T&>(value)); },
            .remove = [](const Entity& e) { e.remove<T>(); },
        });
}

auto opsForType(nb::handle type) -> const ComponentOps&
{
    for (const auto& [pythonType, ops] : componentRegistry()) {
        if (pythonType == type.ptr()) {
            return ops;
        }
    }
    throw nb::type_error(
        std::format("{} is not a StarCore component type",
                    nb::cast<std::string>(nb::str(type)))
            .c_str());
}

auto opsForValue(nb::handle value) -> const ComponentOps&
{
    return opsForType(value.type());
}

/** @brief `__repr__` from the given fields, e.g. "Sphere(radius=1.0)". */
auto describe(const char* type, nb::handle self, std::initializer_list<const char*> fields)
    -> std::string
{
    std::string text = std::string(type) + "(";
    bool first = true;
    for (const char* field : fields) {
        text += std::format("{}{}={}", first ? "" : ", ", field,
                            nb::cast<std::string>(nb::repr(self.attr(field))));
        first = false;
    }
    return text + ")";
}

template <typename E>
void bindEnum(nb::module_& m, const char* name, std::initializer_list<const char*> names)
{
    nb::enum_<E> binding(m, name);
    std::size_t index = 0;
    for (const char* value : names) {
        binding.value(value, static_cast<E>(index++));
    }
}

void bindComponents(nb::module_& m)
{
    bindEnum<LinePattern>(m, "LinePattern", {"SOLID", "DASH", "DOT"});
    bindEnum<LineAlgorithm>(m, "LineAlgorithm", {"BRESENHAM", "MIDPOINT", "DDA"});
    bindEnum<EllipseAlgorithm>(m, "EllipseAlgorithm", {"PARAMETRIC", "MIDPOINT"});
    bindEnum<CurveType>(m, "CurveType", {"BSPLINE", "BEZIER"});

    {
        const TransformComponent d{};
        nb::class_<TransformComponent>(m, "Transform", "Local transform (relative to the parent).")
            .def(
                "__init__",
                [](TransformComponent* self, Vec3 translation, Quat rotation, Vec3 scale) {
                    new (self) TransformComponent{translation, rotation, scale};
                },
                nb::kw_only(), "translation"_a = d.translation, "rotation"_a = d.rotation,
                "scale"_a = d.scale)
            .def_rw("translation", &TransformComponent::translation)
            .def_rw("rotation", &TransformComponent::rotation, "Quaternion (w, x, y, z).")
            .def_rw("scale", &TransformComponent::scale)
            .def("matrix", [](const TransformComponent& t) { return toArray(t.matrix()); })
            .def("__repr__", [](nb::handle self) {
                return describe("Transform", self, {"translation", "rotation", "scale"});
            });
    }
    {
        const MaterialComponent d{};
        nb::class_<MaterialComponent>(m, "Material")
            .def(
                "__init__",
                [](MaterialComponent* self, Vec3 albedo, float roughness, float metallic,
                   Vec3 emissionColor, float emissionPower) {
                    new (self) MaterialComponent{albedo, roughness, metallic, emissionColor,
                                                 emissionPower};
                },
                nb::kw_only(), "albedo"_a = d.albedo, "roughness"_a = d.roughness,
                "metallic"_a = d.metallic, "emission_color"_a = d.emissionColor,
                "emission_power"_a = d.emissionPower)
            .def_rw("albedo", &MaterialComponent::albedo)
            .def_rw("roughness", &MaterialComponent::roughness)
            .def_rw("metallic", &MaterialComponent::metallic)
            .def_rw("emission_color", &MaterialComponent::emissionColor)
            .def_rw("emission_power", &MaterialComponent::emissionPower)
            .def_prop_ro("emission", &MaterialComponent::emission,
                         "emission_color * emission_power")
            .def("__repr__", [](nb::handle self) {
                return describe("Material", self,
                                {"albedo", "roughness", "metallic", "emission_color",
                                 "emission_power"});
            });
    }
    nb::class_<SphereComponent>(m, "Sphere", "Sphere centered at the entity origin.")
        .def(
            "__init__",
            [](SphereComponent* self, float radius) { new (self) SphereComponent{radius}; },
            "radius"_a = SphereComponent{}.radius)
        .def_rw("radius", &SphereComponent::radius)
        .def("__repr__",
             [](nb::handle self) { return describe("Sphere", self, {"radius"}); });

    nb::class_<MeshComponent>(m, "Mesh", "Triangle list in local space (double-sided).")
        .def(
            "__init__",
            [](MeshComponent* self, std::vector<Vec3> positions,
               std::vector<std::uint32_t> indices) {
                new (self) MeshComponent{std::move(positions), std::move(indices)};
            },
            "positions"_a = std::vector<Vec3>{}, "indices"_a = std::vector<std::uint32_t>{})
        .def_rw("positions", &MeshComponent::positions)
        .def_rw("indices", &MeshComponent::indices)
        .def("__repr__", [](const MeshComponent& mesh) {
            return std::format("Mesh({} vertices, {} triangles)", mesh.positions.size(),
                               mesh.indices.size() / 3);
        });

    {
        const StrokeComponent d{};
        nb::class_<StrokeComponent>(m, "Stroke", "Color/thickness/pattern of 2D shapes.")
            .def(
                "__init__",
                [](StrokeComponent* self, Vec4 color, std::uint32_t thickness,
                   LinePattern pattern) { new (self) StrokeComponent{color, thickness, pattern}; },
                nb::kw_only(), "color"_a = d.color, "thickness"_a = d.thickness,
                "pattern"_a = d.pattern)
            .def_rw("color", &StrokeComponent::color)
            .def_rw("thickness", &StrokeComponent::thickness)
            .def_rw("pattern", &StrokeComponent::pattern)
            .def("__repr__", [](nb::handle self) {
                return describe("Stroke", self, {"color", "thickness", "pattern"});
            });
    }
    {
        const LineShapeComponent d{};
        nb::class_<LineShapeComponent>(m, "LineShape")
            .def(
                "__init__",
                [](LineShapeComponent* self, Vec3 start, Vec3 end, LineAlgorithm algorithm) {
                    new (self) LineShapeComponent{start, end, algorithm};
                },
                "start"_a = d.start, "end"_a = d.end, nb::kw_only(),
                "algorithm"_a = d.algorithm)
            .def_rw("start", &LineShapeComponent::start)
            .def_rw("end", &LineShapeComponent::end)
            .def_rw("algorithm", &LineShapeComponent::algorithm)
            .def("__repr__", [](nb::handle self) {
                return describe("LineShape", self, {"start", "end", "algorithm"});
            });
    }
    {
        const EllipseShapeComponent d{};
        nb::class_<EllipseShapeComponent>(m, "EllipseShape",
                                          "Ellipse in the local XY plane, centered at the origin.")
            .def(
                "__init__",
                [](EllipseShapeComponent* self, float radiusX, float radiusY,
                   EllipseAlgorithm algorithm) {
                    new (self) EllipseShapeComponent{radiusX, radiusY, algorithm};
                },
                "radius_x"_a = d.radiusX, "radius_y"_a = d.radiusY, nb::kw_only(),
                "algorithm"_a = d.algorithm)
            .def_rw("radius_x", &EllipseShapeComponent::radiusX)
            .def_rw("radius_y", &EllipseShapeComponent::radiusY)
            .def_rw("algorithm", &EllipseShapeComponent::algorithm)
            .def("__repr__", [](nb::handle self) {
                return describe("EllipseShape", self, {"radius_x", "radius_y", "algorithm"});
            });
    }
    {
        const ArcShapeComponent d{};
        nb::class_<ArcShapeComponent>(m, "ArcShape",
                                      "Circular arc in the local XY plane (angles in radians).")
            .def(
                "__init__",
                [](ArcShapeComponent* self, float radius, float startAngle, float sweepAngle) {
                    new (self) ArcShapeComponent{radius, startAngle, sweepAngle};
                },
                "radius"_a = d.radius, "start_angle"_a = d.startAngle,
                "sweep_angle"_a = d.sweepAngle)
            .def_rw("radius", &ArcShapeComponent::radius)
            .def_rw("start_angle", &ArcShapeComponent::startAngle)
            .def_rw("sweep_angle", &ArcShapeComponent::sweepAngle)
            .def("__repr__", [](nb::handle self) {
                return describe("ArcShape", self, {"radius", "start_angle", "sweep_angle"});
            });
    }
    {
        const CurveShapeComponent d{};
        nb::class_<CurveShapeComponent>(m, "CurveShape")
            .def(
                "__init__",
                [](CurveShapeComponent* self, std::vector<Vec3> controlPoints, CurveType type,
                   std::uint32_t order) {
                    new (self) CurveShapeComponent{std::move(controlPoints), type, order};
                },
                "control_points"_a = std::vector<Vec3>{}, nb::kw_only(), "type"_a = d.type,
                "order"_a = d.order)
            .def_rw("control_points", &CurveShapeComponent::controlPoints)
            .def_rw("type", &CurveShapeComponent::type)
            .def_rw("order", &CurveShapeComponent::order,
                    "B-spline order (degree + 1), clamped to the number of control points.")
            .def("__repr__", [](nb::handle self) {
                return describe("CurveShape", self, {"control_points", "type", "order"});
            });
    }
    {
        const PolygonShapeComponent d{};
        nb::class_<PolygonShapeComponent>(m, "PolygonShape")
            .def(
                "__init__",
                [](PolygonShapeComponent* self, std::vector<Vec3> vertices, Vec4 fillColor,
                   bool filled) {
                    new (self) PolygonShapeComponent{std::move(vertices), fillColor, filled};
                },
                "vertices"_a = std::vector<Vec3>{}, nb::kw_only(),
                "fill_color"_a = d.fillColor, "filled"_a = d.filled)
            .def_rw("vertices", &PolygonShapeComponent::vertices)
            .def_rw("fill_color", &PolygonShapeComponent::fillColor)
            .def_rw("filled", &PolygonShapeComponent::filled)
            .def("__repr__", [](nb::handle self) {
                return describe("PolygonShape", self, {"vertices", "fill_color", "filled"});
            });
    }
    nb::class_<PointShapeComponent>(m, "PointShape", "A single marker point.")
        .def(
            "__init__",
            [](PointShapeComponent* self, Vec3 position) {
                new (self) PointShapeComponent{position};
            },
            "position"_a = Vec3(0.0F))
        .def_rw("position", &PointShapeComponent::position)
        .def("__repr__",
             [](nb::handle self) { return describe("PointShape", self, {"position"}); });

    nb::class_<PortalComponent>(m, "Portal", "Rectangular portal in the local XY plane.")
        .def(
            "__init__",
            [](PortalComponent* self, Vec2 size, std::uint64_t linkedPortal) {
                new (self) PortalComponent{size, Uuid{linkedPortal}};
            },
            "size"_a = PortalComponent{}.size, "linked_portal"_a = 0)
        .def_rw("size", &PortalComponent::size)
        .def_prop_rw(
            "linked_portal", [](const PortalComponent& p) { return p.linkedPortal.value(); },
            [](PortalComponent& p, std::uint64_t id) { p.linkedPortal = Uuid{id}; },
            "Uuid of the linked portal (0 = none).")
        .def("__repr__",
             [](nb::handle self) { return describe("Portal", self, {"size", "linked_portal"}); });

    registerComponent<TransformComponent>();
    registerComponent<MaterialComponent>();
    registerComponent<SphereComponent>();
    registerComponent<MeshComponent>();
    registerComponent<StrokeComponent>();
    registerComponent<LineShapeComponent>();
    registerComponent<EllipseShapeComponent>();
    registerComponent<ArcShapeComponent>();
    registerComponent<CurveShapeComponent>();
    registerComponent<PolygonShapeComponent>();
    registerComponent<PointShapeComponent>();
    registerComponent<PortalComponent>();
}

auto childrenOf(const PyEntity& self) -> std::vector<PyEntity>
{
    const Entity& entity = self.checked();
    std::vector<PyEntity> children;
    for (const auto child : entity.scene()->childrenOf(entity)) {
        children.push_back(PyEntity{entity.scene()->wrap(child), self.owner});
    }
    return children;
}

void bindEntity(nb::module_& m)
{
    // Generic signatures for Entity.get/try_get (shown as such in the type stub).
    m.attr("ComponentT") = nb::type_var("ComponentT");

    nb::class_<PyEntity>(m, "Entity",
                         "Handle to an entity of a Scene (keeps the scene alive).")
        .def_prop_ro("valid", [](const PyEntity& e) { return e.entity.isValid(); })
        .def("__bool__", [](const PyEntity& e) { return e.entity.isValid(); })
        .def_prop_ro("uuid", [](const PyEntity& e) { return e.checked().uuid().value(); },
                     "Persistent id (saved in scene files).")
        .def_prop_ro(
            "handle",
            [](const PyEntity& e) { return entt::to_integral(e.checked().handle()); },
            "Runtime id, as stored in entity-id images.")
        .def_prop_rw(
            "name", [](const PyEntity& e) { return e.checked().name(); },
            [](const PyEntity& e, std::string name) {
                e.checked().patch<NameComponent>([&](auto& n) { n.name = std::move(name); });
            })
        .def_prop_rw(
            "transform",
            [](const PyEntity& e) { return e.checked().get<TransformComponent>(); },
            [](const PyEntity& e, const TransformComponent& transform) {
                e.checked().patch<TransformComponent>([&](auto& t) { t = transform; });
            },
            "Local transform (a copy; assign to apply changes).")
        .def_prop_ro(
            "world_transform",
            [](const PyEntity& e) { return toArray(e.checked().worldTransform()); },
            nb::rv_policy::move)
        .def_prop_ro("parent",
                     [](const PyEntity& e) -> std::optional<PyEntity> {
                         const Entity& entity = e.checked();
                         const Entity parent = entity.scene()->parentOf(entity);
                         return parent ? std::optional(PyEntity{parent, e.owner}) : std::nullopt;
                     })
        .def_prop_ro("children", &childrenOf)
        .def_prop_ro("scene", [](const PyEntity& e) { return e.owner; })
        .def(
            "add",
            [](const PyEntity& e, nb::handle component) {
                const ComponentOps& ops = opsForValue(component);
                if (ops.has(e.checked())) {
                    throw nb::value_error(
                        std::format("the entity already has a {} component",
                                    nb::cast<std::string>(component.type().attr("__name__")))
                            .c_str());
                }
                ops.set(e.entity, component);
            },
            "component"_a, "Add a component (fails if the entity already has one of its type).")
        .def(
            "set",
            [](const PyEntity& e, nb::handle component) {
                opsForValue(component).set(e.checked(), component);
            },
            "component"_a, "Add or replace a component.")
        .def(
            "get",
            [](const PyEntity& e, nb::handle type) {
                nb::object value = opsForType(type).get(e.checked());
                if (value.is_none()) {
                    throw nb::key_error(nb::cast<std::string>(type.attr("__name__")).c_str());
                }
                return value;
            },
            "component_type"_a,
            nb::sig("def get(self, component_type: type[ComponentT], /) -> ComponentT"),
            "A copy of the component (KeyError if missing).")
        .def(
            "try_get",
            [](const PyEntity& e, nb::handle type) { return opsForType(type).get(e.checked()); },
            "component_type"_a,
            nb::sig("def try_get(self, component_type: type[ComponentT], /) -> ComponentT | None"),
            "A copy of the component, or None.")
        .def(
            "has",
            [](const PyEntity& e, nb::handle type) { return opsForType(type).has(e.checked()); },
            "component_type"_a)
        .def(
            "remove",
            [](const PyEntity& e, nb::handle type) { opsForType(type).remove(e.checked()); },
            "component_type"_a)
        .def(
            "components",
            [](const PyEntity& e) {
                nb::list result;
                for (const auto& [type, ops] : componentRegistry()) {
                    if (nb::object value = ops.get(e.checked()); !value.is_none()) {
                        result.append(value);
                    }
                }
                return result;
            },
            "Copies of all components (except identity/hierarchy).")
        .def("__eq__",
             [](const PyEntity& a, const PyEntity& b) { return a.entity == b.entity; })
        .def("__hash__",
             [](const PyEntity& e) {
                 return std::hash<std::uint64_t>{}(
                     (static_cast<std::uint64_t>(entt::to_integral(e.entity.handle())) << 32U) ^
                     reinterpret_cast<std::uintptr_t>(e.entity.scene()));
             })
        .def("__repr__", [](const PyEntity& e) {
            if (!e.entity.isValid()) {
                return std::string("<Entity (destroyed)>");
            }
            return std::format("<Entity '{}' uuid={}>", e.entity.name(), e.entity.uuid().value());
        });
}

void bindSceneClass(nb::module_& m)
{
    nb::class_<Scene>(m, "Scene", "Owns all entities (an EnTT registry) and their hierarchy.")
        .def(nb::init<>())
        .def(
            "create_entity",
            [](Scene& scene, std::string name, std::optional<std::uint64_t> uuid) {
                Entity entity = uuid ? scene.createEntity(Uuid{*uuid}, std::move(name))
                                     : scene.createEntity(std::move(name));
                return *wrap(scene, entity);
            },
            "name"_a = "Entity", "uuid"_a = nb::none(),
            "New entity with a name and an identity transform.")
        .def(
            "destroy_entity",
            [](Scene& scene, const PyEntity& entity) {
                scene.destroyEntity(unwrapOptional(scene, entity));
            },
            "entity"_a, "Destroy an entity and, recursively, its children.")
        .def(
            "copy_entity",
            [](Scene& scene, const PyEntity& source) {
                return *wrap(scene, scene.copyEntity(source.checked()));
            },
            "source"_a,
            "Copy an entity (from any scene) into this scene as a new root entity.")
        .def("clear", &Scene::clear, "Destroy all entities and reset the environment.")
        .def(
            "find_entity",
            [](Scene& scene, std::uint64_t uuid) { return wrap(scene, scene.findEntity(Uuid{uuid})); },
            "uuid"_a)
        .def(
            "find_entity_by_name",
            [](Scene& scene, const std::string& name) {
                return wrap(scene, scene.findEntityByName(name));
            },
            "name"_a)
        .def(
            "entities",
            [](Scene& scene) {
                std::vector<PyEntity> result;
                for (const auto handle : scene.registry().view<IdComponent>()) {
                    result.push_back(*wrap(scene, scene.wrap(handle)));
                }
                std::ranges::reverse(result);  // EnTT iterates newest first.
                return result;
            },
            "All entities in creation order.")
        .def("__len__", &Scene::entityCount)
        .def(
            "set_parent",
            [](Scene& scene, const PyEntity& child, const std::optional<PyEntity>& parent) {
                return scene.setParent(unwrapOptional(scene, child),
                                       unwrapOptional(scene, parent));
            },
            "child"_a, "parent"_a.none(),
            "Re-parent `child` (None detaches it) keeping its world transform; False if this "
            "would create a cycle.")
        .def_prop_rw(
            "sky_color", [](const Scene& scene) { return scene.environment().skyColor; },
            [](Scene& scene, Vec3 color) {
                scene.environment().skyColor = color;
                scene.markDirty();
            })
        .def_prop_ro("version", &Scene::version,
                     "Increases whenever the scene changes (through the API).")
        .def("mark_dirty", &Scene::markDirty)
        .def("to_yaml", [](const Scene& scene) { return serializeScene(scene); })
        .def(
            "load_yaml",
            [](Scene& scene, const std::string& yaml) {
                if (auto loaded = deserializeScene(yaml, scene); !loaded) {
                    throw std::runtime_error(loaded.error());
                }
            },
            "yaml"_a, "Replace the contents with a scene document (RuntimeError on errors).")
        .def(
            "save",
            [](const Scene& scene, const std::filesystem::path& path) {
                if (auto saved = saveScene(scene, path); !saved) {
                    throw std::runtime_error(saved.error());
                }
            },
            "path"_a)
        .def(
            "load",
            [](Scene& scene, const std::filesystem::path& path) {
                if (auto loaded = loadScene(path, scene); !loaded) {
                    throw std::runtime_error(loaded.error());
                }
            },
            "path"_a);
}

}  // namespace

void bindScene(nb::module_& m)
{
    bindComponents(m);
    bindEntity(m);
    bindSceneClass(m);
}

}  // namespace starcore::python
