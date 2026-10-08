#include <algorithm>
#include <ranges>

#include <glm/matrix.hpp>

#include "starcore/scene/entity.hpp"
#include "starcore/scene/scene.hpp"

namespace starcore
{

namespace
{

template <typename... Components>
struct ComponentList
{
};

// Components whose changes affect rendering/serialization.
using TrackedComponents =
    ComponentList<TransformComponent, HierarchyComponent, LayerComponent, MaterialComponent,
                  SphereComponent, MeshComponent, StrokeComponent,
                  LineShapeComponent, EllipseShapeComponent, ArcShapeComponent,
                  CurveShapeComponent, PolygonShapeComponent,
                  PointShapeComponent, PortalComponent, NameComponent>;

template <auto Callback, typename Instance, typename... Components>
void connectAll(entt::registry& registry, Instance& instance,
                ComponentList<Components...> /*unused*/)
{
    (registry.on_construct<Components>().template connect<Callback>(instance),
     ...);
    (registry.on_update<Components>().template connect<Callback>(instance),
     ...);
    (registry.on_destroy<Components>().template connect<Callback>(instance),
     ...);
}

}  // namespace

Scene::Scene()
{
    connectAll<&Scene::onComponentChanged>(m_registry, *this,
                                           TrackedComponents{});
}

Scene::~Scene()
{
    // Disconnect before members are destroyed.
    m_registry.clear();
}

void Scene::onComponentChanged(entt::registry& /*registry*/,
                               entt::entity /*entity*/)
{
    ++m_version;
}

auto Scene::createEntity(std::string name) -> Entity
{
    return createEntity(Uuid{}, std::move(name));
}

auto Scene::createEntity(Uuid uuid, std::string name) -> Entity
{
    Entity entity{m_registry.create(), this};
    entity.add<IdComponent>(uuid);
    entity.add<NameComponent>(std::move(name));
    entity.add<TransformComponent>();
    m_uuidMap[uuid] = entity.handle();
    return entity;
}

void Scene::destroyEntity(Entity entity)
{
    if (!entity.isValid() || entity.scene() != this) {
        return;
    }
    // Unlink from the parent so its child list stays consistent.
    if (const auto* hierarchy = entity.tryGet<HierarchyComponent>();
        hierarchy != nullptr && m_registry.valid(hierarchy->parent)) {
        std::erase(
            m_registry.get<HierarchyComponent>(hierarchy->parent).children,
            entity.handle());
    }
    destroyRecursive(entity.handle());
    ++m_version;
}

namespace
{

// Data components duplicated by Scene::copyEntity (identity and hierarchy
// are handled separately).
using CopiedComponents =
    ComponentList<LayerComponent, MaterialComponent, SphereComponent, MeshComponent,
                  StrokeComponent, LineShapeComponent, EllipseShapeComponent,
                  ArcShapeComponent, CurveShapeComponent, PolygonShapeComponent,
                  PointShapeComponent, PortalComponent>;

template <typename... Components>
void copyComponents(Entity source, Entity target,
                    ComponentList<Components...> /*unused*/)
{
    (
        [&] {
            if (const auto* component = source.tryGet<Components>()) {
                target.addOrReplace<Components>(*component);
            }
        }(),
        ...);
}

}  // namespace

auto Scene::copyEntity(Entity source) -> Entity
{
    Entity copy = createEntity(source.name());
    copy.get<TransformComponent>() = source.get<TransformComponent>();
    copyComponents(source, copy, CopiedComponents{});
    ++m_version;
    return copy;
}

void Scene::destroyRecursive(entt::entity handle)
{
    if (const auto* hierarchy = m_registry.try_get<HierarchyComponent>(handle)) {
        const auto children = hierarchy->children;  // Copy before destroying.
        for (const auto child : children) {
            destroyRecursive(child);
        }
    }
    if (const auto* id = m_registry.try_get<IdComponent>(handle)) {
        m_uuidMap.erase(id->id);
    }
    m_registry.destroy(handle);
}

void Scene::clear()
{
    m_registry.clear();
    m_uuidMap.clear();
    m_environment = {};
    m_layers = {};
    ++m_version;
}

auto Scene::findEntity(Uuid uuid) -> Entity
{
    const auto it = m_uuidMap.find(uuid);
    return it == m_uuidMap.end() ? Entity{} : Entity{it->second, this};
}

auto Scene::findEntityByName(std::string_view name) -> Entity
{
    for (const auto [handle, component] :
         m_registry.view<NameComponent>().each()) {
        if (component.name == name) {
            return Entity{handle, this};
        }
    }
    return {};
}

auto Scene::wrap(entt::entity handle) -> Entity
{
    return m_registry.valid(handle) ? Entity{handle, this} : Entity{};
}

auto Scene::entityCount() const -> std::size_t
{
    return m_registry.view<IdComponent>().size();
}

auto Scene::parentOf(Entity entity) -> Entity
{
    const auto* hierarchy = entity.tryGet<HierarchyComponent>();
    return hierarchy != nullptr ? wrap(hierarchy->parent) : Entity{};
}

auto Scene::childrenOf(Entity entity) const -> std::span<const entt::entity>
{
    const auto* hierarchy =
        m_registry.try_get<HierarchyComponent>(entity.handle());
    return hierarchy != nullptr ? std::span<const entt::entity>(hierarchy->children)
                                : std::span<const entt::entity>{};
}

auto Scene::worldTransform(entt::entity handle) const -> Mat4
{
    Mat4 world{1.0F};
    // Accumulate local transforms from the entity up to the root.
    for (auto current = handle; m_registry.valid(current);) {
        if (const auto* transform =
                m_registry.try_get<TransformComponent>(current)) {
            world = transform->matrix() * world;
        }
        const auto* hierarchy = m_registry.try_get<HierarchyComponent>(current);
        current = hierarchy != nullptr ? hierarchy->parent : entt::null;
    }
    return world;
}

auto Scene::layerOf(entt::entity entity) const -> std::uint32_t
{
    const auto* layer = m_registry.try_get<LayerComponent>(entity);
    return layer != nullptr ? layer->layer : 0U;
}

void Scene::setLayer(Entity entity, std::uint32_t layer, bool recursive)
{
    if (!entity.isValid()) {
        return;
    }
    const auto value = static_cast<std::uint8_t>(layer % kLayerCount);
    if (value == 0) {
        entity.remove<LayerComponent>();  // Layer 0 is the implicit default.
    } else {
        entity.addOrReplace<LayerComponent>(value);
    }
    if (recursive) {
        const auto children = childrenOf(entity);
        const std::vector<entt::entity> copy(children.begin(), children.end());
        for (const auto child : copy) {
            setLayer(wrap(child), layer, true);
        }
    }
}

auto Scene::setParent(Entity child, Entity parent) -> bool
{
    if (!child.isValid() || child == parent) {
        return false;
    }
    // Reject cycles: `parent` must not be a descendant of `child`.
    for (auto current = parent.handle(); m_registry.valid(current);) {
        if (current == child.handle()) {
            return false;
        }
        const auto* hierarchy = m_registry.try_get<HierarchyComponent>(current);
        current = hierarchy != nullptr ? hierarchy->parent : entt::null;
    }

    const Mat4 childWorld = worldTransform(child.handle());

    auto& childHierarchy =
        m_registry.get_or_emplace<HierarchyComponent>(child.handle());
    if (childHierarchy.parent != entt::null) {
        auto& oldSiblings =
            m_registry.get<HierarchyComponent>(childHierarchy.parent).children;
        std::erase(oldSiblings, child.handle());
    }
    childHierarchy.parent = parent.isValid() ? parent.handle() : entt::null;

    Mat4 local = childWorld;
    if (parent.isValid()) {
        m_registry.get_or_emplace<HierarchyComponent>(parent.handle())
            .children.push_back(child.handle());
        local = glm::inverse(worldTransform(parent.handle())) * childWorld;
    }

    const auto decomposed = decomposeTransform(local);
    m_registry.patch<TransformComponent>(
        child.handle(), [&](TransformComponent& transform) {
            transform.translation = decomposed.translation;
            transform.rotation = decomposed.rotation;
            transform.scale = decomposed.scale;
        });
    ++m_version;
    return true;
}

}  // namespace starcore
