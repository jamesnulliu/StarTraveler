#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <starcore/app/layer.hpp>
#include <starcore/asset/model.hpp>
#include <starcore/common/image.hpp>
#include <starcore/gpu/gpu_renderer.hpp>
#include <starcore/render/camera.hpp>
#include <starcore/render/path_tracer.hpp>
#include <starcore/render/render_scene.hpp>
#include <starcore/scene/entity.hpp>
#include <starcore/ui/texture.hpp>

#include "editor/editor_context.hpp"
#include "editor/selection.hpp"
#include "editor/tools.hpp"
#include "gameplay/portals.hpp"
#include "gameplay/ship.hpp"
#include "gameplay/space_dust.hpp"

namespace startraveler
{

/** @brief Command-line options. */
struct GameOptions
{
    // Close the application after this many frames (0 = run normally); for
    // smoke tests and CI.
    std::uint32_t exitAfterFrames = 0;
    // Write the composed viewport image here when closing via exitAfterFrames.
    std::filesystem::path screenshotPath;
    // Start with the CPU path tracer even if the GPU supports ray queries.
    bool cpuRenderer = false;
    // Start with ray tracing off (the GPU's raster pipeline).
    bool raster = false;
    // Fly the ship forward at full thrust (smoke tests, screenshots of the effects).
    bool cruise = false;
    // What the hybrid pipeline shows (its debug views; also in the Render panel).
    starcore::HybridView view = starcore::HybridView::Final;
};

struct RenderOptions
{
    // Accumulate path-traced frames continuously (legacy "Render" mode).
    bool liveRender = true;
    // false: rasterize on the GPU (direct light without shadows, no rays), or
    // without a GPU renderer draw the 2D overlay over the plain sky color.
    bool rayTracing = true;
    // Path trace on the GPU (ray queries) when available, else on the CPU.
    bool gpu = true;
    // Preview shading while the view moves, the chosen mode once it is still.
    // The CPU needs it; the GPU path traces in real time.
    bool adaptiveCpu = true;
    bool adaptiveGpu = false;
    // GPU: samples per pixel follow this much GPU time per frame (0: use
    // tracer.samplesPerPixel).
    float gpuBudgetMs = 8.0F;
    // GPU: the hybrid pipeline (rasterized first hits, reprojection and
    // denoising) by default; plain path tracing converges to the reference.
    // Bloom, motion blur and a vignette.
    starcore::GpuRenderOptions gpuRender{
        .pipeline = starcore::GpuPipeline::Hybrid,
        .effects = {.bloom = true,
                    .bloomStrength = 0.05F,
                    .motionBlur = true,
                    .shutter = 0.5F,
                    .vignette = 0.25F},
    };
    starcore::PathTracerSettings tracer{.fireflyClamp = 8.0F};
    // GPU: draw particles (engine exhaust, sparks) and the space dust.
    bool particles = true;
    bool spaceDust = true;
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
              GameOptions options = {});
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
    /** @brief Particles (exhaust, sparks) and the space dust around the camera. */
    void updateParticles(float deltaSeconds);
    void updateViewportSize(starcore::UVec2 size);
    void handleViewportInput();
    [[nodiscard]] auto usingGpu() const -> bool;
    /** @brief The GPU pipeline frames are rendered with (if usingGpu()). */
    [[nodiscard]] auto gpuPipeline() const -> starcore::GpuPipeline;
    [[nodiscard]] auto adaptive() const -> bool;
    void composeFrame();
    void drawOverlay(starcore::ColorImage& target);
    /** @brief The viewport image as shown (GPU frames are read back). */
    [[nodiscard]] auto captureFrame() -> starcore::ColorImage;
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

    starcore::ParticleSystem m_particles;
    SpaceDust m_dust;
    std::vector<starcore::RenderParticle> m_frameParticles;  // Drawn this frame.

    starcore::Camera m_camera{45.0F, 0.1F, 1000.0F};
    starcore::FlyCameraController m_cameraController;
    bool m_cameraActive = false;

    starcore::CpuPathTracer m_tracer;
    std::unique_ptr<starcore::GpuRenderer> m_gpuRenderer;  // Null without a usable GPU.
    std::string m_gpuUnavailable;                           // Why it is null.
    // The render scene, kept up to date incrementally.
    starcore::RenderSceneBuilder m_renderScene;
    starcore::Mat4 m_lastView{0.0F};
    bool m_previewing = false;  // Adaptive rendering is showing the preview.
    RenderOptions m_options;
    bool m_frozen = false;    // Keep the displayed frame (seed fill paints on it).
    bool m_stepOnce = false;  // Trace one frame while live rendering is off.

    // CPU: the traced image with the overlay drawn in (also a frozen frame).
    starcore::ColorImage m_frame;
    std::unique_ptr<starcore::ui::Texture2D> m_texture;
    // GPU: the traced image stays on the GPU; the overlay is a second,
    // transparent texture on top.
    starcore::ui::ImGuiImage m_gpuImage;
    starcore::ColorImage m_overlay;
    starcore::ColorImage m_uploadedOverlay;
    std::unique_ptr<starcore::ui::Texture2D> m_overlayTexture;
    starcore::IdImage m_ids;  // Overlay shapes, for picking.

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

    GameOptions m_gameOptions;
    std::uint32_t m_frameCount = 0;
    bool m_layoutChecked = false;

    float m_traceMs = 0.0F;
    std::string m_status;
    FileDialog m_fileDialog = FileDialog::None;
    std::string m_fileDialogPath;
};

}  // namespace startraveler
