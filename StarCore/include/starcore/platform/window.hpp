#pragma once

#include <cstdint>
#include <string>

#include "starcore/math/types.hpp"

struct GLFWwindow;

namespace starcore
{

struct WindowConfig
{
    std::string title = "StarCore";
    std::uint32_t width = 1600;
    std::uint32_t height = 900;
};

/** @brief GLFW window without a graphics context (Vulkan renders into it). */
class Window
{
public:
    explicit Window(const WindowConfig& config);
    ~Window();
    Window(const Window&) = delete;
    auto operator=(const Window&) -> Window& = delete;

    static void pollEvents();

    [[nodiscard]] auto handle() const noexcept -> GLFWwindow*
    {
        return m_handle;
    }
    [[nodiscard]] auto shouldClose() const -> bool;
    void requestClose();
    [[nodiscard]] auto isMinimized() const -> bool;
    [[nodiscard]] auto framebufferSize() const -> IVec2;
    void setTitle(const std::string& title);

private:
    GLFWwindow* m_handle = nullptr;
};

}  // namespace starcore
