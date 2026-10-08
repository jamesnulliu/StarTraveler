#include <algorithm>
#include <chrono>
#include <format>
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
#include <starcore/platform/input.hpp>
#include <starcore/raster/fill.hpp>
#include <starcore/render/overlay.hpp>
#include <starcore/scene/serializer.hpp>

#include "game_layer.hpp"
#include "gameplay/hud.hpp"

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

}  // namespace

GameLayer::GameLayer(std::filesystem::path scenePath, AutomationOptions automation)
    : m_scenePath(std::move(scenePath)), m_automation(std::move(automation))
{
}

GameLayer::~GameLayer() = default;

void GameLayer::onAttach()
{
    m_texture = std::make_unique<vulkan::Texture2D>(Application::get().vulkan());
    if (!m_scenePath.empty()) {
        openScene(m_scenePath);
    }
    resetCamera();
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
    m_selection.clear();
    setTool(ToolKind::None);
    m_portals.reset();
    m_renderSceneSourceVersion = ~0ULL;
    m_eulerEntity = entt::null;
    m_frozen = false;
}

void GameLayer::resetCamera()
{
    m_camera.setPosition({0.0F, 0.0F, 10.0F});
    m_camera.setRotation(-90.0F, 0.0F);
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
    if (kind != ToolKind::None) {
        m_selection.clear();
    }
}

void GameLayer::handleShortcuts()
{
    const ImGuiIO& io = ImGui::GetIO();
    if (io.WantTextInput) {
        return;
    }
    if (io.KeyCtrl && input::isKeyPressed(ImGuiKey_N)) {
        newScene();
    } else if (io.KeyCtrl && input::isKeyPressed(ImGuiKey_O)) {
        m_fileDialog = FileDialog::Open;
    } else if (io.KeyCtrl && input::isKeyPressed(ImGuiKey_S)) {
        if (m_scenePath.empty()) {
            m_fileDialog = FileDialog::SaveAs;
        } else {
            saveScene(m_scenePath);
        }
    } else if (io.KeyCtrl && input::isKeyPressed(ImGuiKey_D) && m_selection.selected()) {
        Entity copy = m_scene.copyEntity(m_selection.selected());
        m_selection.select(copy);
    }

    if (!m_viewport.hovered || m_cameraActive) {
        return;
    }
    // Legacy: T opens a portal in front of the camera.
    if (!io.KeyCtrl && input::isKeyPressed(ImGuiKey_T)) {
        Entity portal = m_portals.createPortal(m_scene, m_camera);
        m_status = std::format("Created {}", portal.name());
    }
    if (m_toolKind != ToolKind::None && input::isKeyPressed(ImGuiKey_Escape)) {
        setTool(ToolKind::None);  // Cancel drawing.
    }
}

void GameLayer::updateCamera()
{
    // Look mode starts with a right click on the viewport and lasts while
    // the button is held; the cursor is captured meanwhile.
    if (!m_cameraActive && m_viewport.hovered &&
        input::isMouseClicked(ImGuiMouseButton_Right)) {
        m_cameraActive = true;
    }
    if (m_cameraActive && !input::isMouseDown(ImGuiMouseButton_Right)) {
        m_cameraActive = false;
    }
    input::setCursorMode(m_cameraActive ? input::CursorMode::Locked
                                        : input::CursorMode::Normal);

    const CameraInput cameraInput{
        .forward = input::isKeyDown(ImGuiKey_W),
        .backward = input::isKeyDown(ImGuiKey_S),
        .left = input::isKeyDown(ImGuiKey_A),
        .right = input::isKeyDown(ImGuiKey_D),
        .up = input::isKeyDown(ImGuiKey_E),
        .down = input::isKeyDown(ImGuiKey_Q),
        .look = m_cameraActive,
        .mouseDelta = m_cameraActive ? input::mouseDelta() : Vec2(0.0F),
    };
    m_cameraController.update(m_camera, cameraInput, Application::get().deltaTime());
    if (m_portals.update(m_scene, m_camera)) {
        m_status = "Teleported!";
    }
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
    m_frame.resize(size.x, size.y, 0U);
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
            m_status = std::format("Created {}", result.created.name());
            // Stay in the tool to draw more shapes of the same kind.
        }
        return;
    }

    m_selection.update(context, m_hovered);
}

