#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>

#include <entt/entity/registry.hpp>

#include "starcore/math/types.hpp"
#include "starcore/scene/layers.hpp"
#include "starcore/scene/uuid.hpp"

namespace starcore
{

class Entity;

struct SceneEnvironment
{
    // Radiance returned by rays that escape the scene (legacy default).
    Vec3 skyColor{10.0F / 255.0F, 0.0F, 60.0F / 255.0F};
};

/**
 * @brief Owns all entities (an EnTT registry) and their hierarchy.
 *
 * Not copyable or movable: Entity handles keep a pointer to their Scene.
 */
class Scene
{
public:
    Scene();
    ~Scene();
    Scene(const Scene&) = delete;
    auto operator=(const Scene&) -> Scene& = delete;
    Scene(Scene&&) = delete;
    auto operator=(Scene&&) -> Scene& = delete;

    /** @brief New entity with Id, Name and Transform components. */
    auto createEntity(std::string name = "Entity") -> Entity;
    auto createEntity(Uuid uuid, std::string name) -> Entity;

    /** @brief Destroy an entity and, recursively, its children. */
    void destroyEntity(Entity entity);

    /**
     * @brief Copy an entity (from this or another scene) into this scene
     *        with a new id: name, local transform and every data component
     *        (not the hierarchy; the copy is a root entity).
     */
    auto copyEntity(Entity source) -> Entity;

    /** @brief Destroy all entities and reset the environment and layers. */
    void clear();

    [[nodiscard]] auto findEntity(Uuid uuid) -> Entity;
    [[nodiscard]] auto findEntityByName(std::string_view name) -> Entity;
    [[nodiscard]] auto wrap(entt::entity handle) -> Entity;
    [[nodiscard]] auto entityCount() const -> std::size_t;

    // Hierarchy -----------------------------------------------------------------------------------

    /**
     * @brief Re-parent `child` under `parent` (or detach it when `parent` is
     *        null), keeping its world transform unchanged.
     * @return false if this would create a cycle.
     */
    auto setParent(Entity child, Entity parent) -> bool;
    [[nodiscard]] auto parentOf(Entity entity) -> Entity;
    [[nodiscard]] auto childrenOf(Entity entity) const
        -> std::span<const entt::entity>;
    [[nodiscard]] auto worldTransform(entt::entity entity) const -> Mat4;

    // Layers --------------------------------------------------------------------------------------

    /** @brief Layer of an entity (0 without a LayerComponent). */
    [[nodiscard]] auto layerOf(entt::entity entity) const -> std::uint32_t;
    /** @brief Move an entity (and optionally its descendants) to a layer. */
    void setLayer(Entity entity, std::uint32_t layer, bool recursive = false);

    /** @brief Per-layer names, visibility, pickability and collision matrix. */
    template <typename Self>
    [[nodiscard]] auto layers(this Self& self) -> auto&
    {
        return self.m_layers;
    }

    // Access --------------------------------------------------------------------------------------

    /** @brief The registry (const for const scenes). */
    template <typename Self>
    [[nodiscard]] auto registry(this Self& self) -> auto&
    {
        return self.m_registry;
    }

    template <typename Self>
    [[nodiscard]] auto environment(this Self& self) -> auto&
    {
        return self.m_environment;
    }

    /**
     * @brief Increases whenever the scene changes. Components added/removed
     *        or updated through the registry bump it automatically; code that
     *        edits components in place must call markDirty().
     */
    [[nodiscard]] auto version() const noexcept -> std::uint64_t
    {
        return m_version;
    }

    void markDirty() noexcept
    {
        ++m_version;
    }

private:
    void onComponentChanged(entt::registry& registry, entt::entity entity);
    void destroyRecursive(entt::entity entity);

    entt::registry m_registry;
    std::unordered_map<Uuid, entt::entity> m_uuidMap;
    SceneEnvironment m_environment;
    SceneLayers m_layers;
    std::uint64_t m_version = 0;
};

}  // namespace starcore
