#include <algorithm>
#include <cmath>

#include <glm/geometric.hpp>
#include <glm/matrix.hpp>

#include <starcore/math/color.hpp>
#include <starcore/platform/input.hpp>

#include "editor/selection.hpp"

import starcore.raster;

namespace startraveler
{

using namespace starcore;

namespace
{

constexpr float kHandlePickRadius = 8.0F;  // Pixels.
constexpr std::uint32_t kHandleSize = 9;   // Legacy control point thickness.
const std::uint32_t kHandleColor = packRgba8({0.0F, 1.0F, 0.0F, 1.0F});

auto toWorld(const Mat4& world, Vec3 local) -> Vec3
{
    return Vec3(world * Vec4(local, 1.0F));
}

auto toLocal(const Mat4& world, Vec3 point) -> Vec3
{
    return Vec3(glm::inverse(world) * Vec4(point, 1.0F));
}

}  // namespace

void Selection::select(Entity entity)
{
    m_selected = entity;
    m_drag.reset();
}

void Selection::clear()
{
    m_selected = {};
    m_drag.reset();
}

auto Selection::handles(const Entity& entity) const -> std::vector<Vec3>
{
    std::vector<Vec3> result;
    const Mat4 world = entity.worldTransform();
    if (const auto* line = entity.tryGet<LineShapeComponent>()) {
        result.push_back(toWorld(world, line->start));
        result.push_back(toWorld(world, line->end));
    }
    if (const auto* polygon = entity.tryGet<PolygonShapeComponent>()) {
        for (const Vec3& v : polygon->vertices) {
            result.push_back(toWorld(world, v));
        }
    }
    if (const auto* curve = entity.tryGet<CurveShapeComponent>()) {
        for (const Vec3& p : curve->controlPoints) {
            result.push_back(toWorld(world, p));
        }
    }
    if (const auto* ellipse = entity.tryGet<EllipseShapeComponent>()) {
        result.push_back(toWorld(world, {ellipse->radiusX, 0.0F, 0.0F}));
        result.push_back(toWorld(world, {0.0F, ellipse->radiusY, 0.0F}));
    }
    if (const auto* arc = entity.tryGet<ArcShapeComponent>()) {
        const float end = arc->startAngle + arc->sweepAngle;
        result.push_back(toWorld(world, {arc->radius * std::cos(arc->startAngle),
                                         arc->radius * std::sin(arc->startAngle), 0.0F}));
        result.push_back(toWorld(world, {arc->radius * std::cos(end),
                                         arc->radius * std::sin(end), 0.0F}));
    }
    return result;
}

void Selection::moveHandle(const Entity& entity, std::size_t index,
                           Vec3 worldPosition) const
{
    const Vec3 local = toLocal(entity.worldTransform(), worldPosition);
    std::size_t base = 0;

    if (entity.has<LineShapeComponent>()) {
        if (index < base + 2) {
            entity.patch<LineShapeComponent>([&](auto& line) {
                (index == base ? line.start : line.end) = local;
            });
            return;
        }
        base += 2;
    }
    if (const auto* polygon = entity.tryGet<PolygonShapeComponent>()) {
        if (index < base + polygon->vertices.size()) {
            entity.patch<PolygonShapeComponent>(
                [&](auto& p) { p.vertices[index - base] = local; });
            return;
        }
        base += polygon->vertices.size();
    }
    if (const auto* curve = entity.tryGet<CurveShapeComponent>()) {
        if (index < base + curve->controlPoints.size()) {
            entity.patch<CurveShapeComponent>(
                [&](auto& c) { c.controlPoints[index - base] = local; });
            return;
        }
        base += curve->controlPoints.size();
    }
    if (entity.has<EllipseShapeComponent>()) {
        if (index < base + 2) {
            entity.patch<EllipseShapeComponent>([&](auto& ellipse) {
                if (index == base) {
                    ellipse.radiusX = std::abs(local.x);
                } else {
                    ellipse.radiusY = std::abs(local.y);
                }
            });
            return;
        }
        base += 2;
    }
    if (entity.has<ArcShapeComponent>() && index < base + 2) {
        entity.patch<ArcShapeComponent>([&](auto& arc) {
            const float angle = std::atan2(local.y, local.x);
            if (index == base) {
                // Start handle: radius and start angle; keep the end fixed.
                const float end = arc.startAngle + arc.sweepAngle;
                arc.radius = glm::length(Vec2(local));
                arc.startAngle = angle;
                arc.sweepAngle = end - angle;
            } else {
                // End handle: shortest change of the sweep.
                float delta = angle - (arc.startAngle + arc.sweepAngle);
                delta = std::remainder(delta, kTwoPi);
                arc.sweepAngle = std::clamp(arc.sweepAngle + delta, -kTwoPi, kTwoPi);
            }
        });
    }
}

auto Selection::handleUnderMouse(const EditorContext& context) const
    -> std::optional<std::size_t>
{
    if (!m_selected) {
        return std::nullopt;
    }
    const auto points = handles(m_selected);
    for (std::size_t i = 0; i < points.size(); ++i) {
        const auto screen = context.camera.worldToScreen(points[i]);
        if (screen && glm::distance(*screen, context.viewport.mouse) <= kHandlePickRadius) {
            return i;
        }
    }
    return std::nullopt;
}

void Selection::update(EditorContext& context,
                       std::optional<entt::entity> pickedEntity)
{
    if (m_selected && !m_selected.isValid()) {
        clear();  // Destroyed elsewhere (e.g. undo, scene reload).
    }

    // Keyboard shortcuts work while the viewport is hovered.
    if (context.viewport.hovered) {
        if (m_selected && input::isKeyPressed(ImGuiKey_Delete)) {
            context.scene.destroyEntity(m_selected);
            clear();
            return;
        }
        if (input::isKeyPressed(ImGuiKey_Escape)) {
            clear();
            return;
        }
    }

    // Continue an active drag.
    if (m_drag.has_value()) {
        if (!input::isMouseDown(ImGuiMouseButton_Left) || !m_selected) {
            m_drag.reset();
            return;
        }
        const Vec3 target = context.camera.screenToWorld(context.viewport.mouse, m_drag->depth);
        if (m_drag->handle.has_value()) {
            moveHandle(m_selected, *m_drag->handle, target);
        } else {
            const Vec3 newOrigin = m_drag->originWorld + (target - m_drag->grabWorld);
            Mat4 parentWorld{1.0F};
            if (Entity parent = context.scene.parentOf(m_selected)) {
                parentWorld = parent.worldTransform();
            }
            const Vec3 local = toLocal(parentWorld, newOrigin);
            m_selected.patch<TransformComponent>([&](auto& t) { t.translation = local; });
        }
        return;
    }

    if (!context.viewport.acceptsInput() ||
        !input::isMouseClicked(ImGuiMouseButton_Left)) {
        return;
    }

    // Start dragging a handle of the selection.
    if (const auto handle = handleUnderMouse(context)) {
        const Vec3 world = handles(m_selected)[*handle];
        m_drag = Drag{.handle = handle,
                      .depth = context.camera.depthOf(world),
                      .grabWorld = world};
        return;
    }

    Entity picked = pickedEntity ? context.scene.wrap(*pickedEntity) : Entity{};

    // Ctrl+click: attach the clicked entity to the selection (legacy: right
    // click while a Parent entity was selected).
    if (ImGui::GetIO().KeyCtrl && m_selected && picked && picked != m_selected) {
        context.scene.setParent(picked, m_selected);
        return;
    }

    if (!picked) {
        clear();
        return;
    }
    if (picked != m_selected) {
        select(picked);
    }

    // Grab the entity: it follows the mouse in the plane at the grab depth.
    // Use the depth of the visible geometry (its handles), not of the origin,
    // which may lie elsewhere; otherwise the shape moves faster or slower
    // than the mouse (parallax).
    const Mat4 world = picked.worldTransform();
    const Vec3 origin = Vec3(world[3]);
    Vec3 grabPoint = origin;
    if (const auto points = handles(picked); !points.empty()) {
        grabPoint = Vec3(0.0F);
        for (const Vec3& p : points) {
            grabPoint += p;
        }
        grabPoint /= static_cast<float>(points.size());
    }
    float depth = context.camera.depthOf(grabPoint);
    if (depth <= context.camera.nearClip()) {
        depth = context.drawDepth;
    }
    m_drag = Drag{.handle = std::nullopt,
                  .depth = depth,
                  .grabWorld = context.camera.screenToWorld(context.viewport.mouse, depth),
                  .originWorld = origin};
}

void Selection::drawHandles(const EditorContext& context, ColorImage& frame) const
{
    if (!m_selected || !m_selected.isValid()) {
        return;
    }
    raster::StrokeWriter writer(frame, nullptr, kHandleColor, kNoEntityId, kHandleSize);
    for (const Vec3& point : handles(m_selected)) {
        if (const auto screen = context.camera.worldToScreen(point)) {
            writer(raster::toPixel(*screen));
        }
    }
}

}  // namespace startraveler
