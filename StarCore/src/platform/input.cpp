#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include "starcore/app/application.hpp"
#include "starcore/platform/input.hpp"

namespace starcore::input
{

namespace
{

// ImGui's keyboard navigation before the cursor was locked (restored after).
bool g_navKeyboard = false;

}  // namespace

void setCursorMode(CursorMode mode)
{
    GLFWwindow* window = Application::get().window().handle();
    int glfwMode = GLFW_CURSOR_NORMAL;
    switch (mode) {
    case CursorMode::Normal:
        glfwMode = GLFW_CURSOR_NORMAL;
        break;
    case CursorMode::Hidden:
        glfwMode = GLFW_CURSOR_HIDDEN;
        break;
    case CursorMode::Locked:
        glfwMode = GLFW_CURSOR_DISABLED;
        break;
    }
    const int previous = glfwGetInputMode(window, GLFW_CURSOR);
    if (previous == glfwMode) {
        return;
    }
    glfwSetInputMode(window, GLFW_CURSOR, glfwMode);

    ImGuiIO& io = ImGui::GetIO();
    if (glfwMode == GLFW_CURSOR_DISABLED) {
        g_navKeyboard = (io.ConfigFlags & ImGuiConfigFlags_NavEnableKeyboard) != 0;
        io.ConfigFlags |= ImGuiConfigFlags_NoMouse;
        io.ConfigFlags &= ~ImGuiConfigFlags_NavEnableKeyboard;
    } else if (previous == GLFW_CURSOR_DISABLED) {
        io.ConfigFlags &= ~ImGuiConfigFlags_NoMouse;
        if (g_navKeyboard) {
            io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        }
    }
}

}  // namespace starcore::input
