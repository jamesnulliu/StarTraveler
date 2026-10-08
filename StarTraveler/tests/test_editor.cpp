// Drives the real editor tools, selection and portals through a headless
// Dear ImGui context with synthetic mouse/keyboard events (no window, no GPU).

#include <functional>
#include <optional>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <glm/geometric.hpp>
#include <imgui.h>

#include <starcore/scene/entity.hpp>

#include "editor/selection.hpp"
#include "editor/tools.hpp"
#include "gameplay/portals.hpp"

using namespace starcore;
using namespace startraveler;
using Catch::Matchers::WithinAbs;

namespace
{

constexpr std::uint32_t kWidth = 800;
constexpr std::uint32_t kHeight = 600;

/** @brief Headless ImGui context; each step() is one frame. */
class Harness
{
public:
    Harness()
    {
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        // Apply all queued events in the frame they were sent (by default
        // ImGui defers e.g. a click that follows a mouse move to the next
        // frame), so each step() is exactly one frame of input.
        io.ConfigInputTrickleEventQueue = false;
        io.DisplaySize = ImVec2(static_cast<float>(kWidth), static_cast<float>(kHeight));
        io.DeltaTime = 1.0F / 60.0F;
        io.Fonts->Build();
        camera.setViewport(kWidth, kHeight);
    }

    ~Harness()
    {
        ImGui::DestroyContext();
    }
    Harness(const Harness&) = delete;
    auto operator=(const Harness&) -> Harness& = delete;

    /** @brief One frame: queue input events, then run `body` inside it. */
    void step(Vec2 mouse, std::function<void(EditorContext&)> body,
              std::optional<bool> leftDown = std::nullopt,
              std::optional<ImGuiKey> keyPress = std::nullopt, bool ctrl = false)
    {
        ImGuiIO& io = ImGui::GetIO();
        io.AddMousePosEvent(mouse.x, mouse.y);
        if (leftDown.has_value()) {
            io.AddMouseButtonEvent(ImGuiMouseButton_Left, *leftDown);
        }
        io.AddKeyEvent(ImGuiMod_Ctrl, ctrl);
        if (keyPress.has_value()) {
            io.AddKeyEvent(*keyPress, true);
        }
        ImGui::NewFrame();
        EditorContext context{.scene = scene,
                              .camera = camera,
                              .viewport = {.size = {kWidth, kHeight},
                                           .mouse = mouse,
                                           .hovered = true,
                                           .cameraActive = false},
                              .stroke = {},
                              .drawDepth = 5.0F};
        body(context);
        ImGui::EndFrame();
        if (keyPress.has_value()) {
            io.AddKeyEvent(*keyPress, false);
        }
    }

    /** @brief Press and release the left button at `mouse`. */
    void click(Vec2 mouse, const std::function<void(EditorContext&)>& body)
    {
        step(mouse, body, true);
        step(mouse, body, false);
    }

    Scene scene;
    Camera camera;  // At (0, 0, 10) looking down -Z.
};

auto projected(const Camera& camera, Vec3 world) -> Vec2
{
    const auto screen = camera.worldToScreen(world);
    REQUIRE(screen.has_value());
    return *screen;
}

auto onlyEntity(Scene& scene) -> Entity
{
    REQUIRE(scene.entityCount() == 1);
    return scene.wrap(*scene.registry().view<IdComponent>().begin());
}

}  // namespace

TEST_CASE("Line tool places endpoints under the clicks", "[editor][tools]")
{
    Harness h;
    auto tool = makeTool(ToolKind::Line);
    ToolResult result;
    auto run = [&](EditorContext& c) {
        if (auto r = tool->update(c); r.finished) {
            result = r;
        }
    };

    h.click({100, 100}, run);
    REQUIRE(h.scene.entityCount() == 0);  // Only previewed so far.
    REQUIRE(tool->previewScene().entityCount() == 1);
    h.step({300, 200}, run);
    h.click({300, 200}, run);

    REQUIRE(result.finished);
    Entity line = onlyEntity(h.scene);
    REQUIRE(line == result.created);
    REQUIRE(line.has<StrokeComponent>());
    const Mat4 world = line.worldTransform();
    const auto& shape = line.get<LineShapeComponent>();
    REQUIRE(glm::distance(projected(h.camera, Vec3(world * Vec4(shape.start, 1))), Vec2{100, 100}) < 0.5F);
    REQUIRE(glm::distance(projected(h.camera, Vec3(world * Vec4(shape.end, 1))), Vec2{300, 200}) < 0.5F);
    REQUIRE(tool->previewScene().entityCount() == 0);
}

