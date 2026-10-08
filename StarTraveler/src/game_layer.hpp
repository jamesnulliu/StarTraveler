#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

#include <starcore/app/layer.hpp>
#include <starcore/asset/model.hpp>
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
#include "gameplay/ship.hpp"

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
    // Preview shading while the view moves, the chosen mode once it is still.
    bool adaptive = true;
    starcore::PathTracerSettings tracer{.fireflyClamp = 8.0F};
};

enum class ViewMode : std::uint8_t
{
    Pilot,  // From the ship's cockpit (default).
    Free,   // Free-flying editor camera; the ship stays where it is.
};

/**
 * @brief The StarTraveler game/editor (port of the legacy XDrawBoard Layer):
 *        fly a ship through a path-traced scene from its 3D cockpit, draw 2D
 *        shapes with the classic raster algorithms, edit and group them, open
 *        portals.
 */
class GameLayer final : public starcore::Layer
{
public:
    /** @param assetRoot Directory with the game's models/ and scenes/. */
    GameLayer(std::filesystem::path scenePath, std::filesystem::path assetRoot,
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
    void spawnShip();
    void setViewMode(ViewMode mode);

    // Per frame.
    void buildDefaultLayoutIfNeeded();
    void handleShortcuts();
    void updateCamera();
    void updateViewportSize(starcore::UVec2 size);
    void handleViewportInput();
    void composeFrame();
    void drawHud(starcore::Vec2 origin);
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
    void drawLayersPanel();
    void drawFileDialog();

    // Creation helpers (Create menu).
    auto createAtCursor(const std::string& name) -> starcore::Entity;

    starcore::Scene m_scene;
    std::filesystem::path m_scenePath;
    std::filesystem::path m_assetRoot;
    starcore::AssetLibrary m_assets;  // Models referenced by scenes, shared across loads.

    ShipSystem m_ships;
    starcore::Entity m_ship;
    ViewMode m_viewMode = ViewMode::Pilot;
    float m_collisionFlash = 0.0F;  // Seconds left of the collision indicator.

    starcore::Camera m_camera{45.0F, 0.1F, 1000.0F};
    starcore::FlyCameraController m_cameraController;
    bool m_cameraActive = false;

    starcore::CpuPathTracer m_tracer;
    starcore::RenderScene m_renderScene;
    starcore::Mat4 m_lastView{0.0F};
    bool m_previewing = false;  // Adaptive rendering is showing the preview.
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
    std::uint32_t m_activeLayer = 0;  // New drawings and created entities go here.
    bool m_layerRecursive = true;      // Inspector: layer changes include children.
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
