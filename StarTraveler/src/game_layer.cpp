#include <algorithm>
#include <bit>
#include <chrono>
#include <format>
#include <iterator>
#include <ranges>
#include <vector>

#include <glm/geometric.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/trigonometric.hpp>
#include <imgui.h>
#include <imgui_internal.h>  // DockBuilder API
#include <imgui_stdlib.h>

#include <starcore/app/application.hpp>
#include <starcore/common/log.hpp>
#include <starcore/math/color.hpp>
#include <starcore/physics/physics_world.hpp>
#include <starcore/platform/input.hpp>
#include <starcore/render/overlay.hpp>
#include <starcore/scene/serializer.hpp>

#include "game_layer.hpp"

import starcore.raster;

namespace startraveler
{

using namespace starcore;

namespace
{

constexpr const char* kEntityPayload = "STARCORE_ENTITY";

auto color3Edit(const char* label, Vec3& value) -> bool
{
    return ImGui::ColorEdit3(label, &value.x);
}

auto color4Edit(const char* label, Vec4& value) -> bool
{
    return ImGui::ColorEdit4(label, &value.x);
}

template <typename E>
auto enumCombo(const char* label, E& value) -> bool
{
    bool changed = false;
    if (ImGui::BeginCombo(label, enumToString(value).data())) {
        for (std::size_t i = 0; i < EnumNames<E>::kNames.size(); ++i) {
            const auto option = static_cast<E>(i);
            if (ImGui::Selectable(EnumNames<E>::kNames[i], option == value)) {
                value = option;
                changed = true;
            }
        }
        ImGui::EndCombo();
    }
    return changed;
}

/** @brief Collapsing header with a remove button; returns {open, remove}. */
auto componentHeader(const char* label, bool removable) -> std::pair<bool, bool>
{
    ImGui::PushID(label);
    const float rightEdge = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
    const bool open = ImGui::CollapsingHeader(label, ImGuiTreeNodeFlags_DefaultOpen |
                                                         ImGuiTreeNodeFlags_AllowOverlap);
    bool remove = false;
    if (removable) {
        ImGui::SameLine(rightEdge - ImGui::GetFrameHeight());
        remove = ImGui::SmallButton("X");
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Remove component");
        }
    }
    ImGui::PopID();
    return {open, remove};
}

auto cubeMesh() -> MeshComponent
{
    return MeshComponent{.mesh = primitives::box(Vec3(0.5F)).build()};
}

auto planeMesh(float size) -> MeshComponent
{
    return MeshComponent{.mesh = primitives::plane(Vec2(size)).build()};
}

/** @brief Alpha-blend `overlay` (straight alpha) over the opaque `base`. */
void compositeOver(ColorImage& base, const ColorImage& overlay)
{
    for (std::size_t i = 0; i < base.pixels().size(); ++i) {
        const std::uint32_t top = overlay.pixels()[i];
        const std::uint32_t alpha = top >> 24U;
        if (alpha == 255U) {
            base.pixels()[i] = top;
        } else if (alpha != 0U) {
            const Vec4 a = unpackRgba8(top);
            const Vec4 b = unpackRgba8(base.pixels()[i]);
            base.pixels()[i] = packRgba8(Vec4(Vec3(a) * a.a + Vec3(b) * (1.0F - a.a), 1.0F));
        }
    }
}

}  // namespace

GameLayer::GameLayer(std::filesystem::path scenePath, std::filesystem::path assetRoot,
                     GameOptions options)
    : m_scenePath(std::move(scenePath)), m_assetRoot(std::move(assetRoot)),
      m_gameOptions(std::move(options))
{
}

GameLayer::~GameLayer() = default;

void GameLayer::onAttach()
{
    vulkan::Context& vulkan = Application::get().vulkan();
    m_texture = std::make_unique<ui::Texture2D>(vulkan);
    m_overlayTexture = std::make_unique<ui::Texture2D>(vulkan);
    if (auto renderer = GpuRenderer::create(vulkan)) {
        m_gpuRenderer = std::move(*renderer);
        if (!m_gpuRenderer->rayTracing()) {
            m_gpuUnavailable = std::format("{} has no ray queries", vulkan.deviceInfo().name);
            SC_WARN("{}: path tracing on the CPU, rasterizing on the GPU", m_gpuUnavailable);
        }
    } else {
        m_gpuUnavailable = renderer.error();
        SC_WARN("GPU renderer unavailable ({}); using the CPU", m_gpuUnavailable);
    }
    m_options.gpu =
        m_gpuRenderer != nullptr && m_gpuRenderer->rayTracing() && !m_gameOptions.cpuRenderer;
    m_options.rayTracing = !m_gameOptions.raster;
    m_options.gpuRender.view = m_gameOptions.view;
    m_options.gpuRender.upscaling = m_gameOptions.upscaling;
    resetCamera();
    if (!m_scenePath.empty()) {
        openScene(m_scenePath);
    }
    if (!m_ship.isValid()) {
        onSceneReplaced();  // No scene file: an empty scene with the ship.
    }
}

// Scene files ---------------------------------------------------------------------------------------

void GameLayer::newScene()
{
    m_scene.clear();
    m_scenePath.clear();
    onSceneReplaced();
    m_status = "New scene";
}

void GameLayer::openScene(const std::filesystem::path& path)
{
    if (auto loaded = loadScene(path, m_scene, &m_assets); !loaded) {
        m_status = loaded.error();
        SC_ERROR("{}", loaded.error());
        return;
    }
    m_scenePath = path;
    onSceneReplaced();
    m_status = std::format("Opened {}", path.string());
    SC_INFO("{}", m_status);
}

void GameLayer::saveScene(const std::filesystem::path& path)
{
    if (auto saved = starcore::saveScene(m_scene, path); !saved) {
        m_status = saved.error();
        SC_ERROR("{}", saved.error());
        return;
    }
    m_scenePath = path;
    m_status = std::format("Saved {}", path.string());
    SC_INFO("{}", m_status);
}

void GameLayer::onSceneReplaced()
{
    m_particles.clear();
    m_selection.clear();
    setTool(ToolKind::None);
    m_portals.reset();
    m_eulerEntity = entt::null;
    m_frozen = false;
    spawnShip();
}

void GameLayer::resetCamera()
{
    m_camera.setPosition({0.0F, 0.0F, 10.0F});
    m_camera.setRotation(-90.0F, 0.0F);
    m_portals.reset();
}

void GameLayer::spawnShip()
{
    if (m_ship.isValid()) {
        m_scene.destroyEntity(m_ship);
    }
    m_ship = {};
    configureGameLayers(m_scene.layers());

    // At the scene's "PlayerStart" entity (looking down its -Z), or where the
    // legacy camera started.
    Vec3 position{0.0F, 0.0F, 13.0F};
    float yaw = -90.0F;
    float pitch = 0.0F;
    if (const Entity start = m_scene.findEntityByName("PlayerStart")) {
        const Mat4 world = start.worldTransform();
        position = Vec3(world[3]);
        const Vec3 forward = glm::normalize(Vec3(world * Vec4(0.0F, 0.0F, -1.0F, 0.0F)));
        yaw = glm::degrees(std::atan2(forward.z, forward.x));
        pitch = glm::degrees(std::asin(std::clamp(forward.y, -1.0F, 1.0F)));
    }
    auto ship = m_ships.spawn(m_scene, m_assets, m_assetRoot / "models" / "cockpit.glb", position,
                              yaw, pitch);
    if (!ship) {
        m_status = std::format("No ship: {}", ship.error());
        SC_ERROR("{}", m_status);
        setViewMode(ViewMode::Free);
        return;
    }
    m_ship = *ship;
    setViewMode(m_viewMode);
}

void GameLayer::setViewMode(ViewMode mode)
{
    if (mode == ViewMode::Pilot && !m_ship.isValid()) {
        mode = ViewMode::Free;
    }
    m_viewMode = mode;
    // A wider view from the cockpit.
    m_camera.setVerticalFov(mode == ViewMode::Pilot ? 70.0F : 45.0F);
    if (mode == ViewMode::Pilot) {
        m_ships.attachCamera(m_camera, m_ship);
    }
    m_portals.reset();
}

// Per-frame logic -------------------------------------------------------------------------------------

auto GameLayer::makeContext() -> EditorContext
{
    return EditorContext{.scene = m_scene,
                         .camera = m_camera,
                         .viewport = m_viewport,
                         .stroke = m_stroke,
                         .drawDepth = m_drawDepth};
}

void GameLayer::setTool(ToolKind kind)
{
    m_toolKind = kind;
    m_tool = makeTool(kind);
    if (kind != ToolKind::None && kind != ToolKind::Select) {
        m_selection.clear();
    }
    if (kind != ToolKind::None) {
        // Editing: the clicks go to the tool.
        m_captured = false;
        m_paused = false;
    }
}

void GameLayer::setPaused(bool paused)
{
    m_paused = paused;
    m_captured = !paused;  // Resuming flies on.
    m_focusPauseMenu = paused;
}