TEST_CASE("Ellipse tool sizes radii from the mouse", "[editor][tools]")
{
    Harness h;
    auto tool = makeTool(ToolKind::Ellipse);
    auto run = [&](EditorContext& c) { (void) tool->update(c); };

    h.click({400, 300}, run);
    h.step({500, 250}, run);
    h.click({500, 250}, run);

    Entity ellipse = onlyEntity(h.scene);
    const auto& shape = ellipse.get<EllipseShapeComponent>();
    const Mat4 world = ellipse.worldTransform();
    // The X apex projects 100 px right of the center, the Y apex 50 px up.
    const Vec2 xApex = projected(h.camera, Vec3(world * Vec4(shape.radiusX, 0, 0, 1)));
    const Vec2 yApex = projected(h.camera, Vec3(world * Vec4(0, shape.radiusY, 0, 1)));
    REQUIRE_THAT(xApex.x, WithinAbs(500.0, 0.5));
    REQUIRE_THAT(yApex.y, WithinAbs(250.0, 0.5));
}

TEST_CASE("Arc tool follows the mouse past 180 degrees", "[editor][tools]")
{
    Harness h;
    auto tool = makeTool(ToolKind::Arc);
    auto run = [&](EditorContext& c) { (void) tool->update(c); };

    const Vec2 center{400, 300};
    h.click(center, run);
    h.click({500, 300}, run);  // Start point: right of the center.
    // Sweep counter-clockwise on screen (up, left, down) = 270 degrees.
    for (const Vec2 p : {Vec2{470, 230}, Vec2{400, 200}, Vec2{330, 230},
                         Vec2{300, 300}, Vec2{330, 370}, Vec2{400, 400}}) {
        h.step(p, run);
    }
    h.click({400, 400}, run);

    const auto& arc = onlyEntity(h.scene).get<ArcShapeComponent>();
    // Legacy computed the angle with acos and could never exceed 180 degrees.
    REQUIRE_THAT(std::abs(arc.sweepAngle), WithinAbs(1.5 * kPi, 1e-3));
}

TEST_CASE("Polygon and curve tools collect points until Enter", "[editor][tools]")
{
    Harness h;
    SECTION("polygon")
    {
        auto tool = makeTool(ToolKind::Polygon);
        auto run = [&](EditorContext& c) { (void) tool->update(c); };
        h.click({100, 100}, run);
        h.click({200, 100}, run);
        h.step({200, 100}, run, std::nullopt, ImGuiKey_Enter);  // Too few points.
        REQUIRE(h.scene.entityCount() == 0);
        h.click({150, 200}, run);
        h.step({150, 200}, run, std::nullopt, ImGuiKey_Enter);

        Entity polygon = onlyEntity(h.scene);
        REQUIRE(polygon.get<PolygonShapeComponent>().vertices.size() == 3);
        // Translation is the centroid of the clicked points.
        const Vec2 centroid = projected(h.camera, polygon.get<TransformComponent>().translation);
        REQUIRE(glm::distance(centroid, Vec2{150, 400.0F / 3.0F}) < 0.5F);
    }
    SECTION("curve")
    {
        auto tool = makeTool(ToolKind::Curve);
        auto run = [&](EditorContext& c) { (void) tool->update(c); };
        for (const Vec2 p : {Vec2{100, 300}, Vec2{200, 100}, Vec2{300, 500}, Vec2{400, 300}}) {
            h.click(p, run);
        }
        h.step({400, 300}, run, std::nullopt, ImGuiKey_Enter);
        Entity curve = onlyEntity(h.scene);
        REQUIRE(curve.get<CurveShapeComponent>().controlPoints.size() == 4);
        REQUIRE(curve.get<CurveShapeComponent>().type == CurveType::BSpline);
    }
}

TEST_CASE("Group tool places a marker", "[editor][tools]")
{
    Harness h;
    auto tool = makeTool(ToolKind::Group);
    h.click({250, 250}, [&](EditorContext& c) { (void) tool->update(c); });
    Entity group = onlyEntity(h.scene);
    REQUIRE(group.has<PointShapeComponent>());
    REQUIRE(group.get<StrokeComponent>().thickness == 17);
}

