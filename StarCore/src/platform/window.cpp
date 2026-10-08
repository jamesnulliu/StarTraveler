module;

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include "starcore/common/log.hpp"

module starcore.platform;

import std;
import starcore.math;

namespace starcore
{

namespace
{

int g_windowCount = 0;  // GLFW is initialized while at least one exists.

}  // namespace

Window::Window(const WindowConfig& config)
{
    if (g_windowCount == 0) {
        glfwSetErrorCallback([](int error, const char* description) {
            SC_CORE_ERROR("GLFW error {}: {}", error, description);
        });
        if (glfwInit() == GLFW_FALSE) {
            throw std::runtime_error("failed to initialize GLFW");
        }
    }
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    m_handle = glfwCreateWindow(static_cast<int>(config.width),
                                static_cast<int>(config.height),
                                config.title.c_str(), nullptr, nullptr);
    if (m_handle == nullptr) {
        if (g_windowCount == 0) {
            glfwTerminate();
        }
        throw std::runtime_error("failed to create the GLFW window");
    }
    ++g_windowCount;
}

Window::~Window()
{
    glfwDestroyWindow(m_handle);
    if (--g_windowCount == 0) {
        glfwTerminate();
    }
}

void Window::pollEvents()
{
    glfwPollEvents();
}

auto Window::shouldClose() const -> bool
{
    return glfwWindowShouldClose(m_handle) != 0;
}

void Window::requestClose()
{
    glfwSetWindowShouldClose(m_handle, GLFW_TRUE);
}

auto Window::isMinimized() const -> bool
{
    return glfwGetWindowAttrib(m_handle, GLFW_ICONIFIED) != 0;
}

auto Window::framebufferSize() const -> IVec2
{
    int width = 0;
    int height = 0;
    glfwGetFramebufferSize(m_handle, &width, &height);
    return {width, height};
}

void Window::setTitle(const std::string& title)
{
    glfwSetWindowTitle(m_handle, title.c_str());
}

}  // namespace starcore
