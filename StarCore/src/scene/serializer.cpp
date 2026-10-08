#include <format>
#include <fstream>
#include <sstream>
#include <unordered_map>
#include <vector>

#include <yaml-cpp/yaml.h>

#include "starcore/asset/model.hpp"
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

auto vec3Sequence(std::span<const Vec3> values) -> YAML::Node
{
    YAML::Node node(YAML::NodeType::Sequence);
    for (const Vec3& value : values) {
        node.push_back(value);
    }
    return node;
}

/** @brief State shared by the entity readers/writers of one document. */
struct DocumentContext
{
    std::filesystem::path baseDirectory;  // For relative model paths.
    AssetLibrary* assets = nullptr;
};

auto writeMesh(const MeshComponent& component, const DocumentContext& context) -> YAML::Node
{
    YAML::Node node;
    if (!component.source.empty()) {
        // A model reference: relative to the scene file when possible.
        const std::filesystem::path model(component.source.path);
        std::filesystem::path relative;
        if (!context.baseDirectory.empty()) {
            relative = model.lexically_relative(
                std::filesystem::absolute(context.baseDirectory).lexically_normal());
        }
        node["Model"] = (relative.empty() ? model : relative).generic_string();
        node["Index"] = component.source.meshIndex;
        return node;
    }
    const MeshData& mesh = *component.mesh;
    node["Positions"] = vec3Sequence(mesh.positions);
    if (!mesh.normals.empty()) {
        node["Normals"] = vec3Sequence(mesh.normals);
    }
    node["Indices"] = mesh.indices;
    node["Indices"].SetStyle(YAML::EmitterStyle::Flow);
    return node;
}

auto readMesh(const YAML::Node& node, const DocumentContext& context) -> MeshComponent
{
    if (const auto model = node["Model"]) {
        std::filesystem::path path(model.as<std::string>());
        if (path.is_relative() && !context.baseDirectory.empty()) {
            path = context.baseDirectory / path;
        }
        const MeshSource source{normalizeAssetPath(path), read(node["Index"], 0U)};
        auto mesh = context.assets->loadMesh(source);
        if (!mesh) {
            throw std::runtime_error(mesh.error());
        }
        return MeshComponent{.mesh = *std::move(mesh), .source = source};
    }
    auto mesh = makeMesh(read(node["Positions"], std::vector<Vec3>{}),
                         read(node["Indices"], std::vector<std::uint32_t>{}),
                         read(node["Normals"], std::vector<Vec3>{}));
    if (!mesh) {
        throw std::runtime_error(std::format("invalid mesh: {}", mesh.error()));
    }
    return MeshComponent{.mesh = *std::move(mesh)};
}

