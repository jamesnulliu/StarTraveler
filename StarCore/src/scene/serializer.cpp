#include <format>
#include <fstream>
#include <sstream>
#include <unordered_map>
#include <vector>

#include <yaml-cpp/yaml.h>

#include "starcore/scene/entity.hpp"
#include "starcore/scene/serializer.hpp"

// YAML conversions for glm types ------------------------------------------------------------------

namespace YAML
{

template <glm::length_t L>
struct convert<glm::vec<L, float, glm::defaultp>>
{
    static auto encode(const glm::vec<L, float, glm::defaultp>& v) -> Node
    {
        Node node(NodeType::Sequence);
        for (glm::length_t i = 0; i < L; ++i) {
            node.push_back(v[i]);
        }
        node.SetStyle(EmitterStyle::Flow);
        return node;
    }

    static auto decode(const Node& node, glm::vec<L, float, glm::defaultp>& v)
        -> bool
    {
        if (!node.IsSequence() || node.size() != static_cast<std::size_t>(L)) {
            return false;
        }
        for (glm::length_t i = 0; i < L; ++i) {
            v[i] = node[static_cast<std::size_t>(i)].as<float>();
        }
        return true;
    }
};

/** @brief Quaternions are stored as [w, x, y, z]. */
template <>
struct convert<glm::quat>
{
    static auto encode(const glm::quat& q) -> Node
    {
        Node node(NodeType::Sequence);
        node.push_back(q.w);
        node.push_back(q.x);
        node.push_back(q.y);
        node.push_back(q.z);
        node.SetStyle(EmitterStyle::Flow);
        return node;
    }

    static auto decode(const Node& node, glm::quat& q) -> bool
    {
        if (!node.IsSequence() || node.size() != 4) {
            return false;
        }
        q = glm::quat(node[0].as<float>(), node[1].as<float>(),
                      node[2].as<float>(), node[3].as<float>());
        return true;
    }
};

}  // namespace YAML

