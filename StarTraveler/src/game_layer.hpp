#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

#include <starcore/app/layer.hpp>
#include <starcore/common/image.hpp>
#include <starcore/render/camera.hpp>
#include <starcore/render/path_tracer.hpp>
#include <starcore/render/render_scene.hpp>
#include <starcore/scene/entity.hpp>
#include <starcore/vulkan/texture.hpp>

#include "editor/editor_context.hpp"
#include "editor/selection.hpp"
#include "editor/tools.hpp"
#include "gameplay/portals.hpp"

namespace startraveler
{

/** @brief Command-line driven automation (smoke tests, CI). */
struct AutomationOptions
{
    // Close the application after this many frames (0 = run normally).
    std::uint32_t exitAfterFrames = 0;
    // Write the composed viewport image here when closing via exitAfterFrames.
    std::filesystem::path screenshotPath;
};

struct RenderOptions
{
    // Accumulate path-traced frames continuously (legacy "Render" mode).
    bool liveRender = true;
    // false: draw the 2D overlay over the plain sky color (fast 2D mode).
    bool rayTracing = true;
    bool showHud = true;  // Legacy cockpit dashboard.
    starcore::PathTracerSettings tracer{};
};

/**
 * @brief The StarTraveler game/editor (port of the legacy XDrawBoard Layer):
 *        fly through a path-traced scene, draw 2D shapes with the classic
 *        raster algorithms, edit and group them, open portals.
 */
class GameLayer final : public starcore::Layer
{
public:
    explicit GameLayer(std::filesystem::path scenePath,
                       AutomationOptions automation = {});
    ~GameLayer() override;

    void onAttach() override;
    void onUIRender() override;

    /** @brief Main menu bar contents (see Application::setMenubarCallback). */
    void drawMenuBar();

private:
    enum class FileDialog : std::uint8_t
    {
        None,
        Open,
        SaveAs,
    };

    // Scene files.
    void newScene();
    void openScene(const std::filesystem::path& path);
    void saveScene(const std::filesystem::path& path);
    void resetCamera();
    void onSceneReplaced();

    // Per frame.
    void buildDefaultLayoutIfNeeded();
    void handleShortcuts();
    void updateCamera();
    void updateViewportSize(starcore::UVec2 size);
    void handleViewportInput();
    void composeFrame();
    [[nodiscard]] auto makeContext() -> EditorContext;
    void setTool(ToolKind kind);

    // Panels.
    void drawViewport();
    void drawToolsPanel();
    void drawRenderPanel();
    void drawStatusPanel();
    void drawHierarchyPanel();
    void drawHierarchyNode(starcore::Entity entity);
    void drawInspectorPanel();
    void drawFileDialog();

    // Creation helpers (Create menu).
    auto createAtCursor(const std::string& name) -> starcore::Entity;

    starcore::Scene m_scene;
    std::filesystem::path m_scenePath;

    starcore::Camera m_camera{45.0F, 0.1F, 1000.0F};
    starcore::FlyCameraController m_cameraController;
    bool m_cameraActive = false;

    starcore::CpuPathTracer m_tracer;
    starcore::RenderScene m_renderScene;
    std::uint64_t m_renderSceneSourceVersion = ~0ULL;
    RenderOptions m_options;
    bool m_frozen = false;    // Keep the displayed frame (seed fill paints on it).
    bool m_stepOnce = false;  // Trace one frame while live rendering is off.

    starcore::ColorImage m_frame;
    starcore::IdImage m_ids;
    std::unique_ptr<starcore::vulkan::Texture2D> m_texture;

    ViewportState m_viewport;
    std::optional<entt::entity> m_hovered;  // Entity under the mouse.

    StrokeDefaults m_stroke;
    float m_drawDepth = 5.0F;
    ToolKind m_toolKind = ToolKind::None;
    std::unique_ptr<Tool> m_tool;
    Selection m_selection;
    PortalSystem m_portals;

    // Inspector: Euler angles of the selected entity are kept between frames
    // so editing them does not drift (legacy recomputed them from the
    // quaternion every frame).
    entt::entity m_eulerEntity{entt::null};
    starcore::Quat m_eulerSource{1.0F, 0.0F, 0.0F, 0.0F};
    starcore::Vec3 m_eulerDegrees{0.0F};

    AutomationOptions m_automation;
    std::uint32_t m_frameCount = 0;
    bool m_layoutChecked = false;

    float m_traceMs = 0.0F;
    std::string m_status;
    FileDialog m_fileDialog = FileDialog::None;
    std::string m_fileDialogPath;
};

}  // namespace startraveler
