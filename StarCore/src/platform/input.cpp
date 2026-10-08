module;

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

module starcore.platform;

import std;
import starcore.app;

namespace starcore::input
{

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
    if (glfwGetInputMode(window, GLFW_CURSOR) != glfwMode) {
        glfwSetInputMode(window, GLFW_CURSOR, glfwMode);
    }
}

}  // namespace starcore::input