void GameLayer::composeFrame()
{
    if (m_frozen) {
        return;
    }
    if (m_scene.version() != m_renderSceneSourceVersion) {
        m_renderScene = buildRenderScene(m_scene);
        // Only geometry/material changes restart the accumulation.
        m_renderScene.sceneVersion = hashRenderScene(m_renderScene);
        m_renderSceneSourceVersion = m_scene.version();
    }

    if (m_options.rayTracing) {
        if (m_options.liveRender || m_stepOnce) {
            const auto start = std::chrono::steady_clock::now();
            m_tracer.render(m_renderScene, m_camera, m_options.tracer);
            m_traceMs = std::chrono::duration<float, std::milli>(
                            std::chrono::steady_clock::now() - start)
                            .count();
            m_stepOnce = false;
        }
        std::ranges::copy(m_tracer.image().pixels(), m_frame.pixels().begin());
    } else {
        const Vec3 sky = linearToSrgb(glm::clamp(m_scene.environment().skyColor, 0.0F, 1.0F));
        m_frame.fill(packRgba8(Vec4(sky, 1.0F)));
    }

    m_ids.fill(kNoEntityId);
    renderOverlay(m_scene, m_camera, m_frame, &m_ids);
    if (m_tool) {
        renderOverlay(m_tool->previewScene(), m_camera, m_frame);
    }
    m_selection.drawHandles(makeContext(), m_frame);
    if (m_options.showHud) {
        drawCockpitHud(m_frame);
    }
    m_texture->setPixels(m_frame.pixels(), m_frame.width(), m_frame.height());
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
    ImGui::DockBuilderDockWindow("Inspector", right);
    ImGui::DockBuilderDockWindow("Render", rightBottom);
    ImGui::DockBuilderDockWindow("Status", rightBottom);
    ImGui::DockBuilderFinish(dockspace);
}

void GameLayer::onUIRender()
{
    buildDefaultLayoutIfNeeded();
    handleShortcuts();
    updateCamera();

    drawViewport();
    drawToolsPanel();
    drawRenderPanel();
    drawStatusPanel();
    drawHierarchyPanel();
    drawInspectorPanel();
    drawFileDialog();

    ++m_frameCount;
    if (m_automation.exitAfterFrames != 0 &&
        m_frameCount >= m_automation.exitAfterFrames) {
        if (!m_automation.screenshotPath.empty()) {
            if (auto written = writePng(m_frame, m_automation.screenshotPath); !written) {
                SC_ERROR("{}", written.error());
            } else {
                SC_INFO("Screenshot written to {}", m_automation.screenshotPath.string());
            }
        }
        SC_INFO("Exiting after {} frames ({} accumulated)", m_frameCount,
                m_tracer.accumulatedFrames());
        Application::get().close();
    }
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
    m_viewport.cameraActive = m_cameraActive;

    handleViewportInput();
    composeFrame();

    if (m_texture->imguiTexture() != 0) {
        ImGui::Image(m_texture->imguiTexture(),
                     ImVec2(static_cast<float>(m_viewport.size.x),
                            static_cast<float>(m_viewport.size.y)));
    }

    // Entity under the mouse, for the status bar and the next click.
    m_hovered.reset();
    if (m_viewport.hovered) {
        m_hovered = pickEntity(m_renderScene, m_camera, &m_ids, m_viewport.mouse);
        if (m_hovered && !m_scene.registry().valid(*m_hovered)) {
            m_hovered.reset();
        }
    }

    ImGui::End();
    ImGui::PopStyleVar();
}