void writeEntity(YAML::Emitter& out, const Scene& scene, entt::entity entity,
                 const DocumentContext& context)
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
    if (const auto* layer = registry.try_get<LayerComponent>(entity); layer != nullptr) {
        out << YAML::Key << "Layer" << YAML::Value << static_cast<int>(layer->layer);
    }
    if (const auto* m = registry.try_get<MaterialComponent>(entity)) {
        YAML::Node node;
        node["Albedo"] = m->albedo;
        node["Roughness"] = m->roughness;
        node["Metallic"] = m->metallic;
        node["EmissionColor"] = m->emissionColor;
        node["EmissionPower"] = m->emissionPower;
        if (m->transmission > 0.0F) {
            node["Transmission"] = m->transmission;
            node["Ior"] = m->ior;
            node["ThinWalled"] = m->thinWalled;
        }
        out << YAML::Key << "Material" << YAML::Value << node;
    }
    if (const auto* s = registry.try_get<SphereComponent>(entity)) {
        YAML::Node node;
        node["Radius"] = s->radius;
        out << YAML::Key << "Sphere" << YAML::Value << node;
    }
    if (const auto* mesh = registry.try_get<MeshComponent>(entity);
        mesh != nullptr && mesh->mesh) {
        out << YAML::Key << "Mesh" << YAML::Value << writeMesh(*mesh, context);
    }
    if (const auto* collider = registry.try_get<ColliderComponent>(entity)) {
        YAML::Node node;
        node["Shape"] = encodeEnum(collider->shape);
        if (collider->shape == ColliderShape::Sphere) {
            node["Radius"] = collider->radius;
        } else if (collider->shape == ColliderShape::Box) {
            node["HalfExtents"] = collider->halfExtents;
        }
        if (collider->trigger) {
            node["Trigger"] = true;
        }
        out << YAML::Key << "Collider" << YAML::Value << node;
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
                std::vector<std::pair<entt::entity, Uuid>>& parentLinks,
                const DocumentContext& context)
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
    if (const auto layer = node["Layer"]) {
        scene.setLayer(entity, static_cast<std::uint32_t>(layer.as<int>()));
    }
    if (const auto m = node["Material"]) {
        auto& material = entity.add<MaterialComponent>();
        material.albedo = read(m["Albedo"], material.albedo);
        material.roughness = read(m["Roughness"], material.roughness);
        material.metallic = read(m["Metallic"], material.metallic);
        material.emissionColor = read(m["EmissionColor"], material.emissionColor);
        material.emissionPower = read(m["EmissionPower"], material.emissionPower);
        material.transmission = read(m["Transmission"], material.transmission);
        material.ior = read(m["Ior"], material.ior);
        material.thinWalled = read(m["ThinWalled"], material.thinWalled);
    }
    if (const auto s = node["Sphere"]) {
        entity.add<SphereComponent>(read(s["Radius"], 1.0F));
    }
    if (const auto m = node["Mesh"]) {
        entity.add<MeshComponent>(readMesh(m, context));
    }
    if (const auto c = node["Collider"]) {
        auto& collider = entity.add<ColliderComponent>();
        collider.shape = decodeEnum(c["Shape"], collider.shape);
        collider.radius = read(c["Radius"], collider.radius);
        collider.halfExtents = read(c["HalfExtents"], collider.halfExtents);
        collider.trigger = read(c["Trigger"], collider.trigger);
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

auto serializeScene(const Scene& scene, const std::filesystem::path& baseDirectory)
    -> std::string
{
    const DocumentContext context{.baseDirectory = baseDirectory};
    YAML::Emitter out;
    out.SetFloatPrecision(9);  // Round-trips every float exactly.
    out << YAML::BeginMap;
    out << YAML::Key << "StarCoreScene" << YAML::Value << kSceneFormatVersion;
    out << YAML::Key << "Environment" << YAML::Value << YAML::BeginMap;
    out << YAML::Key << "SkyColor" << YAML::Value
        << YAML::Node(scene.environment().skyColor);
    out << YAML::EndMap;

    if (scene.layers() != SceneLayers{}) {
        out << YAML::Key << "Layers" << YAML::Value << YAML::BeginSeq;
        for (std::size_t i = 0; i < kLayerCount; ++i) {
            const LayerSettings& layer = scene.layers().layers[i];
            out << YAML::Flow << YAML::BeginMap;
            out << YAML::Key << "Name" << YAML::Value << layer.name;
            out << YAML::Key << "Visible" << YAML::Value << layer.visible;
            out << YAML::Key << "Pickable" << YAML::Value << layer.pickable;
            out << YAML::Key << "CollisionMask" << YAML::Value
                << static_cast<int>(scene.layers().collisionMasks[i]);
            out << YAML::EndMap;
        }
        out << YAML::EndSeq;
    }

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
        writeEntity(out, scene, entity, context);
    }
    out << YAML::EndSeq;
    out << YAML::EndMap;
    return out.c_str();
}

auto deserializeScene(std::string_view yaml, Scene& scene,
                      const std::filesystem::path& baseDirectory, AssetLibrary* assets)
    -> std::expected<void, std::string>
{
    scene.clear();
    AssetLibrary temporaryAssets;
    const DocumentContext context{.baseDirectory = baseDirectory,
                                  .assets = assets != nullptr ? assets : &temporaryAssets};
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

        if (const auto layers = root["Layers"]) {
            for (std::size_t i = 0; i < std::min<std::size_t>(layers.size(), kLayerCount); ++i) {
                const YAML::Node layer = layers[i];
                LayerSettings& settings = scene.layers().layers[i];
                settings.name = read(layer["Name"], settings.name);
                settings.visible = read(layer["Visible"], settings.visible);
                settings.pickable = read(layer["Pickable"], settings.pickable);
                scene.layers().collisionMasks[i] = static_cast<std::uint8_t>(
                    read(layer["CollisionMask"], static_cast<int>(kAllLayers)));
            }
        }

        std::vector<std::pair<entt::entity, Uuid>> parentLinks;
        for (const auto& node : root["Entities"]) {
            readEntity(node, scene, parentLinks, context);
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
    file << serializeScene(scene, std::filesystem::absolute(path).parent_path());
    return {};
}

auto loadScene(const std::filesystem::path& path, Scene& scene, AssetLibrary* assets)
    -> std::expected<void, std::string>
{
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return std::unexpected(
            std::format("cannot open \"{}\"", path.string()));
    }
    std::stringstream buffer;
    buffer << file.rdbuf();
    return deserializeScene(buffer.str(), scene, std::filesystem::absolute(path).parent_path(),
                            assets);
}

}  // namespace starcore