void GameLayer::updateMouseCapture()
{
    const ImGuiIO& io = ImGui::GetIO();
    const bool escape = !io.WantTextInput &&
                        !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId) &&
                        input::isKeyPressed(ImGuiKey_Escape);
    const bool clicked = m_viewport.hovered && input::isMouseClicked(ImGuiMouseButton_Left);
    if (m_captured) {
        // Another application taking the focus pauses too.
        if (escape || !Application::get().window().isFocused()) {
            setPaused(true);
        }
    } else if (m_paused) {
        if (escape || clicked) {
            setPaused(false);
        }
    } else if (escape) {
        if (m_toolKind != ToolKind::None) {
            setTool(ToolKind::None);  // Cancel drawing, drop the selection.
            m_selection.clear();
        } else {
            setPaused(true);
        }
    } else if (clicked && m_toolKind == ToolKind::None) {
        m_captured = true;
    }

    // The editing tools look around while RMB is held over the view.
    if (!m_dragLook && !m_captured && !m_paused && m_viewport.hovered &&
        input::isMouseClicked(ImGuiMouseButton_Right)) {
        m_dragLook = true;
    }
    if (m_dragLook && (m_captured || m_paused || !input::isMouseDown(ImGuiMouseButton_Right))) {
        m_dragLook = false;
    }
    input::setCursorMode(looking() ? input::CursorMode::Locked : input::CursorMode::Normal);
}

void GameLayer::handleShortcuts()
{
    const ImGuiIO& io = ImGui::GetIO();
    if (io.WantTextInput) {
        return;
    }
    // While flying Ctrl goes down: the editing shortcuts wait for the mouse.
    const bool ctrl = io.KeyCtrl && !looking();
    if (ctrl && input::isKeyPressed(ImGuiKey_N)) {
        newScene();
    } else if (ctrl && input::isKeyPressed(ImGuiKey_O)) {
        m_fileDialog = FileDialog::Open;
    } else if (ctrl && input::isKeyPressed(ImGuiKey_S)) {
        if (m_scenePath.empty()) {
            m_fileDialog = FileDialog::SaveAs;
        } else {
            saveScene(m_scenePath);
        }
    } else if (ctrl && input::isKeyPressed(ImGuiKey_D) && m_selection.selected()) {
        Entity copy = m_scene.copyEntity(m_selection.selected());
        m_selection.select(copy);
    }

    if (m_paused || (!looking() && !m_viewport.hovered)) {
        return;
    }
    // Legacy: T opens a portal in front of the camera.
    if (!ctrl && input::isKeyPressed(ImGuiKey_T)) {
        Entity portal = m_portals.createPortal(m_scene, m_camera, kPortalLayer);
        m_status = std::format("Created {}", portal.name());
    }
    if (!ctrl && input::isKeyPressed(ImGuiKey_C)) {
        setViewMode(m_viewMode == ViewMode::Pilot ? ViewMode::Free : ViewMode::Pilot);
    }
}

void GameLayer::updateCamera()
{
    if (m_paused) {
        return;
    }
    const float deltaSeconds = Application::get().deltaTime();
    m_collisionFlash = std::max(0.0F, m_collisionFlash - deltaSeconds);

    // W/S/A/D, Space up, Ctrl down, while the mouse steers.
    const ImGuiIO& io = ImGui::GetIO();
    const bool keys = looking() && !io.WantTextInput;
    auto key = [&](ImGuiKey name) { return keys && input::isKeyDown(name); };
    auto axis = [](bool positive, bool negative) {
        return static_cast<float>(positive) - static_cast<float>(negative);
    };
    const bool up = key(ImGuiKey_Space);
    const bool down = keys && io.KeyCtrl;
    const Vec2 look = looking() ? input::mouseDelta() : Vec2(0.0F);

    if (m_viewMode == ViewMode::Pilot && m_ship.isValid()) {
        const ShipInput shipInput{
            .thrust = {axis(key(ImGuiKey_D), key(ImGuiKey_A)), axis(up, down),
                       m_gameOptions.cruise ? 1.0F : axis(key(ImGuiKey_W), key(ImGuiKey_S))},
            .look = look,
            .boost = keys && io.KeyShift,
        };
        const PhysicsWorld physics = PhysicsWorld::build(m_scene);
        const ShipEvents events =
            m_ships.update(m_scene, m_ship, physics, shipInput, deltaSeconds);
        if (events.collided) {
            m_collisionFlash = 0.4F;
            // Sparks off the hull: small, hot, quickly cooling and slowing.
            const ParticleSettings sparks{.lifetime = {0.15F, 0.4F},
                                          .speed = {2.0F, 6.0F},
                                          .spread = 1.4F,
                                          .size = {0.012F, 0.0F},
                                          .startColor = {6.0F, 3.0F, 0.9F},
                                          .endColor = {0.8F, 0.1F, 0.0F},
                                          .drag = 2.5F};
            m_particles.emit(sparks, events.contactPoint, events.contactNormal,
                             m_ship.get<ShipComponent>().velocity, 8);
        }
        if (events.teleported) {
            m_status = "Teleported!";
        }
        m_ships.attachCamera(m_camera, m_ship);
        return;
    }

    const CameraInput cameraInput{
        .forward = key(ImGuiKey_W),
        .backward = key(ImGuiKey_S),
        .left = key(ImGuiKey_A),
        .right = key(ImGuiKey_D),
        .up = up,
        .down = down,
        .look = looking(),
        .mouseDelta = look,
    };
    m_cameraController.update(m_camera, cameraInput, deltaSeconds);
    if (m_portals.update(m_scene, m_camera)) {
        m_status = "Teleported!";
    }
}

void GameLayer::updateParticles(float deltaSeconds)
{
    m_particles.update(m_scene, deltaSeconds);
    m_dust.update(m_camera.position());
}

void GameLayer::updateViewportSize(UVec2 size)
{
    size = glm::max(size, UVec2(1));
    if (size == m_viewport.size) {
        return;
    }
    m_viewport.size = size;
    m_camera.setViewport(size.x, size.y);
    m_tracer.resize(size.x, size.y);
    if (m_gpuRenderer) {
        m_gpuRenderer->resize(size.x, size.y);
    }
    m_frame.resize(size.x, size.y, 0U);
    m_overlay.resize(size.x, size.y, 0U);
    m_ids.resize(size.x, size.y, kNoEntityId);
    m_frozen = false;
}

void GameLayer::handleViewportInput()
{
    EditorContext context = makeContext();

    if (m_toolKind == ToolKind::SeedFill) {
        if (context.viewport.acceptsInput() && input::isMouseClicked(ImGuiMouseButton_Left)) {
            // Seed fill paints the displayed frame, so freeze it (legacy only
            // allowed seed fill while rendering was stopped).
            if (!m_frozen) {
                m_frame = captureFrame();
            }
            m_frozen = true;
            const std::size_t filled = raster::floodFill(
                m_frame, raster::toPixel(m_viewport.mouse), packRgba8(m_stroke.primary));
            m_texture->setPixels(m_frame.pixels(), m_frame.width(), m_frame.height());
            m_status = std::format("Seed fill: {} pixels (frame frozen; unfreeze "
                                   "in Render settings)", filled);
        }
        return;
    }

    if (m_tool) {
        const ToolResult result = m_tool->update(context);
        if (result.finished) {
            m_scene.setLayer(result.created, m_activeLayer, /*recursive=*/true);
            m_status = std::format("Created {}", result.created.name());
            // Stay in the tool to draw more shapes of the same kind.
        }
        return;
    }

    if (m_toolKind == ToolKind::Select) {
        m_selection.update(context, m_hovered);
    }
}

auto GameLayer::usingGpu() const -> bool
{
    if (m_gpuRenderer == nullptr) {
        return false;
    }
    // Ray tracing on the GPU if chosen and possible; rasterizing always is.
    return !m_options.rayTracing || (m_options.gpu && m_gpuRenderer->rayTracing());
}

auto GameLayer::gpuPipeline() const -> GpuPipeline
{
    return m_options.rayTracing ? m_options.gpuRender.pipeline : GpuPipeline::Raster;
}

auto GameLayer::adaptive() const -> bool
{
    return usingGpu() ? m_options.adaptiveGpu : m_options.adaptiveCpu;
}

void GameLayer::drawOverlay(ColorImage& target)
{
    const SceneLayers& layers = m_scene.layers();
    m_ids.fill(kNoEntityId);
    renderOverlay(m_scene, m_camera, target, &m_ids, layers.visibleMask(),
                  layers.pickableMask());
    if (m_tool) {
        renderOverlay(m_tool->previewScene(), m_camera, target);
    }
    m_selection.drawHandles(makeContext(), target);
}

