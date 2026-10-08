#pragma once

#include <cstddef>
#include <optional>
#include <vector>

#include <starcore/common/image.hpp>
#include <starcore/render/render_scene.hpp>
#include <starcore/scene/entity.hpp>

#include "editor/editor_context.hpp"

namespace startraveler
{

/**
 * @brief Selection, control-point handles and dragging (legacy
 *        Block_EntityUnderMouse / ControlPoint, without raw Vertex* binders).
 *
 * - Click an entity to select it; click empty space to deselect.
 * - Drag a green handle to edit that vertex/radius/angle.
 * - Drag the selected entity itself to move it in the view plane.
 * - Ctrl+click another entity to make it a child of the selection.
 * - Delete removes the selection, Esc deselects.
 */
class Selection
{
public:
    /** @param pickedEntity Entity under the mouse this frame (if any). */
    void update(EditorContext& context,
                std::optional<entt::entity> pickedEntity);

    /** @brief Draw handles of the selection on top of the frame. */
    void drawHandles(const EditorContext& context,
                     starcore::ColorImage& frame) const;

    [[nodiscard]] auto selected() const -> starcore::Entity
    {
        return m_selected;
    }
    void select(starcore::Entity entity);
    void clear();
    /** @brief True while a handle or the entity is being dragged. */
    [[nodiscard]] auto isDragging() const -> bool
    {
        return m_drag.has_value();
    }

private:
    struct Drag
    {
        std::optional<std::size_t> handle;  // nullopt: move the entity.
        float depth = 0.0F;                 // Camera depth of the grab point.
        starcore::Vec3 grabWorld{0.0F};
        starcore::Vec3 originWorld{0.0F};   // Entity origin at grab time.
    };

    [[nodiscard]] auto handles(const starcore::Entity& entity) const
        -> std::vector<starcore::Vec3>;  // World positions.
    void moveHandle(const starcore::Entity& entity, std::size_t index,
                    starcore::Vec3 world) const;
    [[nodiscard]] auto handleUnderMouse(const EditorContext& context) const
        -> std::optional<std::size_t>;

    starcore::Entity m_selected;
    std::optional<Drag> m_drag;
};

}  // namespace startraveler
