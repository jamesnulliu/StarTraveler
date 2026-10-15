#pragma once
// Input polling on top of Dear ImGui's input state, which covers every
// platform window (with multi-viewports a panel may live outside the main
// window). Edge queries (pressed/released) replace the legacy pattern of
// stepping state machines on "button down" to detect clicks.

#include <cstdint>

#include <imgui.h>

#include "starcore/math/types.hpp"

namespace starcore::input
{

enum class CursorMode : std::uint8_t
{
    Normal,
    Hidden,
    // Hidden and captured; mouse deltas stay unbounded (camera look). The
    // mouse and the keys steer the application meanwhile: ImGui neither
    // hovers its windows under the hidden cursor nor navigates them with the
    // keyboard (Space would press the focused button).
    Locked,
};

[[nodiscard]] inline auto isKeyDown(ImGuiKey key) -> bool
{
    return ImGui::IsKeyDown(key);
}

[[nodiscard]] inline auto isKeyPressed(ImGuiKey key, bool repeat = false)
    -> bool
{
    return ImGui::IsKeyPressed(key, repeat);
}

[[nodiscard]] inline auto isKeyReleased(ImGuiKey key) -> bool
{
    return ImGui::IsKeyReleased(key);
}

[[nodiscard]] inline auto isMouseDown(ImGuiMouseButton button) -> bool
{
    return ImGui::IsMouseDown(button);
}

[[nodiscard]] inline auto isMouseClicked(ImGuiMouseButton button) -> bool
{
    return ImGui::IsMouseClicked(button);
}

[[nodiscard]] inline auto isMouseReleased(ImGuiMouseButton button) -> bool
{
    return ImGui::IsMouseReleased(button);
}

/** @brief Mouse position in ImGui (OS desktop) coordinates. */
[[nodiscard]] inline auto mousePosition() -> Vec2
{
    const ImVec2 position = ImGui::GetMousePos();
    return {position.x, position.y};
}

/** @brief Mouse movement since the previous frame, in pixels. */
[[nodiscard]] inline auto mouseDelta() -> Vec2
{
    const ImVec2 delta = ImGui::GetIO().MouseDelta;
    return {delta.x, delta.y};
}

/** @brief Change the cursor mode of the application's main window. */
void setCursorMode(CursorMode mode);

}  // namespace starcore::input