void GameLayer::composeFrame()
{
    if (m_frozen) {
        return;
    }
    // Only what changed is rewritten (and uploaded to the GPU).
    const RenderScene& renderScene = m_renderScene.update(m_scene);

    PathTracerSettings settings = m_options.tracer;
    settings.instanceMask = m_scene.layers().visibleMask();
    // Adaptive: noise-free preview while moving, accumulate once still.
    m_previewing = adaptive() && m_camera.view() != m_lastView;
    m_lastView = m_camera.view();
    if (m_previewing) {
        settings.mode = RenderMode::Preview;
    }
    const bool gpu = usingGpu();
    if ((m_options.rayTracing || gpu) && (m_options.liveRender || m_stepOnce)) {
        const auto start = std::chrono::steady_clock::now();
        if (gpu) {
            GpuRenderOptions options = m_options.gpuRender;
            options.pipeline = gpuPipeline();
            if (m_options.gpuBudgetMs > 0.0F && tracesRays(options.pipeline)) {
                settings.samplesPerPixel = m_gpuRenderer->samplesForBudget(m_options.gpuBudgetMs);
            }
            m_frameParticles.clear();
            if (m_options.particles) {
                std::ranges::copy(m_particles.particles(), std::back_inserter(m_frameParticles));
            }
            if (m_options.spaceDust) {
                m_dust.appendParticles(m_frameParticles, m_camera.position());
            }
            m_gpuRenderer->render(renderScene, m_camera, settings, options, m_frameParticles);
        } else {
            // Coarse first passes and a per-frame time budget keep the UI
            // responsive while the image converges; the CPU traverses a TLAS.
            m_renderScene.updateTlas();
            m_tracer.render(renderScene, m_camera, settings,
                            {.timeBudgetMs = 30.0F, .firstPassStride = m_previewing ? 3U : 4U});
        }
        m_traceMs = std::chrono::duration<float, std::milli>(
                        std::chrono::steady_clock::now() - start)
                        .count();
        m_stepOnce = false;
    }

    if (gpu) {
        // The traced image never leaves the GPU; only the overlay is uploaded
        // (when it changed).
        m_overlay.fill(0U);
        drawOverlay(m_overlay);
        if (!std::ranges::equal(m_overlay.pixels(), m_uploadedOverlay.pixels())) {
            m_overlayTexture->setPixels(m_overlay.pixels(), m_overlay.width(), m_overlay.height());
            m_uploadedOverlay = m_overlay;
        }
        m_gpuImage.set(m_gpuRenderer->outputView(), GpuRenderer::kOutputLayout);
        return;
    }

    if (m_options.rayTracing) {
        std::ranges::copy(m_tracer.image().pixels(), m_frame.pixels().begin());
    } else {
        const Vec3 sky = linearToSrgb(glm::clamp(m_scene.environment().skyColor, 0.0F, 1.0F));
        m_frame.fill(packRgba8(Vec4(sky, 1.0F)));
    }
    drawOverlay(m_frame);
    m_texture->setPixels(m_frame.pixels(), m_frame.width(), m_frame.height());
}

auto GameLayer::captureFrame() -> ColorImage
{
    if (m_frozen || !usingGpu()) {
        return m_frame;
    }
    ColorImage frame = m_gpuRenderer->readImage();
    compositeOver(frame, m_overlay);
    return frame;
}

void GameLayer::buildDefaultLayoutIfNeeded()
{
    if (m_layoutChecked) {
        return;
    }
    m_layoutChecked = true;
    const ImGuiID dockspace = Application::get().dockspaceId();
    const ImGuiDockNode* node = ImGui::DockBuilderGetNode(dockspace);
    // A saved layout (imgui.ini) leaves a split node; keep it.
    if (node == nullptr || !node->IsLeafNode() || node->Windows.Size != 0) {
        return;
    }

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::DockBuilderRemoveNode(dockspace);
    // ImGuiDockNodeFlags_DockSpace lives in the private flags enum.
    ImGui::DockBuilderAddNode(dockspace,
                              static_cast<ImGuiDockNodeFlags>(ImGuiDockNodeFlags_DockSpace) |
                                  ImGuiDockNodeFlags_PassthruCentralNode);
    ImGui::DockBuilderSetNodeSize(dockspace, viewport->WorkSize);

    ImGuiID center = dockspace;
    ImGuiID left = ImGui::DockBuilderSplitNode(center, ImGuiDir_Left, 0.20F, nullptr, &center);
    ImGuiID right = ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.27F, nullptr, &center);
    ImGuiID leftBottom = ImGui::DockBuilderSplitNode(left, ImGuiDir_Down, 0.55F, nullptr, &left);
    ImGuiID rightBottom = ImGui::DockBuilderSplitNode(right, ImGuiDir_Down, 0.45F, nullptr, &right);

    ImGui::DockBuilderDockWindow("Viewport", center);
    ImGui::DockBuilderDockWindow("Tools", left);
    ImGui::DockBuilderDockWindow("Hierarchy", leftBottom);
    ImGui::DockBuilderDockWindow("Layers", leftBottom);
    ImGui::DockBuilderDockWindow("Inspector", right);
    ImGui::DockBuilderDockWindow("Render", rightBottom);
    ImGui::DockBuilderDockWindow("Status", rightBottom);
    ImGui::DockBuilderFinish(dockspace);
}

void GameLayer::onUIRender()
{
    buildDefaultLayoutIfNeeded();
    updateMouseCapture();
    handleShortcuts();
    updateCamera();
    if (!m_paused) {
        updateParticles(Application::get().deltaTime());
    }

    drawViewport();
    drawToolsPanel();
    drawRenderPanel();
    drawStatusPanel();
    drawHierarchyPanel();
    drawInspectorPanel();
    drawLayersPanel();
    drawFileDialog();

    ++m_frameCount;
    if (m_gameOptions.exitAfterFrames != 0 &&
        m_frameCount >= m_gameOptions.exitAfterFrames) {
        if (!m_gameOptions.screenshotPath.empty()) {
            if (auto written = writePng(captureFrame(), m_gameOptions.screenshotPath); !written) {
                SC_ERROR("{}", written.error());
            } else {
                SC_INFO("Screenshot written to {}", m_gameOptions.screenshotPath.string());
            }
        }
        if (usingGpu()) {
            SC_INFO("Exiting after {} frames ({} accumulated on the GPU, {} pipeline, last "
                    "{:.2f} ms, {:.2f} tracing, for {} samples per pixel)",
                    m_frameCount, m_gpuRenderer->accumulatedFrames(),
                    enumToString(gpuPipeline()), m_gpuRenderer->gpuTimeMs(),
                    m_gpuRenderer->traceTimeMs(),
                    m_options.gpuBudgetMs > 0.0F
                        ? m_gpuRenderer->samplesForBudget(m_options.gpuBudgetMs)
                        : m_options.tracer.samplesPerPixel);
        } else {
            SC_INFO("Exiting after {} frames ({} accumulated on the CPU)", m_frameCount,
                    m_tracer.accumulatedFrames());
        }
        Application::get().close();
    }
}

void GameLayer::drawHud(Vec2 origin)
{
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const Vec2 size(m_viewport.size);
    const ImVec2 center(origin.x + size.x * 0.5F, origin.y + size.y * 0.5F);
    const ImU32 white = IM_COL32(230, 240, 255, 170);

    const bool pilot = m_viewMode == ViewMode::Pilot && m_ship.isValid();
    if (pilot || m_captured) {
        // Crosshair: the ship's heading, or where the camera looks.
        draw->AddLine({center.x - 12, center.y}, {center.x - 4, center.y}, white, 1.5F);
        draw->AddLine({center.x + 4, center.y}, {center.x + 12, center.y}, white, 1.5F);
        draw->AddLine({center.x, center.y - 12}, {center.x, center.y - 4}, white, 1.5F);
        draw->AddLine({center.x, center.y + 4}, {center.x, center.y + 12}, white, 1.5F);
    }
    if (!m_captured && !m_paused && m_toolKind == ToolKind::None) {
        const char* prompt = "Click to fly";
        const ImVec2 promptSize = ImGui::CalcTextSize(prompt);
        const ImVec2 corner(center.x - promptSize.x * 0.5F, center.y + 24.0F);
        draw->AddRectFilled({corner.x - 8, corner.y - 4},
                            {corner.x + promptSize.x + 8, corner.y + promptSize.y + 4},
                            IM_COL32(0, 0, 0, 140), 4.0F);
        draw->AddText(corner, white, prompt);
    }

    std::string text;
    if (pilot) {
        text = std::format("SPEED {:5.1f}   W/S/A/D thrust, Space/Ctrl up/down, Shift boost, "
                           "C free camera, T portal, Esc pause",
                           glm::length(m_ship.get<ShipComponent>().velocity));
    } else {
        text = "FREE CAMERA   W/S/A/D, Space/Ctrl up/down, C back to the cockpit, Esc pause";
    }
    if (m_toolKind != ToolKind::None) {
        text = std::format("{}: hold RMB to look around, Esc back to flying",
                           toolName(m_toolKind));
    }
    if (m_previewing) {
        text += "   [preview while moving]";
    }
    const ImVec2 corner(origin.x + 10.0F, origin.y + 10.0F);
    const ImVec2 textSize = ImGui::CalcTextSize(text.c_str());
    draw->AddRectFilled({corner.x - 6, corner.y - 4},
                        {corner.x + textSize.x + 6, corner.y + textSize.y + 4},
                        IM_COL32(0, 0, 0, 120), 4.0F);
    draw->AddText(corner, white, text.c_str());

    if (m_collisionFlash > 0.0F) {
        const auto alpha = static_cast<int>(255.0F * std::min(1.0F, m_collisionFlash / 0.4F));
        draw->AddRect({origin.x + 2, origin.y + 2}, {origin.x + size.x - 2, origin.y + size.y - 2},
                      IM_COL32(255, 60, 40, alpha), 0.0F, 0, 4.0F);
        const char* warning = "COLLISION";
        const ImVec2 warningSize = ImGui::CalcTextSize(warning);
        draw->AddText({center.x - warningSize.x * 0.5F, center.y + 30.0F},
                      IM_COL32(255, 80, 60, alpha), warning);
    }
}

