#pragma once

#include <string>
#include <utility>

#include <entt/entity/registry.hpp>

#include "starcore/common/assert.hpp"
#include "starcore/scene/components.hpp"
#include "starcore/scene/scene.hpp"

namespace starcore
{

/**
 * @brief Lightweight, copyable handle to an entity in a Scene.
 *
 * Like a pointer, a const Entity still gives mutable access to components.
 */
class Entity
{
public:
    Entity() = default;
    Entity(entt::entity handle, Scene* scene) : m_handle(handle), m_scene(scene)
    {
    }

    template <typename T, typename... Args>
    auto add(Args&&... args) const -> T&
    {
        SC_ASSERT(!has<T>(), "Entity already has this component");
        return registry().template emplace<T>(m_handle,
                                              std::forward<Args>(args)...);
    }

    template <typename T, typename... Args>
    auto addOrReplace(Args&&... args) const -> T&
    {
        return registry().template emplace_or_replace<T>(
            m_handle, std::forward<Args>(args)...);
    }

    template <typename T>
    [[nodiscard]] auto get() const -> T&
    {
        SC_ASSERT(has<T>(), "Entity does not have this component");
        return registry().template get<T>(m_handle);
    }

    /** @brief Component pointer, or nullptr if the entity lacks it. */
    template <typename T>
    [[nodiscard]] auto tryGet() const -> T*
    {
        return registry().template try_get<T>(m_handle);
    }

    template <typename... T>
    [[nodiscard]] auto has() const -> bool
    {
        return registry().template all_of<T...>(m_handle);
    }

    template <typename T>
    void remove() const
    {
        registry().template remove<T>(m_handle);
    }

    /**
     * @brief Edit a component in place and notify the scene (bumps
     *        Scene::version()), e.g. `e.patch<SphereComponent>([](auto& s) {
     *        s.radius = 2; })`.
     */
    template <typename T, typename Fn>
    auto patch(Fn&& fn) const -> T&
    {
        return registry().template patch<T>(m_handle, std::forward<Fn>(fn));
    }

    [[nodiscard]] auto isValid() const -> bool
    {
        return m_scene != nullptr && registry().valid(m_handle);
    }

    explicit operator bool() const
    {
        return isValid();
    }

    [[nodiscard]] auto handle() const noexcept -> entt::entity
    {
        return m_handle;
    }

    [[nodiscard]] auto scene() const noexcept -> Scene*
    {
        return m_scene;
    }

    [[nodiscard]] auto uuid() const -> Uuid
    {
        return get<IdComponent>().id;
    }

    [[nodiscard]] auto name() const -> const std::string&
    {
        return get<NameComponent>().name;
    }

    [[nodiscard]] auto worldTransform() const -> Mat4
    {
        return m_scene->worldTransform(m_handle);
    }

    auto operator==(const Entity&) const -> bool = default;

private:
    [[nodiscard]] auto registry() const -> entt::registry&
    {
        SC_ASSERT(m_scene != nullptr, "Entity is not bound to a scene");
        return m_scene->registry();
    }

    entt::entity m_handle{entt::null};
    Scene* m_scene = nullptr;
};

}  // namespace starcore