TEST_CASE("Selection: handles, dragging, parenting and deletion", "[editor][selection]")
{
    Harness h;
    // A line from screen (200, 300) to (600, 300) at depth 5.
    const Vec3 a = h.camera.screenToWorld({200, 300}, 5.0F);
    const Vec3 b = h.camera.screenToWorld({600, 300}, 5.0F);
    Entity line = h.scene.createEntity("Line");
    line.add<LineShapeComponent>(LineShapeComponent{a, b});
    Entity other = h.scene.createEntity("Other");
    other.add<PointShapeComponent>();

    Selection selection;
    std::optional<entt::entity> picked = line.handle();
    auto run = [&](EditorContext& c) { selection.update(c, picked); };

    // Click selects (and grabs, without moving).
    h.click({400, 300}, run);
    REQUIRE(selection.selected() == line);
    REQUIRE(glm::distance(line.get<LineShapeComponent>().start, a) < 1e-5F);

    // Drag the end handle from (600, 300) to (650, 350).
    h.step({600, 300}, run, true);
    REQUIRE(selection.isDragging());
    h.step({650, 350}, run);
    h.step({650, 350}, run, false);
    REQUIRE_FALSE(selection.isDragging());
    const Vec3 end = Vec3(line.worldTransform() * Vec4(line.get<LineShapeComponent>().end, 1));
    REQUIRE(glm::distance(projected(h.camera, end), Vec2{650, 350}) < 0.5F);
    REQUIRE_THAT(h.camera.depthOf(end), WithinAbs(5.0, 1e-3));  // Same depth.

    // Drag the body: the whole entity moves 100 px down.
    h.step({400, 300}, run, true);
    h.step({400, 400}, run);
    h.step({400, 400}, run, false);
    const Vec3 start = Vec3(line.worldTransform() * Vec4(line.get<LineShapeComponent>().start, 1));
    REQUIRE(glm::distance(projected(h.camera, start), Vec2{200, 400}) < 0.5F);

    // Ctrl+click on another entity makes it a child of the selection.
    picked = other.handle();
    h.step({50, 50}, run, true, std::nullopt, true);
    h.step({50, 50}, run, false, std::nullopt, true);
    REQUIRE(h.scene.parentOf(other) == line);
    REQUIRE(selection.selected() == line);

    // Delete removes the selection (and its children).
    h.step({50, 50}, run, std::nullopt, ImGuiKey_Delete);
    REQUIRE_FALSE(line.isValid());
    REQUIRE_FALSE(other.isValid());
    REQUIRE_FALSE(selection.selected());
}

TEST_CASE("Portals pair up and teleport on crossing", "[gameplay][portals]")
{
    Scene scene;
    Camera camera;
    PortalSystem portals;

    // Legacy bug: with no portals at all, standing near the origin teleported.
    camera.setPosition({0.1F, 0.0F, 0.1F});
    portals.update(scene, camera);
    camera.setPosition({0.0F, 0.0F, 0.0F});
    REQUIRE_FALSE(portals.update(scene, camera));

    camera.setPosition({0.0F, 0.0F, 10.0F});  // Facing -Z.
    Entity first = portals.createPortal(scene, camera);
    REQUIRE(first.get<PortalComponent>().linkedPortal.value() == 0);
    camera.setPosition({20.0F, 0.0F, 10.0F});
    camera.setRotation(0.0F, 0.0F);  // Facing +X.
    Entity second = portals.createPortal(scene, camera);
    REQUIRE(first.get<PortalComponent>().linkedPortal == second.uuid());
    REQUIRE(second.get<PortalComponent>().linkedPortal == first.uuid());

    // Walk through the first portal (at z = 7, facing +Z towards us).
    camera.setPosition({0.0F, 0.0F, 7.5F});
    camera.setRotation(-90.0F, 0.0F);
    portals.reset();
    REQUIRE_FALSE(portals.update(scene, camera));
    camera.setPosition({0.0F, 0.0F, 6.5F});
    REQUIRE(portals.update(scene, camera));

    // Out of the second portal (at x = 23), half a unit in front of it, and
    // looking away from it (towards -X, back where its creator stood).
    REQUIRE_THAT(camera.position().x, WithinAbs(22.5, 1e-3));
    REQUIRE_THAT(camera.position().z, WithinAbs(10.0, 1e-3));
    REQUIRE(glm::distance(camera.forward(), Vec3{-1.0F, 0.0F, 0.0F}) < 1e-3F);

    // Walking beside a portal (outside its rectangle) does nothing.
    camera.setPosition({5.0F, 0.0F, 7.5F});
    portals.reset();
    portals.update(scene, camera);
    camera.setPosition({5.0F, 0.0F, 6.5F});
    REQUIRE_FALSE(portals.update(scene, camera));
}