void GameLayer::drawPauseMenu(Vec2 origin, std::uint32_t platformViewport)
{
    // In the middle of the view.
    const Vec2 size(m_viewport.size);
    ImGui::SetNextWindowPos({origin.x + size.x * 0.5F, origin.y + size.y * 0.5F},
                            ImGuiCond_Always, {0.5F, 0.5F});
    ImGui::SetNextWindowViewport(platformViewport);
    if (m_focusPauseMenu) {
        ImGui::SetNextWindowFocus();
        m_focusPauseMenu = false;
    }
    constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_NoDecoration |
                                        ImGuiWindowFlags_AlwaysAutoResize |
                                        ImGuiWindowFlags_NoSavedSettings |
                                        ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoMove;
    if (ImGui::Begin("Paused", nullptr, kFlags)) {
        const float width = ImGui::GetFontSize() * 16.0F;
        const char* title = "PAUSED";
        ImGui::SetCursorPosX((width - ImGui::CalcTextSize(title).x) * 0.5F +
                             ImGui::GetStyle().WindowPadding.x);
        ImGui::TextUnformatted(title);
        ImGui::Spacing();
        if (ImGui::Button("Resume", {width, 0.0F})) {
            setPaused(false);
        }
        const bool pilot = m_viewMode == ViewMode::Pilot;
        ImGui::BeginDisabled(!m_ship.isValid());
        if (ImGui::Button(pilot ? "Free camera" : "Back to the cockpit", {width, 0.0F})) {
            setViewMode(pilot ? ViewMode::Free : ViewMode::Pilot);
        }
        ImGui::EndDisabled();
        if (ImGui::Button("Respawn ship", {width, 0.0F})) {
            spawnShip();
        }
        if (ImGui::Button("Quit", {width, 0.0F})) {
            Application::get().close();
        }
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + width);
        ImGui::TextDisabled("Click the view or press Esc to fly on. Mouse: steer. W/S/A/D: "
                            "thrust. Space/Ctrl: up/down. Shift: boost. C: free camera. "
                            "T: portal. The panels stay usable while paused.");
        ImGui::PopTextWrapPos();
    }
    ImGui::End();
}

// Panels ----------------------------------------------------------------------------------------------

void GameLayer::drawViewport()
{
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0F, 0.0F));
    ImGui::Begin("Viewport");

    const ImVec2 available = ImGui::GetContentRegionAvail();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    updateViewportSize({static_cast<std::uint32_t>(std::max(1.0F, available.x)),
                        static_cast<std::uint32_t>(std::max(1.0F, available.y))});

    const ImVec2 mouse = ImGui::GetMousePos();
    m_viewport.mouse = {mouse.x - origin.x, mouse.y - origin.y};
    m_viewport.hovered = ImGui::IsWindowHovered() && m_viewport.mouse.x >= 0.0F &&
                         m_viewport.mouse.y >= 0.0F &&
                         m_viewport.mouse.x < static_cast<float>(m_viewport.size.x) &&
                         m_viewport.mouse.y < static_cast<float>(m_viewport.size.y);
    m_viewport.cameraActive = looking();

    handleViewportInput();
    composeFrame();

    const ImVec2 imageSize(static_cast<float>(m_viewport.size.x),
                           static_cast<float>(m_viewport.size.y));
    if (usingGpu() && !m_frozen) {
        if (m_gpuImage.imguiTexture() != 0) {
            ImGui::Image(m_gpuImage.imguiTexture(), imageSize);
        }
        if (m_overlayTexture->imguiTexture() != 0) {
            ImGui::GetWindowDrawList()->AddImage(
                m_overlayTexture->imguiTexture(), origin,
                ImVec2(origin.x + imageSize.x, origin.y + imageSize.y));
        }
    } else if (m_texture->imguiTexture() != 0) {
        ImGui::Image(m_texture->imguiTexture(), imageSize);
    }
    drawHud({origin.x, origin.y});
    if (m_paused) {
        // Dimmed under the pause menu.
        ImGui::GetWindowDrawList()->AddRectFilled(
            origin, ImVec2(origin.x + imageSize.x, origin.y + imageSize.y), IM_COL32(0, 0, 0, 110));
    }
    const ImGuiID platformViewport = ImGui::GetWindowViewport()->ID;

    // Entity under the mouse, for the status bar and the next click.
    m_hovered.reset();
    if (m_viewport.hovered) {
        // The first visible thing under the mouse, unless its layer is locked
        // (so clicks on the cockpit do not select what is behind it).
        const SceneLayers& layers = m_scene.layers();
        m_hovered = pickEntity(m_renderScene.renderScene(), m_camera, &m_ids, m_viewport.mouse,
                               layers.visibleMask());
        if (m_hovered && (layers.pickableMask() & layerBit(m_scene.layerOf(*m_hovered))) == 0U) {
            m_hovered.reset();
        }
        if (m_hovered && !m_scene.registry().valid(*m_hovered)) {
            m_hovered.reset();
        }
    }

    ImGui::End();
    ImGui::PopStyleVar();
    if (m_paused) {
        drawPauseMenu({origin.x, origin.y}, platformViewport);
    }
}

void GameLayer::drawToolsPanel()
{
    ImGui::Begin("Tools");
    constexpr ToolKind kTools[] = {ToolKind::None,  ToolKind::Select,  ToolKind::Line,
                                   ToolKind::Ellipse, ToolKind::Arc,   ToolKind::Polygon,
                                   ToolKind::Curve, ToolKind::Group,   ToolKind::SeedFill};
    for (const ToolKind kind : kTools) {
        const bool active = kind == m_toolKind;
        if (active) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        }
        if (ImGui::Button(toolName(kind).data())) {
            setTool(kind);
        }
        if (active) {
            ImGui::PopStyleColor();
        }
        ImGui::SameLine();
        if (ImGui::GetContentRegionAvail().x < 90.0F) {
            ImGui::NewLine();
        }
    }
    ImGui::NewLine();
    if (m_tool) {
        ImGui::TextWrapped("%s", m_tool->hint().data());
    } else if (m_toolKind == ToolKind::SeedFill) {
        ImGui::TextWrapped("Click inside a region of the frame to flood-fill it with "
                           "Color 1 (freezes the frame).");
    } else if (m_toolKind == ToolKind::Select) {
        ImGui::TextWrapped("Click to select, drag to move, drag green handles to edit. "
                           "Ctrl+click: add child. Del: delete. Ctrl+D: duplicate. "
                           "Hold RMB to look around. Esc: back to flying.");
    } else {
        ImGui::TextWrapped("Click the view to fly: the mouse steers, W/S/A/D thrust, "
                           "Space/Ctrl up/down, Shift boost. Esc pauses and frees the mouse. "
                           "T: portal. C: cockpit / free camera.");
    }
    ImGui::Separator();
    color4Edit("Color 1", m_stroke.primary);
    color4Edit("Color 2", m_stroke.secondary);
    int thickness = static_cast<int>(m_stroke.thickness);
    if (ImGui::SliderInt("Thickness", &thickness, 1, 25)) {
        m_stroke.thickness = static_cast<std::uint32_t>(thickness);
    }
    enumCombo("Pattern", m_stroke.pattern);
    ImGui::DragFloat("Draw depth", &m_drawDepth, 0.1F, 0.5F, 500.0F, "%.1f");
    ImGui::End();
}