void GameLayer::drawToolsPanel()
{
    ImGui::Begin("Tools");
    constexpr ToolKind kTools[] = {ToolKind::None,    ToolKind::Line,  ToolKind::Ellipse,
                                   ToolKind::Arc,     ToolKind::Polygon, ToolKind::Curve,
                                   ToolKind::Group,   ToolKind::SeedFill};
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
    } else {
        ImGui::TextWrapped("Click to select, drag to move, drag green handles to edit. "
                           "Ctrl+click: add child. Del: delete. Ctrl+D: duplicate. "
                           "T: portal. Hold RMB + WASD/QE: fly.");
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
    ImGui::Text("Ray query: %s (GPU renderer planned)",
                device.rayQuerySupported ? "supported" : "not supported");
    ImGui::Separator();

    ImGui::Checkbox("Ray tracing", &m_options.rayTracing);
    ImGui::SameLine();
    ImGui::Checkbox("Live", &m_options.liveRender);
    ImGui::SameLine();
    if (ImGui::Button("Step")) {
        m_stepOnce = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Reset")) {
        m_tracer.resetAccumulation();
    }
    if (m_frozen) {
        ImGui::TextColored(ImVec4(1.0F, 0.7F, 0.2F, 1.0F), "Frame frozen (seed fill)");
        ImGui::SameLine();
        if (ImGui::Button("Unfreeze")) {
            m_frozen = false;
        }
    }
    ImGui::Checkbox("Cockpit HUD", &m_options.showHud);

    auto& settings = m_options.tracer;
    enumCombo("Mode", settings.mode);
    ImGui::Checkbox("Accumulate", &settings.accumulate);
    ImGui::SameLine();
    ImGui::Checkbox("Sample lights", &settings.nextEventEstimation);
    int bounces = static_cast<int>(settings.maxBounces);
    if (ImGui::SliderInt("Bounces", &bounces, 1, 16)) {
        settings.maxBounces = static_cast<std::uint32_t>(bounces);
    }
    int samples = static_cast<int>(settings.samplesPerPixel);
    if (ImGui::SliderInt("Samples / frame", &samples, 1, 16)) {
        settings.samplesPerPixel = static_cast<std::uint32_t>(samples);
    }
    ImGui::DragFloat("Exposure", &settings.exposure, 0.01F, 0.01F, 16.0F);
    ImGui::DragFloat("Firefly clamp", &settings.fireflyClamp, 0.1F, 0.0F, 100.0F,
                     settings.fireflyClamp > 0.0F ? "%.1f" : "off");
    enumCombo("Tone mapper", settings.toneMapper);
    ImGui::Checkbox("sRGB output", &settings.srgbOutput);
    if (color3Edit("Sky", m_scene.environment().skyColor)) {
        m_scene.markDirty();
    }

    ImGui::Separator();
    ImGui::Text("Frame: %.2f ms (%.0f FPS)", 1000.0F * Application::get().deltaTime(),
                ImGui::GetIO().Framerate);
    ImGui::Text("Trace: %.2f ms on %u threads", m_traceMs, m_tracer.threadCount());
    ImGui::Text("Accumulated frames: %u", m_tracer.accumulatedFrames());
    ImGui::Text("Instances: %zu, lights: %zu, triangles: %zu", m_renderScene.instances.size(),
                m_renderScene.lights.size(), m_renderScene.triangleCount());
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
    ImGui::Text("Speed: %.2f", m_cameraController.speed());
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
        setTool(ToolKind::None);
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
        addItem("Sphere", SphereComponent{});
        addItem("Mesh (cube)", cubeMesh());
        addItem("Stroke", StrokeComponent{});
        addItem("Line Segment", LineShapeComponent{});
        addItem("Ellipse", EllipseShapeComponent{});
        addItem("Arc", ArcShapeComponent{});
        addItem("Polygon", PolygonShapeComponent{
                               .vertices = {{-0.5F, -0.5F, 0}, {0.5F, -0.5F, 0}, {0, 0.5F, 0}}});
        addItem("Point", PointShapeComponent{});
        ImGui::EndPopup();
    }

    if (changed) {
        m_scene.markDirty();
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
    setTool(ToolKind::None);
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
            m_portals.createPortal(m_scene, m_camera);
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("View")) {
        if (ImGui::MenuItem("Reset camera")) {
            resetCamera();
        }
        ImGui::MenuItem("Cockpit HUD", nullptr, &m_options.showHud);
        ImGui::EndMenu();
    }
}

}  // namespace startraveler
