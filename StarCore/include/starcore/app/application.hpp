#pragma once

#include <concepts>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "starcore/app/layer.hpp"
#include "starcore/platform/window.hpp"
#include "starcore/vulkan/context.hpp"

namespace starcore
{

struct ApplicationConfig
{
    std::string name = "StarCore";
    std::uint32_t width = 1600;
    std::uint32_t height = 900;
    bool vsync = true;
    // Needs VK_LAYER_KHRONOS_validation (Vulkan SDK or vcpkg port).
    bool enableValidation = false;
    // ImGui multi-viewports: panels can be dragged out of the main window.
    bool enableViewports = true;
};

/**
 * @brief Window + Vulkan + Dear ImGui (docking) application with a layer
 *        stack (the replacement for Walnut::Application).
 *
 * Frame: poll events -> Layer::onUpdate -> ImGui frame (menu bar, dockspace,
 * Layer::onUIRender) -> texture uploads + UI pass -> present.
 */
class Application
{
public:
    /** @throws std::runtime_error if the window or Vulkan cannot be set up. */
    explicit Application(ApplicationConfig config);
    virtual ~Application();
    Application(const Application&) = delete;
    auto operator=(const Application&) -> Application& = delete;

    void run();
    void close();

    template <std::derived_from<Layer> T, typename... Args>
    auto pushLayer(Args&&... args) -> T&
    {
        auto layer = std::make_unique<T>(std::forward<Args>(args)...);
        T& reference = *layer;
        m_layers.push_back(std::move(layer));
        reference.onAttach();
        return reference;
    }

    /** @brief Called inside ImGui::BeginMainMenuBar() every frame. */
    void setMenubarCallback(std::function<void()> callback)
    {
        m_menubarCallback = std::move(callback);
    }

    [[nodiscard]] auto window() -> Window&
    {
        return *m_window;
    }
    [[nodiscard]] auto vulkan() -> vulkan::Context&
    {
        return *m_vulkan;
    }
    [[nodiscard]] auto config() const -> const ApplicationConfig&
    {
        return m_config;
    }
    /** @brief Duration of the previous frame in seconds. */
    [[nodiscard]] auto deltaTime() const -> float
    {
        return m_deltaTime;
    }

    /** @brief ImGuiID of the main-window dockspace (for DockBuilder layouts). */
    [[nodiscard]] auto dockspaceId() const -> std::uint32_t
    {
        return m_dockspaceId;
    }

    [[nodiscard]] static auto get() -> Application&;

private:
    struct Frame;  // Swapchain + per-frame Vulkan state (ImGui helpers).

    void initImGui();
    void shutdownImGui();
    void rebuildSwapchainIfNeeded();
    void renderFrame();
    void presentFrame();

    ApplicationConfig m_config;
    std::unique_ptr<Window> m_window;
    std::unique_ptr<vulkan::Context> m_vulkan;
    std::unique_ptr<Frame> m_frame;
    std::vector<std::unique_ptr<Layer>> m_layers;
    std::function<void()> m_menubarCallback;

    bool m_running = false;
    float m_deltaTime = 0.0F;
    std::uint32_t m_dockspaceId = 0;
};

}  // namespace starcore