void GameLayer::drawRenderPanel()
{
    ImGui::Begin("Render");
    const auto& device = Application::get().vulkan().deviceInfo();
    ImGui::Text("GPU: %s", device.name.c_str());
    if (m_gpuRenderer && m_gpuRenderer->rayTracing()) {
        ImGui::TextUnformatted("Path tracer:");
        ImGui::SameLine();
        if (ImGui::RadioButton("GPU (ray queries)", m_options.gpu)) {
            m_options.gpu = true;
        }
        ImGui::SameLine();
        if (ImGui::RadioButton("CPU", !m_options.gpu)) {
            m_options.gpu = false;
        }
    } else {
        ImGui::TextDisabled("CPU path tracer (GPU: %s)", m_gpuUnavailable.c_str());
    }
    ImGui::Separator();

    ImGui::Checkbox("Ray tracing", &m_options.rayTracing);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(m_gpuRenderer ? "Off: rasterize on the GPU (direct light, shadow maps; "
                                          "no reflections or bounced light, no rays)"
                                        : "Off: only the 2D shapes");
    }
    ImGui::SameLine();
    ImGui::Checkbox("Live", &m_options.liveRender);
    ImGui::SameLine();
    if (ImGui::Button("Step")) {
        m_stepOnce = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Reset")) {
        m_tracer.resetAccumulation();
        if (m_gpuRenderer) {
            m_gpuRenderer->resetAccumulation();
        }
    }
    if (m_frozen) {
        ImGui::TextColored(ImVec4(1.0F, 0.7F, 0.2F, 1.0F), "Frame frozen (seed fill)");
        ImGui::SameLine();
        if (ImGui::Button("Unfreeze")) {
            m_frozen = false;
        }
    }
    ImGui::Checkbox("Adaptive (preview while moving)",
                    usingGpu() ? &m_options.adaptiveGpu : &m_options.adaptiveCpu);

    auto& settings = m_options.tracer;
    enumCombo("Mode", settings.mode);
    ImGui::Checkbox("Accumulate", &settings.accumulate);
    ImGui::SameLine();
    ImGui::Checkbox("Sample lights", &settings.nextEventEstimation);
    int bounces = static_cast<int>(settings.maxBounces);
    if (ImGui::SliderInt("Bounces", &bounces, 1, 16)) {
        settings.maxBounces = static_cast<std::uint32_t>(bounces);
    }
    if (usingGpu()) {
        ImGui::SliderFloat("GPU ms / frame", &m_options.gpuBudgetMs, 0.0F, 30.0F,
                           m_options.gpuBudgetMs > 0.0F ? "%.1f" : "off");
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Samples per frame follow this GPU time (off: the slider below)");
        }
    }
    if (!usingGpu() || m_options.gpuBudgetMs <= 0.0F) {
        int samples = static_cast<int>(settings.samplesPerPixel);
        if (ImGui::SliderInt("Samples / frame", &samples, 1, 16)) {
            settings.samplesPerPixel = static_cast<std::uint32_t>(samples);
        }
    }
    ImGui::DragFloat("Exposure", &settings.exposure, 0.01F, 0.01F, 16.0F);
    ImGui::DragFloat("Firefly clamp", &settings.fireflyClamp, 0.1F, 0.0F, 100.0F,
                     settings.fireflyClamp > 0.0F ? "%.1f" : "off");
    enumCombo("Tone mapper", settings.toneMapper);
    ImGui::Checkbox("sRGB output", &settings.srgbOutput);
    bool skyChanged = color3Edit("Sky", m_scene.environment().skyColor);
    // A space sky: brightness and orientation apply at once; seed, Milky Way and stars
    // generate it anew (a fraction of a second) once edited.
    SpaceSky& space = m_scene.environment().space;
    skyChanged |= ImGui::Checkbox("Space sky", &space.enabled);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Black space with the Milky Way and stars, generated from a seed "
                          "(the sky color adds to it)");
    }
    if (space.enabled) {
        skyChanged |= ImGui::SliderFloat("Sky brightness", &space.brightness, 0.01F, 16.0F,
                                         "%.2f", ImGuiSliderFlags_Logarithmic);
        skyChanged |= ImGui::DragFloat3("Galaxy orientation", &space.orientation.x, 0.5F,
                                        -360.0F, 360.0F, "%.1f");
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Euler angles (degrees) of the galaxy: unrotated, its center is "
                              "towards +X and its band lies in the XZ plane");
        }
        if (!m_skyEditing) {
            m_skyEdit = space;
        }
        bool active = false;
        bool edited = false;
        auto track = [&] {
            active |= ImGui::IsItemActive();
            edited |= ImGui::IsItemDeactivatedAfterEdit();
        };
        int seed = static_cast<int>(m_skyEdit.seed);
        if (ImGui::InputInt("Sky seed", &seed, 1, 10, ImGuiInputTextFlags_EnterReturnsTrue)) {
            m_skyEdit.seed = static_cast<std::uint32_t>(std::max(seed, 0));
            edited = true;
        }
        ImGui::SliderFloat("Milky Way", &m_skyEdit.milkyWay, 0.0F, 4.0F, "%.2f");
        track();
        ImGui::SliderFloat("Faintest stars", &m_skyEdit.starMagnitude, 2.0F, 10.0F, "mag %.1f");
        track();
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Stars down to this magnitude are points (one more: some 2.8 "
                              "times as many); fainter ones are part of the sky's glow");
        }
        m_skyEditing = active;
        if (edited) {
            space.seed = m_skyEdit.seed;
            space.milkyWay = m_skyEdit.milkyWay;
            space.starMagnitude = m_skyEdit.starMagnitude;
            skyChanged = true;
        }
    }
    if (skyChanged) {
        m_scene.markDirty();
    }

    if (usingGpu() && m_options.rayTracing) {
        ImGui::SeparatorText("GPU pipeline");
        GpuRenderOptions& gpu = m_options.gpuRender;
        // The ray-traced ones (Raster is "Ray tracing" off).
        if (ImGui::BeginCombo("Pipeline", enumToString(gpu.pipeline).data())) {
            for (const GpuPipeline pipeline : {GpuPipeline::Hybrid, GpuPipeline::PathTracing}) {
                if (ImGui::Selectable(enumToString(pipeline).data(), pipeline == gpu.pipeline)) {
                    gpu.pipeline = pipeline;
                }
            }
            ImGui::EndCombo();
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Hybrid: rasterized first hits, samples reused across frames and "
                              "denoised.\nPath tracing: camera rays, restarts whenever anything "
                              "moves (the reference).");
        }
        if (gpu.pipeline == GpuPipeline::Hybrid) {
            enumCombo("Trace resolution", gpu.traceResolution);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Half: paths from one pixel of each 2x2 per frame, in turn (a "
                                  "third less GPU time in the cockpit, more noise while moving; "
                                  "still images converge alike). DLSS-RR traces every pixel.");
            }
            enumCombo("Direct shadows", gpu.directShadows);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Of the sphere lights' direct light.\nShadow maps: as games, "
                                  "looked up every frame (no noise, no lag; soft shadows "
                                  "approximated).\nTraced: shadow rays accumulated over frames "
                                  "(converges to the path tracer's image).");
            }
            ImGui::Checkbox("Radiance cache", &gpu.radianceCache);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Bounced light learnt over the frames in cells of the surfaces: "
                                  "paths end at the first surface the cache knows (less noise, "
                                  "about 2 ms more at 1080p)");
            }
            DenoiserSettings& denoiser = gpu.denoiser;
            ImGui::Checkbox("Reuse samples", &denoiser.temporal);
            ImGui::SameLine();
            ImGui::Checkbox("Denoise", &denoiser.spatial);
            int history = static_cast<int>(denoiser.historyFrames);
            if (ImGui::SliderInt("History (frames)", &history, 1, 256)) {
                denoiser.historyFrames = static_cast<std::uint32_t>(history);
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Frames of samples kept while moving: more is smoother, "
                                  "fewer follows changing light faster");
            }
            int iterations = static_cast<int>(denoiser.iterations);
            if (ImGui::SliderInt("Filter passes", &iterations, 1, 6)) {
                denoiser.iterations = static_cast<std::uint32_t>(iterations);
            }
            ImGui::SliderFloat("Color tolerance", &denoiser.colorSigma, 0.5F, 16.0F, "%.1f");
            enumCombo("Show", gpu.view);
        }
    }

    const bool shadowMaps =
        !m_options.rayTracing || (gpuPipeline() == GpuPipeline::Hybrid &&
                                  m_options.gpuRender.directShadows == DirectShadows::ShadowMaps);
    if (usingGpu() && shadowMaps) {
        ImGui::SeparatorText("Shadow maps");
        ShadowSettings& shadows = m_options.gpuRender.shadows;
        ImGui::Checkbox("Shadows", &shadows.enabled);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("The brightest sphere lights see the scene's depth: what is "
                              "nearer them shades what is behind, softer the bigger the light");
        }
        if (shadows.enabled) {
            ImGui::SliderFloat("Distance", &shadows.distance, 4.0F, 512.0F, "%.0f",
                               ImGuiSliderFlags_Logarithmic);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Far lights cast shadows this far from the camera; nearer "
                                  "lights all around them");
            }
            int resolution = static_cast<int>(shadows.resolution);
            if (ImGui::SliderInt("Resolution", &resolution, 256, 2048)) {
                shadows.resolution = std::bit_ceil(static_cast<std::uint32_t>(resolution));
            }
        }
    }

    if (usingGpu() && gpuPipeline() != GpuPipeline::PathTracing) {
        ImGui::SeparatorText("Upscaling");
        UpscalerSettings& upscaling = m_options.gpuRender.upscaling;
        if (ImGui::BeginCombo("Upscaler", enumToString(upscaling.upscaler).data())) {
            for (const Upscaler upscaler :
                 {Upscaler::Off, Upscaler::Taau, Upscaler::Fsr, Upscaler::Dlss, Upscaler::DlssRr}) {
                const bool available = m_gpuRenderer->upscalerAvailable(upscaler);
                ImGui::BeginDisabled(!available);
                if (ImGui::Selectable(enumToString(upscaler).data(),
                                      upscaler == upscaling.upscaler)) {
                    upscaling.upscaler = upscaler;
                }
                ImGui::EndDisabled();
                if (!available && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                    ImGui::SetTooltip("%s", m_gpuRenderer->upscalerError(upscaler).c_str());
                }
            }
            ImGui::EndCombo();
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Render fewer pixels, each frame at another position in them, and "
                              "reconstruct the view's from the frames.\nTAAU: StarCore's own. FSR: "
                              "AMD's, any GPU. DLSS: NVIDIA's, RTX GPUs. DLSS-RR: DLSS that also "
                              "denoises the hybrid pipeline's paths (DLSS for raster).");
        }
        if (upscaling.upscaler != Upscaler::Off) {
            enumCombo("Quality", upscaling.quality);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Rendered: %.0f%% of each side",
                                  100.0F * renderScale(upscaling.quality));
            }
            if (m_gpuRenderer->activeUpscaler() == Upscaler::Fsr) {
                ImGui::SliderFloat("Sharpness", &upscaling.sharpness, 0.0F, 1.0F, "%.2f");
            }
            ImGui::Text("%s: %ux%u -> %ux%u",
                        enumToString(m_gpuRenderer->activeUpscaler()).data(),
                        m_gpuRenderer->renderWidth(), m_gpuRenderer->renderHeight(),
                        m_gpuRenderer->width(), m_gpuRenderer->height());
        }
    }

    if (usingGpu()) {
        ImGui::SeparatorText("Effects");
        EffectSettings& effects = m_options.gpuRender.effects;
        ImGui::Checkbox("Bloom", &effects.bloom);
        if (effects.bloom) {
            ImGui::SameLine();
            ImGui::SetNextItemWidth(-1.0F);
            ImGui::SliderFloat("##Bloom strength", &effects.bloomStrength, 0.0F, 0.3F, "%.3f");
        }
        ImGui::Checkbox("Motion blur", &effects.motionBlur);
        if (effects.motionBlur) {
            ImGui::SameLine();
            ImGui::SetNextItemWidth(-1.0F);
            ImGui::SliderFloat("##Shutter", &effects.shutter, 0.0F, 1.0F, "shutter %.2f");
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Fraction of the frame the shutter is open (Hybrid and Raster)");
        }
        ImGui::SliderFloat("Vignette", &effects.vignette, 0.0F, 1.0F, "%.2f");
        ImGui::Checkbox("Particles", &m_options.particles);
        ImGui::SameLine();
        ImGui::Checkbox("Space dust", &m_options.spaceDust);
    }

    ImGui::Separator();
    ImGui::Text("Frame: %.2f ms (%.0f FPS)", 1000.0F * Application::get().deltaTime(),
                ImGui::GetIO().Framerate);
    if (usingGpu()) {
        ImGui::Text("GPU: %.2f ms, %.2f of it tracing (%.2f ms to submit)",
                    m_gpuRenderer->gpuTimeMs(), m_gpuRenderer->traceTimeMs(), m_traceMs);
        ImGui::Text("Accumulated frames: %u (%u samples / frame)",
                    m_gpuRenderer->accumulatedFrames(),
                    m_options.gpuBudgetMs > 0.0F
                        ? m_gpuRenderer->samplesForBudget(m_options.gpuBudgetMs)
                        : settings.samplesPerPixel);
    } else {
        ImGui::Text("Trace: %.2f ms on %u threads", m_traceMs, m_tracer.threadCount());
        ImGui::Text("Accumulated frames: %u", m_tracer.accumulatedFrames());
    }
    const RenderScene& renderScene = m_renderScene.renderScene();
    ImGui::Text("Instances: %zu, lights: %zu, triangles: %zu", renderScene.instances.size(),
                renderScene.lights.size(), renderScene.triangleCount());
    if (usingGpu()) {
        ImGui::Text("Instances uploaded last frame: %u, particles: %zu",
                    m_gpuRenderer->uploadedInstances(), m_frameParticles.size());
    }
    ImGui::End();
}

