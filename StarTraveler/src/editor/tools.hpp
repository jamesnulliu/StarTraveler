#pragma once

#include <cstdint>
#include <memory>
#include <string_view>

#include <starcore/scene/entity.hpp>
#include <starcore/scene/scene.hpp>

#include "editor/editor_context.hpp"

namespace startraveler
{

enum class ToolKind : std::uint8_t
{
    None,
    Line,
    Ellipse,
    Arc,
    Polygon,
    Curve,
    Group,
    SeedFill,  // Handled by the GameLayer (edits the displayed frame).
};

[[nodiscard]] auto toolName(ToolKind kind) -> std::string_view;

struct ToolResult
{
    bool finished = false;
    starcore::Entity created;  // In the main scene, when finished.
};

/**
 * @brief Interactive shape-drawing tool (a small state machine fed with
 *        edge-triggered input).
 *
 * The shape being drawn lives in the tool's private preview scene, which is
 * rendered with the same overlay code as the real scene; on completion it is
 * copied into the main scene. (Legacy kept half-built shapes and helper lines
 * in Scene::tempEntities and popped scene entities on Esc, which removed the
 * wrong entity for tools that had not created one yet.)
 */
class Tool
{
public:
    virtual ~Tool() = default;

    [[nodiscard]] virtual auto kind() const -> ToolKind = 0;
    /** @brief Short usage hint for the status bar. */
    [[nodiscard]] virtual auto hint() const -> std::string_view = 0;

    virtual auto update(EditorContext& context) -> ToolResult = 0;

    /** @brief Shapes to draw on top of the frame (in-progress geometry). */
    [[nodiscard]] auto previewScene() const -> const starcore::Scene&
    {
        return m_preview;
    }

protected:
    /** @brief Copy the preview shape into the main scene and reset. */
    auto commit(EditorContext& context, starcore::Entity shape) -> ToolResult;

    starcore::Scene m_preview;
};

[[nodiscard]] auto makeTool(ToolKind kind) -> std::unique_ptr<Tool>;

}  // namespace startraveler