namespace starcore
{

namespace
{

template <typename E>
auto encodeEnum(E value) -> std::string
{
    return std::string(enumToString(value));
}

template <typename E>
auto decodeEnum(const YAML::Node& node, E fallback) -> E
{
    if (!node) {
        return fallback;
    }
    E value = fallback;
    if (!enumFromString(node.as<std::string>(), value)) {
        throw std::runtime_error(
            std::format("unknown enum value \"{}\"", node.as<std::string>()));
    }
    return value;
}

template <typename T>
auto read(const YAML::Node& node, const T& fallback) -> T
{
    return node ? node.as<T>() : fallback;
}

void writeEntity(YAML::Emitter& out, const Scene& scene, entt::entity entity)
{
    const auto& registry = scene.registry();
    out << YAML::BeginMap;
    out << YAML::Key << "Id" << YAML::Value
        << registry.get<IdComponent>(entity).id.value();
    if (const auto* name = registry.try_get<NameComponent>(entity)) {
        out << YAML::Key << "Name" << YAML::Value << name->name;
    }
    if (const auto* hierarchy = registry.try_get<HierarchyComponent>(entity);
        hierarchy != nullptr && registry.valid(hierarchy->parent)) {
        out << YAML::Key << "Parent" << YAML::Value
            << registry.get<IdComponent>(hierarchy->parent).id.value();
    }
    if (const auto* t = registry.try_get<TransformComponent>(entity)) {
        YAML::Node node;
        node["Translation"] = t->translation;
        node["Rotation"] = t->rotation;
        node["Scale"] = t->scale;
        out << YAML::Key << "Transform" << YAML::Value << node;
    }
    if (const auto* m = registry.try_get<MaterialComponent>(entity)) {
        YAML::Node node;
        node["Albedo"] = m->albedo;
        node["Roughness"] = m->roughness;
        node["Metallic"] = m->metallic;
        node["EmissionColor"] = m->emissionColor;
        node["EmissionPower"] = m->emissionPower;
        out << YAML::Key << "Material" << YAML::Value << node;
    }
    if (const auto* s = registry.try_get<SphereComponent>(entity)) {
        YAML::Node node;
        node["Radius"] = s->radius;
        out << YAML::Key << "Sphere" << YAML::Value << node;
    }
    if (const auto* mesh = registry.try_get<MeshComponent>(entity)) {
        YAML::Node node;
        node["Positions"] = YAML::Node(YAML::NodeType::Sequence);
        for (const Vec3& p : mesh->positions) {
            node["Positions"].push_back(p);
        }
        node["Indices"] = mesh->indices;
        node["Indices"].SetStyle(YAML::EmitterStyle::Flow);
        out << YAML::Key << "Mesh" << YAML::Value << node;
    }
    if (const auto* s = registry.try_get<StrokeComponent>(entity)) {
        YAML::Node node;
        node["Color"] = s->color;
        node["Thickness"] = s->thickness;
        node["Pattern"] = encodeEnum(s->pattern);
        out << YAML::Key << "Stroke" << YAML::Value << node;
    }
    if (const auto* l = registry.try_get<LineShapeComponent>(entity)) {
        YAML::Node node;
        node["Start"] = l->start;
        node["End"] = l->end;
        node["Algorithm"] = encodeEnum(l->algorithm);
        out << YAML::Key << "LineShape" << YAML::Value << node;
    }
    if (const auto* e = registry.try_get<EllipseShapeComponent>(entity)) {
        YAML::Node node;
        node["RadiusX"] = e->radiusX;
        node["RadiusY"] = e->radiusY;
        node["Algorithm"] = encodeEnum(e->algorithm);
        out << YAML::Key << "EllipseShape" << YAML::Value << node;
    }
    if (const auto* a = registry.try_get<ArcShapeComponent>(entity)) {
        YAML::Node node;
        node["Radius"] = a->radius;
        node["StartAngle"] = a->startAngle;
        node["SweepAngle"] = a->sweepAngle;
        out << YAML::Key << "ArcShape" << YAML::Value << node;
    }
    if (const auto* c = registry.try_get<CurveShapeComponent>(entity)) {
        YAML::Node node;
        node["ControlPoints"] = YAML::Node(YAML::NodeType::Sequence);
        for (const Vec3& p : c->controlPoints) {
            node["ControlPoints"].push_back(p);
        }
        node["Type"] = encodeEnum(c->type);
        node["Order"] = c->order;
        out << YAML::Key << "CurveShape" << YAML::Value << node;
    }
    if (const auto* p = registry.try_get<PolygonShapeComponent>(entity)) {
        YAML::Node node;
        node["Vertices"] = YAML::Node(YAML::NodeType::Sequence);
        for (const Vec3& v : p->vertices) {
            node["Vertices"].push_back(v);
        }
        node["FillColor"] = p->fillColor;
        node["Filled"] = p->filled;
        out << YAML::Key << "PolygonShape" << YAML::Value << node;
    }
    if (const auto* p = registry.try_get<PointShapeComponent>(entity)) {
        YAML::Node node;
        node["Position"] = p->position;
        out << YAML::Key << "PointShape" << YAML::Value << node;
    }
    if (const auto* p = registry.try_get<PortalComponent>(entity)) {
        YAML::Node node;
        node["Size"] = p->size;
        node["LinkedPortal"] = p->linkedPortal.value();
        out << YAML::Key << "Portal" << YAML::Value << node;
    }
    out << YAML::EndMap;
}

void readEntity(const YAML::Node& node, Scene& scene,
                std::vector<std::pair<entt::entity, Uuid>>& parentLinks)
{
    const Uuid id{node["Id"].as<std::uint64_t>()};
    Entity entity = scene.createEntity(id, read<std::string>(node["Name"], "Entity"));

    if (const auto parent = node["Parent"]) {
        parentLinks.emplace_back(entity.handle(), Uuid{parent.as<std::uint64_t>()});
    }
    if (const auto t = node["Transform"]) {
        auto& transform = entity.get<TransformComponent>();
        transform.translation = read(t["Translation"], Vec3(0.0F));
        transform.rotation = read(t["Rotation"], Quat(1.0F, 0.0F, 0.0F, 0.0F));
        transform.scale = read(t["Scale"], Vec3(1.0F));
    }
    if (const auto m = node["Material"]) {
        auto& material = entity.add<MaterialComponent>();
        material.albedo = read(m["Albedo"], material.albedo);
        material.roughness = read(m["Roughness"], material.roughness);
        material.metallic = read(m["Metallic"], material.metallic);
        material.emissionColor = read(m["EmissionColor"], material.emissionColor);
        material.emissionPower = read(m["EmissionPower"], material.emissionPower);
    }
    if (const auto s = node["Sphere"]) {
        entity.add<SphereComponent>(read(s["Radius"], 1.0F));
    }
    if (const auto m = node["Mesh"]) {
        auto& mesh = entity.add<MeshComponent>();
        mesh.positions = read(m["Positions"], std::vector<Vec3>{});
        mesh.indices = read(m["Indices"], std::vector<std::uint32_t>{});
    }
    if (const auto s = node["Stroke"]) {
        auto& stroke = entity.add<StrokeComponent>();
        stroke.color = read(s["Color"], stroke.color);
        stroke.thickness = read(s["Thickness"], stroke.thickness);
        stroke.pattern = decodeEnum(s["Pattern"], stroke.pattern);
    }
    if (const auto l = node["LineShape"]) {
        auto& line = entity.add<LineShapeComponent>();
        line.start = read(l["Start"], line.start);
        line.end = read(l["End"], line.end);
        line.algorithm = decodeEnum(l["Algorithm"], line.algorithm);
    }
    if (const auto e = node["EllipseShape"]) {
        auto& ellipse = entity.add<EllipseShapeComponent>();
        ellipse.radiusX = read(e["RadiusX"], ellipse.radiusX);
        ellipse.radiusY = read(e["RadiusY"], ellipse.radiusY);
        ellipse.algorithm = decodeEnum(e["Algorithm"], ellipse.algorithm);
    }
    if (const auto a = node["ArcShape"]) {
        auto& arc = entity.add<ArcShapeComponent>();
        arc.radius = read(a["Radius"], arc.radius);
        arc.startAngle = read(a["StartAngle"], arc.startAngle);
        arc.sweepAngle = read(a["SweepAngle"], arc.sweepAngle);
    }
    if (const auto c = node["CurveShape"]) {
        auto& curve = entity.add<CurveShapeComponent>();
        curve.controlPoints = read(c["ControlPoints"], std::vector<Vec3>{});
        curve.type = decodeEnum(c["Type"], curve.type);
        curve.order = read(c["Order"], curve.order);
    }
    if (const auto p = node["PolygonShape"]) {
        auto& polygon = entity.add<PolygonShapeComponent>();
        polygon.vertices = read(p["Vertices"], std::vector<Vec3>{});
        polygon.fillColor = read(p["FillColor"], polygon.fillColor);
        polygon.filled = read(p["Filled"], polygon.filled);
    }
    if (const auto p = node["PointShape"]) {
        entity.add<PointShapeComponent>(read(p["Position"], Vec3(0.0F)));
    }
    if (const auto p = node["Portal"]) {
        auto& portal = entity.add<PortalComponent>();
        portal.size = read(p["Size"], portal.size);
        portal.linkedPortal = Uuid{read<std::uint64_t>(p["LinkedPortal"], 0)};
    }
}

}  // namespace

auto serializeScene(const Scene& scene) -> std::string
{
    YAML::Emitter out;
    out.SetFloatPrecision(9);  // Round-trips every float exactly.
    out << YAML::BeginMap;
    out << YAML::Key << "StarCoreScene" << YAML::Value << kSceneFormatVersion;
    out << YAML::Key << "Environment" << YAML::Value << YAML::BeginMap;
    out << YAML::Key << "SkyColor" << YAML::Value
        << YAML::Node(scene.environment().skyColor);
    out << YAML::EndMap;

    // Write in creation order (parents are linked after loading, so the
    // order is only cosmetic).
    std::vector<entt::entity> ordered;
    const auto& registry = scene.registry();
    for (const auto entity : registry.view<const IdComponent>()) {
        ordered.push_back(entity);
    }
    std::ranges::reverse(ordered);  // EnTT iterates newest first.

    out << YAML::Key << "Entities" << YAML::Value << YAML::BeginSeq;
    for (const auto entity : ordered) {
        writeEntity(out, scene, entity);
    }
    out << YAML::EndSeq;
    out << YAML::EndMap;
    return out.c_str();
}

auto deserializeScene(std::string_view yaml, Scene& scene)
    -> std::expected<void, std::string>
{
    scene.clear();
    try {
        const YAML::Node root = YAML::Load(std::string(yaml));
        if (!root["StarCoreScene"]) {
            return std::unexpected("not a StarCore scene (missing \"StarCoreScene\")");
        }
        const int version = root["StarCoreScene"].as<int>();
        if (version > kSceneFormatVersion) {
            return std::unexpected(
                std::format("scene format {} is newer than supported ({})",
                            version, kSceneFormatVersion));
        }
        if (const auto environment = root["Environment"]) {
            scene.environment().skyColor =
                read(environment["SkyColor"], scene.environment().skyColor);
        }

        std::vector<std::pair<entt::entity, Uuid>> parentLinks;
        for (const auto& node : root["Entities"]) {
            readEntity(node, scene, parentLinks);
        }

        // Link hierarchies without touching the (already local) transforms.
        auto& registry = scene.registry();
        for (const auto& [child, parentId] : parentLinks) {
            const Entity parent = scene.findEntity(parentId);
            if (!parent) {
                throw std::runtime_error(std::format(
                    "entity references unknown parent {}", parentId.value()));
            }
            registry.get_or_emplace<HierarchyComponent>(child).parent =
                parent.handle();
            registry.get_or_emplace<HierarchyComponent>(parent.handle())
                .children.push_back(child);
        }
    } catch (const std::exception& error) {
        scene.clear();
        return std::unexpected(std::format("invalid scene: {}", error.what()));
    }
    return {};
}

auto saveScene(const Scene& scene, const std::filesystem::path& path)
    -> std::expected<void, std::string>
{
    if (path.has_parent_path()) {
        std::error_code error;
        std::filesystem::create_directories(path.parent_path(), error);
    }
    std::ofstream file(path, std::ios::binary);
    if (!file) {
        return std::unexpected(
            std::format("cannot open \"{}\" for writing", path.string()));
    }
    file << serializeScene(scene);
    return {};
}

auto loadScene(const std::filesystem::path& path, Scene& scene)
    -> std::expected<void, std::string>
{
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return std::unexpected(
            std::format("cannot open \"{}\"", path.string()));
    }
    std::stringstream buffer;
    buffer << file.rdbuf();
    return deserializeScene(buffer.str(), scene);
}

}  // namespace starcore