void GameLayer::drawStatusPanel()
{
    ImGui::Begin("Status");
    ImGui::Text("Mouse: (%.0f, %.0f)%s", m_viewport.mouse.x, m_viewport.mouse.y,
                m_viewport.hovered ? "" : " (outside)");
    const Vec3 mouseWorld = m_camera.screenToWorld(m_viewport.mouse, m_drawDepth);
    ImGui::Text("Mouse world (depth %.1f): (%.2f, %.2f, %.2f)", m_drawDepth,
                mouseWorld.x, mouseWorld.y, mouseWorld.z);
    ImGui::Separator();
    const Vec3 position = m_camera.position();
    const Vec3 forward = m_camera.forward();
    ImGui::Text("Camera: (%.2f, %.2f, %.2f)", position.x, position.y, position.z);
    ImGui::Text("Forward: (%.2f, %.2f, %.2f)", forward.x, forward.y, forward.z);
    if (m_viewMode == ViewMode::Pilot && m_ship.isValid()) {
        const auto& ship = m_ship.get<ShipComponent>();
        ImGui::Text("Ship speed: %.2f", glm::length(ship.velocity));
    } else {
        ImGui::Text("Camera speed: %.2f", m_cameraController.speed());
    }
    ImGui::Separator();
    const Entity hovered = m_hovered ? m_scene.wrap(*m_hovered) : Entity{};
    ImGui::Text("Under mouse: %s", hovered ? hovered.name().c_str() : "None");
    const Entity selected = m_selection.selected();
    ImGui::Text("Selected: %s", selected ? selected.name().c_str() : "None");
    ImGui::Text("Tool: %s", toolName(m_toolKind).data());
    ImGui::Separator();
    ImGui::TextWrapped("%s", m_status.c_str());
    ImGui::End();
}

void GameLayer::drawHierarchyNode(Entity entity)
{
    const auto children = m_scene.childrenOf(entity);
    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow |
                               ImGuiTreeNodeFlags_SpanAvailWidth |
                               ImGuiTreeNodeFlags_DefaultOpen;
    if (children.empty()) {
        flags |= ImGuiTreeNodeFlags_Leaf;
    }
    if (entity == m_selection.selected()) {
        flags |= ImGuiTreeNodeFlags_Selected;
    }
    const auto id = static_cast<std::uint64_t>(entt::to_integral(entity.handle()));
    const bool open = ImGui::TreeNodeEx(reinterpret_cast<void*>(static_cast<std::uintptr_t>(id)),
                                        flags, "%s", entity.name().c_str());
    if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) {
        setTool(ToolKind::Select);
        m_selection.select(entity);
    }

    // Drag an entity onto another to make it a child.
    if (ImGui::BeginDragDropSource()) {
        const entt::entity handle = entity.handle();
        ImGui::SetDragDropPayload(kEntityPayload, &handle, sizeof(handle));
        ImGui::Text("%s", entity.name().c_str());
        ImGui::EndDragDropSource();
    }
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kEntityPayload)) {
            const auto child = *static_cast<const entt::entity*>(payload->Data);
            if (!m_scene.setParent(m_scene.wrap(child), entity)) {
                m_status = "Cannot parent an entity to its own descendant";
            }
        }
        ImGui::EndDragDropTarget();
    }

    bool destroy = false;
    if (ImGui::BeginPopupContextItem()) {
        if (ImGui::MenuItem("Duplicate")) {
            m_selection.select(m_scene.copyEntity(entity));
        }
        if (ImGui::MenuItem("Detach from parent", nullptr, false,
                            static_cast<bool>(m_scene.parentOf(entity)))) {
            m_scene.setParent(entity, Entity{});
        }
        if (ImGui::MenuItem("Delete")) {
            destroy = true;
        }
        ImGui::EndPopup();
    }

    if (open) {
        // Copy: children may be re-parented by drag & drop while iterating.
        const std::vector<entt::entity> childList(children.begin(), children.end());
        for (const auto child : childList) {
            if (Entity childEntity = m_scene.wrap(child)) {
                drawHierarchyNode(childEntity);
            }
        }
        ImGui::TreePop();
    }
    if (destroy) {
        if (m_selection.selected() == entity) {
            m_selection.clear();
        }
        m_scene.destroyEntity(entity);
    }
}

void GameLayer::drawHierarchyPanel()
{
    ImGui::Begin("Hierarchy");
    std::vector<entt::entity> roots;
    for (const auto entity : m_scene.registry().view<IdComponent>()) {
        const auto* hierarchy = m_scene.registry().try_get<HierarchyComponent>(entity);
        if (hierarchy == nullptr || hierarchy->parent == entt::null) {
            roots.push_back(entity);
        }
    }
    std::ranges::reverse(roots);  // Creation order.
    for (const auto root : roots) {
        if (Entity entity = m_scene.wrap(root)) {
            drawHierarchyNode(entity);
        }
    }

    // Dropping on the empty area detaches the entity.
    ImGui::Dummy(ImGui::GetContentRegionAvail());
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kEntityPayload)) {
            m_scene.setParent(m_scene.wrap(*static_cast<const entt::entity*>(payload->Data)),
                              Entity{});
        }
        ImGui::EndDragDropTarget();
    }
    ImGui::End();
}

void GameLayer::drawInspectorPanel()
{
    ImGui::Begin("Inspector");
    Entity entity = m_selection.selected();
    if (!entity || !entity.isValid()) {
        ImGui::TextDisabled("Nothing selected");
        ImGui::End();
        return;
    }
    bool changed = false;

    ImGui::InputText("Name", &entity.get<NameComponent>().name);
    ImGui::TextDisabled("Id: %llu", static_cast<unsigned long long>(entity.uuid().value()));
    if (Entity parent = m_scene.parentOf(entity)) {
        ImGui::TextDisabled("Parent: %s", parent.name().c_str());
    }

    if (componentHeader("Transform", false).first) {
        auto& transform = entity.get<TransformComponent>();
        changed |= ImGui::DragFloat3("Translation", &transform.translation.x, 0.01F);
        if (m_eulerEntity != entity.handle() || m_eulerSource != transform.rotation) {
            m_eulerEntity = entity.handle();
            m_eulerDegrees = glm::degrees(glm::eulerAngles(transform.rotation));
            m_eulerSource = transform.rotation;
        }
        if (ImGui::DragFloat3("Rotation", &m_eulerDegrees.x, 0.5F)) {
            transform.rotation = Quat(glm::radians(m_eulerDegrees));
            m_eulerSource = transform.rotation;
            changed = true;
        }
        changed |= ImGui::DragFloat3("Scale", &transform.scale.x, 0.01F);
    }

    {
        const std::uint32_t current = m_scene.layerOf(entity.handle());
        const auto& layers = m_scene.layers().layers;
        if (ImGui::BeginCombo("Layer", layers[current].name.c_str())) {
            for (std::uint32_t i = 0; i < kLayerCount; ++i) {
                ImGui::PushID(static_cast<int>(i));
                if (ImGui::Selectable(layers[i].name.c_str(), i == current)) {
                    m_scene.setLayer(entity, i, m_layerRecursive);
                }
                ImGui::PopID();
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        ImGui::Checkbox("+children", &m_layerRecursive);
    }

    if (auto* material = entity.tryGet<MaterialComponent>()) {
        const auto [open, remove] = componentHeader("Material", true);
        if (open) {
            changed |= color3Edit("Albedo", material->albedo);
            changed |= ImGui::SliderFloat("Roughness", &material->roughness, 0.0F, 1.0F);
            changed |= ImGui::SliderFloat("Metallic", &material->metallic, 0.0F, 1.0F);
            changed |= color3Edit("Emission", material->emissionColor);
            changed |= ImGui::DragFloat("Emission power", &material->emissionPower, 0.05F,
                                        0.0F, 1000.0F);
            changed |= ImGui::SliderFloat("Transmission", &material->transmission, 0.0F, 1.0F);
            if (material->transmission > 0.0F) {
                changed |= ImGui::DragFloat("IOR", &material->ior, 0.005F, 1.0F, 3.0F);
                changed |= ImGui::Checkbox("Thin walled (window pane)", &material->thinWalled);
            }
        }
        if (remove) {
            entity.remove<MaterialComponent>();
        }
    }
    if (auto* sphere = entity.tryGet<SphereComponent>()) {
        const auto [open, remove] = componentHeader("Sphere", true);
        if (open) {
            changed |= ImGui::DragFloat("Radius", &sphere->radius, 0.01F, 0.001F, 10000.0F);
        }
        if (remove) {
            entity.remove<SphereComponent>();
        }
    }
    if (auto* mesh = entity.tryGet<MeshComponent>()) {
        const auto [open, remove] = componentHeader("Mesh", true);
        if (open && mesh->mesh) {
            ImGui::Text("%zu vertices, %zu triangles%s", mesh->mesh->positions.size(),
                        mesh->mesh->triangleCount(),
                        mesh->mesh->normals.empty() ? " (flat)" : "");
            if (!mesh->source.empty()) {
                ImGui::TextWrapped("From %s (mesh %u)", mesh->source.path.c_str(),
                                   mesh->source.meshIndex);
            }
        }
        if (remove) {
            entity.remove<MeshComponent>();
        }
    }
    if (auto* collider = entity.tryGet<ColliderComponent>()) {
        const auto [open, remove] = componentHeader("Collider", true);
        if (open) {
            changed |= enumCombo("Shape", collider->shape);
            if (collider->shape == ColliderShape::Sphere) {
                changed |= ImGui::DragFloat("Radius##collider", &collider->radius, 0.01F, 0.001F,
                                            10000.0F);
            } else if (collider->shape == ColliderShape::Box) {
                changed |= ImGui::DragFloat3("Half extents", &collider->halfExtents.x, 0.01F,
                                             0.001F, 10000.0F);
            } else if (!entity.has<MeshComponent>()) {
                ImGui::TextColored(ImVec4(1.0F, 0.7F, 0.2F, 1.0F), "Needs a Mesh component");
            }
            changed |= ImGui::Checkbox("Trigger (no blocking)", &collider->trigger);
        }
        if (remove) {
            entity.remove<ColliderComponent>();
        }
    }
    if (auto* stroke = entity.tryGet<StrokeComponent>()) {
        const auto [open, remove] = componentHeader("Stroke", true);
        if (open) {
            changed |= color4Edit("Color", stroke->color);
            int thickness = static_cast<int>(stroke->thickness);
            if (ImGui::SliderInt("Thickness", &thickness, 1, 25)) {
                stroke->thickness = static_cast<std::uint32_t>(thickness);
                changed = true;
            }
            changed |= enumCombo("Pattern", stroke->pattern);
        }
        if (remove) {
            entity.remove<StrokeComponent>();
        }
    }
    if (auto* line = entity.tryGet<LineShapeComponent>()) {
        const auto [open, remove] = componentHeader("Line Segment", true);
        if (open) {
            changed |= ImGui::DragFloat3("Start", &line->start.x, 0.01F);
            changed |= ImGui::DragFloat3("End", &line->end.x, 0.01F);
            changed |= enumCombo("Algorithm##line", line->algorithm);
        }
        if (remove) {
            entity.remove<LineShapeComponent>();
        }
    }
    if (auto* ellipse = entity.tryGet<EllipseShapeComponent>()) {
        const auto [open, remove] = componentHeader("Ellipse", true);
        if (open) {
            changed |= ImGui::DragFloat("Radius X", &ellipse->radiusX, 0.01F, 0.0F, 1000.0F);
            changed |= ImGui::DragFloat("Radius Y", &ellipse->radiusY, 0.01F, 0.0F, 1000.0F);
            changed |= enumCombo("Algorithm##ellipse", ellipse->algorithm);
        }
        if (remove) {
            entity.remove<EllipseShapeComponent>();
        }
    }
    if (auto* arc = entity.tryGet<ArcShapeComponent>()) {
        const auto [open, remove] = componentHeader("Arc", true);
        if (open) {
            changed |= ImGui::DragFloat("Radius##arc", &arc->radius, 0.01F, 0.0F, 1000.0F);
            float start = glm::degrees(arc->startAngle);
            float sweep = glm::degrees(arc->sweepAngle);
            if (ImGui::DragFloat("Start angle", &start, 0.5F)) {
                arc->startAngle = glm::radians(start);
                changed = true;
            }
            if (ImGui::DragFloat("Sweep angle", &sweep, 0.5F, -360.0F, 360.0F)) {
                arc->sweepAngle = glm::radians(sweep);
                changed = true;
            }
            if (ImGui::Button("Swap minor/major")) {
                // Same end points, the other way around the circle.
                arc->sweepAngle = arc->sweepAngle > 0.0F ? arc->sweepAngle - kTwoPi
                                                         : arc->sweepAngle + kTwoPi;
                changed = true;
            }
        }
        if (remove) {
            entity.remove<ArcShapeComponent>();
        }
    }
    if (auto* curve = entity.tryGet<CurveShapeComponent>()) {
        const auto [open, remove] = componentHeader("Curve", true);
        if (open) {
            changed |= enumCombo("Type", curve->type);
            if (curve->type == CurveType::BSpline) {
                int order = static_cast<int>(curve->order);
                if (ImGui::SliderInt("Order", &order, 2, 8)) {
                    curve->order = static_cast<std::uint32_t>(order);
                    changed = true;
                }
            }
            ImGui::Text("%zu control points", curve->controlPoints.size());
        }
        if (remove) {
            entity.remove<CurveShapeComponent>();
        }
    }
    if (auto* polygon = entity.tryGet<PolygonShapeComponent>()) {
        const auto [open, remove] = componentHeader("Polygon", true);
        if (open) {
            changed |= ImGui::Checkbox("Filled", &polygon->filled);
            changed |= color4Edit("Fill", polygon->fillColor);
            ImGui::Text("%zu vertices", polygon->vertices.size());
        }
        if (remove) {
            entity.remove<PolygonShapeComponent>();
        }
    }
    if (auto* portal = entity.tryGet<PortalComponent>()) {
        const auto [open, remove] = componentHeader("Portal", true);
        if (open) {
            changed |= ImGui::DragFloat2("Size", &portal->size.x, 0.01F, 0.1F, 100.0F);
            const Entity linked = m_scene.findEntity(portal->linkedPortal);
            ImGui::Text("Linked to: %s", linked ? linked.name().c_str() : "(none)");
        }
        if (remove) {
            entity.remove<PortalComponent>();
        }
    }
    if (auto* emitter = entity.tryGet<ParticleEmitterComponent>()) {
        const auto [open, remove] = componentHeader("Particle emitter", true);
        if (open) {
            // Particles are not traced: edits need not restart the accumulation.
            ParticleSettings& particles = emitter->particles;
            ImGui::Checkbox("Enabled", &emitter->enabled);
            ImGui::SameLine();
            ImGui::Checkbox("Inherit velocity", &emitter->inheritVelocity);
            ImGui::DragFloat("Rate (/s)", &emitter->rate, 1.0F, 0.0F, 10000.0F);
            ImGui::DragFloat2("Lifetime (s)", &particles.lifetime.x, 0.01F, 0.01F, 60.0F);
            ImGui::DragFloat2("Speed", &particles.speed.x, 0.05F, 0.0F, 1000.0F);
            ImGui::SliderFloat("Spread", &particles.spread, 0.0F, kPi);
            ImGui::DragFloat2("Size (start, end)", &particles.size.x, 0.001F, 0.0F, 100.0F);
            // Emitted radiance: may exceed 1 (glows, blooms).
            constexpr ImGuiColorEditFlags kHdr =
                ImGuiColorEditFlags_HDR | ImGuiColorEditFlags_Float;
            ImGui::ColorEdit3("Start color", &particles.startColor.x, kHdr);
            ImGui::ColorEdit3("End color", &particles.endColor.x, kHdr);
            ImGui::DragFloat("Drag", &particles.drag, 0.01F, 0.0F, 100.0F);
            ImGui::DragFloat3("Acceleration", &particles.acceleration.x, 0.05F);
        }
        if (remove) {
            entity.remove<ParticleEmitterComponent>();
        }
    }

    ImGui::Separator();
    if (ImGui::Button("Add Component")) {
        ImGui::OpenPopup("AddComponent");
    }
    if (ImGui::BeginPopup("AddComponent")) {
        auto addItem = [&]<typename T>(const char* label, T value) {
            if (!entity.has<T>() && ImGui::MenuItem(label)) {
                entity.add<T>(std::move(value));
            }
        };
        addItem("Material", MaterialComponent{});
        // A collider matching the visible shape.
        if (const auto* sphere = entity.tryGet<SphereComponent>()) {
            addItem("Collider", ColliderComponent{.radius = sphere->radius});
        } else if (entity.has<MeshComponent>()) {
            addItem("Collider", ColliderComponent{.shape = ColliderShape::Mesh});
        } else {
            addItem("Collider", ColliderComponent{.shape = ColliderShape::Box});
        }
        addItem("Sphere", SphereComponent{});
        addItem("Mesh (cube)", cubeMesh());
        addItem("Stroke", StrokeComponent{});
        addItem("Line Segment", LineShapeComponent{});
        addItem("Ellipse", EllipseShapeComponent{});
        addItem("Arc", ArcShapeComponent{});
        addItem("Polygon", PolygonShapeComponent{
                               .vertices = {{-0.5F, -0.5F, 0}, {0.5F, -0.5F, 0}, {0, 0.5F, 0}}});
        addItem("Point", PointShapeComponent{});
        addItem("Particle emitter", ParticleEmitterComponent{});
        ImGui::EndPopup();
    }

    if (changed) {
        m_scene.markDirty();
    }
    ImGui::End();
}

void GameLayer::drawLayersPanel()
{
    ImGui::Begin("Layers");
    SceneLayers& layers = m_scene.layers();
    constexpr ImGuiTableFlags kTableFlags =
        ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV;
    if (ImGui::BeginTable("layers", 4, kTableFlags)) {
        ImGui::TableSetupColumn("Active");
        ImGui::TableSetupColumn("Show");
        ImGui::TableSetupColumn("Lock");
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        for (std::uint32_t i = 0; i < kLayerCount; ++i) {
            LayerSettings& layer = layers.layers[i];
            ImGui::PushID(static_cast<int>(i));
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            if (ImGui::RadioButton("##active", m_activeLayer == i)) {
                m_activeLayer = i;
            }
            ImGui::TableNextColumn();
            ImGui::Checkbox("##visible", &layer.visible);
            ImGui::TableNextColumn();
            bool locked = !layer.pickable;
            if (ImGui::Checkbox("##locked", &locked)) {
                layer.pickable = !locked;
            }
            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-FLT_MIN);
            ImGui::InputText("##name", &layer.name);
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::TextDisabled("New drawings go to the active layer. Locked layers cannot be "
                        "selected in the viewport.");

    if (ImGui::CollapsingHeader("Collision matrix")) {
        // Lower triangle: row i, column j <= i (the matrix is symmetric).
        if (ImGui::BeginTable("collisions", static_cast<int>(kLayerCount) + 1,
                              ImGuiTableFlags_SizingFixedFit)) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            for (std::uint32_t j = 0; j < kLayerCount; ++j) {
                ImGui::TableNextColumn();
                ImGui::Text("%u", j);
            }
            for (std::uint32_t i = 0; i < kLayerCount; ++i) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::Text("%u %s", i, layers.layers[i].name.c_str());
                for (std::uint32_t j = 0; j <= i; ++j) {
                    ImGui::TableNextColumn();
                    ImGui::PushID(static_cast<int>(i * kLayerCount + j));
                    bool collide = layers.collides(i, j);
                    if (ImGui::Checkbox("##c", &collide)) {
                        layers.setCollides(i, j, collide);
                    }
                    ImGui::PopID();
                }
            }
            ImGui::EndTable();
        }
    }
    ImGui::End();
}

void GameLayer::drawFileDialog()
{
    if (m_fileDialog == FileDialog::None) {
        return;
    }
    const char* title = m_fileDialog == FileDialog::Open ? "Open Scene" : "Save Scene As";
    if (!ImGui::IsPopupOpen(title)) {
        m_fileDialogPath = m_scenePath.empty() ? "assets/scenes/untitled.yaml"
                                               : m_scenePath.string();
        ImGui::OpenPopup(title);
    }
    if (ImGui::BeginPopupModal(title, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::SetNextItemWidth(480.0F);
        const bool submitted = ImGui::InputText("##path", &m_fileDialogPath,
                                                ImGuiInputTextFlags_EnterReturnsTrue);
        if (ImGui::Button("OK") || submitted) {
            if (m_fileDialog == FileDialog::Open) {
                openScene(m_fileDialogPath);
            } else {
                saveScene(m_fileDialogPath);
            }
            m_fileDialog = FileDialog::None;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
            m_fileDialog = FileDialog::None;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

auto GameLayer::createAtCursor(const std::string& name) -> Entity
{
    Entity entity = m_scene.createEntity(name);
    entity.get<TransformComponent>().translation =
        m_camera.position() + m_camera.forward() * m_drawDepth;
    m_scene.setLayer(entity, m_activeLayer);
    setTool(ToolKind::Select);
    m_selection.select(entity);
    return entity;
}

void GameLayer::drawMenuBar()
{
    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("New", "Ctrl+N")) {
            newScene();
        }
        if (ImGui::MenuItem("Open...", "Ctrl+O")) {
            m_fileDialog = FileDialog::Open;
        }
        if (ImGui::MenuItem("Save", "Ctrl+S")) {
            if (m_scenePath.empty()) {
                m_fileDialog = FileDialog::SaveAs;
            } else {
                saveScene(m_scenePath);
            }
        }
        if (ImGui::MenuItem("Save As...")) {
            m_fileDialog = FileDialog::SaveAs;
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Exit")) {
            Application::get().close();
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Create")) {
        if (ImGui::MenuItem("Empty")) {
            createAtCursor("Empty");
        }
        if (ImGui::MenuItem("Sphere")) {
            Entity e = createAtCursor("Sphere");
            e.add<SphereComponent>();
            e.add<MaterialComponent>();
        }
        if (ImGui::MenuItem("Light")) {
            Entity e = createAtCursor("Light");
            e.add<SphereComponent>(0.5F);
            e.add<MaterialComponent>(MaterialComponent{
                .albedo = Vec3(1.0F), .emissionColor = Vec3(1.0F), .emissionPower = 10.0F});
        }
        if (ImGui::MenuItem("Cube")) {
            Entity e = createAtCursor("Cube");
            e.add<MeshComponent>(cubeMesh());
            e.add<MaterialComponent>();
        }
        if (ImGui::MenuItem("Plane")) {
            Entity e = createAtCursor("Plane");
            e.add<MeshComponent>(planeMesh(4.0F));
            e.add<MaterialComponent>();
        }
        if (ImGui::MenuItem("Portal", "T")) {
            m_portals.createPortal(m_scene, m_camera, kPortalLayer);
        }
        if (ImGui::MenuItem("Particle emitter")) {
            // A fountain of sparks: up (local -Z turned up), falling back.
            Entity e = createAtCursor("Particle emitter");
            e.patch<TransformComponent>([](TransformComponent& t) {
                t.rotation = glm::angleAxis(kPi / 2.0F, Vec3{1.0F, 0.0F, 0.0F});
            });
            e.add<ParticleEmitterComponent>(ParticleEmitterComponent{
                .particles = {.lifetime = {1.0F, 2.0F},
                              .speed = {3.0F, 5.0F},
                              .spread = 0.3F,
                              .size = {0.04F, 0.0F},
                              .startColor = {6.0F, 3.0F, 1.0F},
                              .endColor = {0.6F, 0.1F, 0.0F},
                              .acceleration = {0.0F, -4.0F, 0.0F}},
                .rate = 120.0F,
                .inheritVelocity = false,
            });
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("View")) {
        if (ImGui::MenuItem("Reset free camera")) {
            resetCamera();
            setViewMode(ViewMode::Free);
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Pilot view", "C", m_viewMode == ViewMode::Pilot,
                            m_ship.isValid())) {
            setViewMode(ViewMode::Pilot);
        }
        if (ImGui::MenuItem("Free camera", "C", m_viewMode == ViewMode::Free)) {
            setViewMode(ViewMode::Free);
        }
        ImGui::MenuItem("Show cockpit", nullptr, &m_scene.layers().layers[kCockpitLayer].visible);
        if (ImGui::MenuItem("Respawn ship")) {
            spawnShip();
        }
        ImGui::EndMenu();
    }
}

}  // namespace startraveler
