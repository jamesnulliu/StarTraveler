module;

#include <imgui.h>

module startraveler.editor;

import std;
import glm;
import starcore.math;
import starcore.platform;
import starcore.raster;
import starcore.scene;

namespace startraveler
{

using namespace starcore;

auto EditorContext::cameraFacingRotation() const -> Quat
{
    // Columns: local X, Y, Z in world space (Z points back at the camera).
    return glm::quat_cast(Mat3(camera.right(), camera.up(), -camera.forward()));
}

auto toolName(ToolKind kind) -> std::string_view
{
    switch (kind) {
    case ToolKind::None:
        return "Select";
    case ToolKind::Line:
        return "Line Segment";
    case ToolKind::Ellipse:
        return "Ellipse";
    case ToolKind::Arc:
        return "Arc";
    case ToolKind::Polygon:
        return "Polygon";
    case ToolKind::Curve:
        return "Curve";
    case ToolKind::Group:
        return "Group";
    case ToolKind::SeedFill:
        return "Seed Fill";
    }
    return "Unknown";
}

auto Tool::commit(EditorContext& context, Entity shape) -> ToolResult
{
    Entity created = context.scene.copyEntity(shape);
    m_preview.clear();
    return {.finished = true, .created = created};
}

namespace
{

auto makeStroke(const EditorContext& context, Vec4 color) -> StrokeComponent
{
    return {.color = color,
            .thickness = context.stroke.thickness,
            .pattern = context.stroke.pattern};
}

/** @brief Dashed helper polyline (an order-2 B-spline is the polyline). */
void setGuide(Scene& preview, Entity& guide, const EditorContext& context,
              std::vector<Vec3> points)
{
    if (!guide) {
        guide = preview.createEntity("Guide");
        guide.add<CurveShapeComponent>().order = 2;
        guide.add<StrokeComponent>(StrokeComponent{
            .color = context.stroke.secondary, .pattern = LinePattern::Dash});
    }
    guide.get<CurveShapeComponent>().controlPoints = std::move(points);
}

// Line segment: click start, click end. -----------------------------------------------------------

class LineTool final : public Tool
{
public:
    [[nodiscard]] auto kind() const -> ToolKind override
    {
        return ToolKind::Line;
    }
    [[nodiscard]] auto hint() const -> std::string_view override
    {
        return "Click the start point, then the end point. Esc cancels.";
    }

    auto update(EditorContext& context) -> ToolResult override
    {
        if (!context.viewport.acceptsInput()) {
            return {};
        }
        const Vec3 mouse = context.mouseWorld();
        if (m_line) {
            // Keep the transform at the midpoint (like legacy's centroid).
            const Vec3 start = m_start;
            const Vec3 mid = (start + mouse) * 0.5F;
            m_line.get<TransformComponent>().translation = mid;
            auto& line = m_line.get<LineShapeComponent>();
            line.start = start - mid;
            line.end = mouse - mid;
        }
        if (!input::isMouseClicked(ImGuiMouseButton_Left)) {
            return {};
        }
        if (!m_line) {
            m_start = mouse;
            m_line = m_preview.createEntity("Line Segment");
            m_line.add<LineShapeComponent>(LineShapeComponent{Vec3(0.0F), Vec3(0.0F)});
            m_line.add<StrokeComponent>(makeStroke(context, context.stroke.primary));
            return {};
        }
        if (glm::distance(m_start, mouse) < 1e-4F) {
            return {};  // Ignore a zero-length segment.
        }
        auto result = commit(context, m_line);
        m_line = {};
        return result;
    }

private:
    Vec3 m_start{0.0F};
    Entity m_line;
};

// Ellipse: click center, move, click to fix the radii. --------------------------------------------

class EllipseTool final : public Tool
{
public:
    [[nodiscard]] auto kind() const -> ToolKind override
    {
        return ToolKind::Ellipse;
    }
    [[nodiscard]] auto hint() const -> std::string_view override
    {
        return "Click the center, move to size the ellipse, click to finish.";
    }

    auto update(EditorContext& context) -> ToolResult override
    {
        if (!context.viewport.acceptsInput()) {
            return {};
        }
        const Vec3 mouse = context.mouseWorld();
        if (m_ellipse) {
            // Radii along the camera's right/up axes (the ellipse plane).
            const Vec3 offset = mouse - m_ellipse.get<TransformComponent>().translation;
            auto& ellipse = m_ellipse.get<EllipseShapeComponent>();
            ellipse.radiusX = std::abs(glm::dot(offset, context.camera.right()));
            ellipse.radiusY = std::abs(glm::dot(offset, context.camera.up()));
        }
        if (!input::isMouseClicked(ImGuiMouseButton_Left)) {
            return {};
        }
        if (!m_ellipse) {
            m_ellipse = m_preview.createEntity("Ellipse");
            auto& transform = m_ellipse.get<TransformComponent>();
            transform.translation = mouse;
            transform.rotation = context.cameraFacingRotation();
            m_ellipse.add<EllipseShapeComponent>(EllipseShapeComponent{0.0F, 0.0F});
            m_ellipse.add<StrokeComponent>(makeStroke(context, context.stroke.primary));
            return {};
        }
        const auto& ellipse = m_ellipse.get<EllipseShapeComponent>();
        if (ellipse.radiusX < 1e-4F && ellipse.radiusY < 1e-4F) {
            return {};
        }
        auto result = commit(context, m_ellipse);
        m_ellipse = {};
        return result;
    }

private:
    Entity m_ellipse;
};

// Arc: click center, click the start (radius), move around, click to finish. -----------------------

class ArcTool final : public Tool
{
public:
    [[nodiscard]] auto kind() const -> ToolKind override
    {
        return ToolKind::Arc;
    }
    [[nodiscard]] auto hint() const -> std::string_view override
    {
        return "Click the center, then the start point; move around the center "
               "(either direction, past 180 degrees too) and click to finish.";
    }

    auto update(EditorContext& context) -> ToolResult override
    {
        if (!context.viewport.acceptsInput()) {
            return {};
        }
        const Vec3 mouse = context.mouseWorld();
        if (m_arc) {
            const auto& transform = m_arc.get<TransformComponent>();
            const Vec3 local = glm::inverse(transform.rotation) *
                               (mouse - transform.translation);
            const float angle = std::atan2(local.y, local.x);
            auto& arc = m_arc.get<ArcShapeComponent>();
            if (!m_hasStart) {
                arc.radius = glm::length(Vec2(local));
                arc.startAngle = angle;
                setGuide(m_preview, m_guide, context,
                         {transform.translation, mouse});
            } else {
                // Unwrap the angle so the sweep is continuous; this picks the
                // minor or major arc naturally (legacy used acos, which only
                // gives [0, 180] and lost the direction).
                float delta = angle - m_lastAngle;
                if (delta > kPi) {
                    delta -= kTwoPi;
                } else if (delta < -kPi) {
                    delta += kTwoPi;
                }
                m_lastAngle = angle;
                arc.sweepAngle = std::clamp(arc.sweepAngle + delta, -kTwoPi, kTwoPi);
                setGuide(m_preview, m_guide, context,
                         {startPoint(transform, arc), transform.translation, mouse});
            }
        }
        if (!input::isMouseClicked(ImGuiMouseButton_Left)) {
            return {};
        }
        if (!m_arc) {
            m_arc = m_preview.createEntity("Arc");
            auto& transform = m_arc.get<TransformComponent>();
            transform.translation = mouse;
            transform.rotation = context.cameraFacingRotation();
            m_arc.add<ArcShapeComponent>(ArcShapeComponent{0.0F, 0.0F, 0.0F});
            m_arc.add<StrokeComponent>(makeStroke(context, context.stroke.primary));
            m_hasStart = false;
            return {};
        }
        if (!m_hasStart) {
            if (m_arc.get<ArcShapeComponent>().radius > 1e-4F) {
                m_hasStart = true;
                m_lastAngle = m_arc.get<ArcShapeComponent>().startAngle;
            }
            return {};
        }
        if (std::abs(m_arc.get<ArcShapeComponent>().sweepAngle) < 1e-3F) {
            return {};
        }
        auto result = commit(context, m_arc);
        m_arc = {};
        m_guide = {};
        return result;
    }

private:
    static auto startPoint(const TransformComponent& transform,
                           const ArcShapeComponent& arc) -> Vec3
    {
        return transform.translation +
               transform.rotation * Vec3(arc.radius * std::cos(arc.startAngle),
                                         arc.radius * std::sin(arc.startAngle),
                                         0.0F);
    }

    Entity m_arc;
    Entity m_guide;
    bool m_hasStart = false;
    float m_lastAngle = 0.0F;
};

// Polygon / curve: click points, Enter (or right-click) to finish. ---------------------------------

/** @brief Shared logic of tools that collect a list of points. */
class PointListTool : public Tool
{
public:
    auto update(EditorContext& context) -> ToolResult override
    {
        const bool finishRequested =
            input::isKeyPressed(ImGuiKey_Enter) ||
            input::isKeyPressed(ImGuiKey_KeypadEnter) ||
            (context.viewport.hovered &&
             input::isMouseClicked(ImGuiMouseButton_Middle));
        if (finishRequested && m_points.size() >= minimumPoints()) {
            Entity shape = buildShape(context, m_points);
            auto result = commit(context, shape);
            m_points.clear();
            m_guide = {};
            return result;
        }
        if (!context.viewport.acceptsInput()) {
            return {};
        }
        const Vec3 mouse = context.mouseWorld();
        if (input::isMouseClicked(ImGuiMouseButton_Left)) {
            m_points.push_back(mouse);
        }
        if (m_points.empty()) {
            return {};
        }
        // Preview: the shape with the mouse as a tentative next point.
        std::vector<Vec3> tentative = m_points;
        tentative.push_back(mouse);
        m_preview.clear();
        m_guide = {};
        if (tentative.size() >= minimumPoints()) {
            buildShape(context, tentative);
        }
        setGuide(m_preview, m_guide, context, guidePoints(tentative));
        return {};
    }

protected:
    [[nodiscard]] virtual auto minimumPoints() const -> std::size_t = 0;
    /** @brief Create the shape entity in m_preview from world points. */
    virtual auto buildShape(const EditorContext& context,
                            const std::vector<Vec3>& points) -> Entity = 0;
    [[nodiscard]] virtual auto guidePoints(const std::vector<Vec3>& points) const
        -> std::vector<Vec3>
    {
        return points;
    }

    /** @brief Centroid (becomes the entity translation, like legacy). */
    static auto centroid(const std::vector<Vec3>& points) -> Vec3
    {
        Vec3 sum{0.0F};
        for (const Vec3& p : points) {
            sum += p;
        }
        return sum / static_cast<float>(points.size());
    }

    std::vector<Vec3> m_points;
    Entity m_guide;
};

class PolygonTool final : public PointListTool
{
public:
    [[nodiscard]] auto kind() const -> ToolKind override
    {
        return ToolKind::Polygon;
    }
    [[nodiscard]] auto hint() const -> std::string_view override
    {
        return "Click vertices; Enter or middle-click finishes (3+ points), "
               "Esc cancels.";
    }

protected:
    [[nodiscard]] auto minimumPoints() const -> std::size_t override
    {
        return 3;
    }

    auto buildShape(const EditorContext& context,
                    const std::vector<Vec3>& points) -> Entity override
    {
        const Vec3 center = centroid(points);
        Entity entity = m_preview.createEntity("Polygon");
        entity.get<TransformComponent>().translation = center;
        auto& polygon = entity.add<PolygonShapeComponent>();
        for (const Vec3& p : points) {
            polygon.vertices.push_back(p - center);
        }
        polygon.fillColor = context.stroke.primary;
        return entity;
    }

    [[nodiscard]] auto guidePoints(const std::vector<Vec3>& points) const
        -> std::vector<Vec3> override
    {
        auto closed = points;
        closed.push_back(points.front());
        return closed;
    }
};

class CurveTool final : public PointListTool
{
public:
    [[nodiscard]] auto kind() const -> ToolKind override
    {
        return ToolKind::Curve;
    }
    [[nodiscard]] auto hint() const -> std::string_view override
    {
        return "Click control points; Enter or middle-click finishes (2+ "
               "points), Esc cancels. Type/order are editable in the Inspector.";
    }

protected:
    [[nodiscard]] auto minimumPoints() const -> std::size_t override
    {
        return 2;
    }

    auto buildShape(const EditorContext& context,
                    const std::vector<Vec3>& points) -> Entity override
    {
        const Vec3 center = centroid(points);
        Entity entity = m_preview.createEntity("Curve");
        entity.get<TransformComponent>().translation = center;
        auto& curve = entity.add<CurveShapeComponent>();
        for (const Vec3& p : points) {
            curve.controlPoints.push_back(p - center);
        }
        curve.type = CurveType::BSpline;
        curve.order = 3;
        entity.add<StrokeComponent>(makeStroke(context, context.stroke.primary));
        return entity;
    }
};

// Group: click to place a group marker; children are attached in the editor. ----------------------

class GroupTool final : public Tool
{
public:
    [[nodiscard]] auto kind() const -> ToolKind override
    {
        return ToolKind::Group;
    }
    [[nodiscard]] auto hint() const -> std::string_view override
    {
        return "Click to place a group. Then Ctrl+click entities (or drag them "
               "in the Hierarchy) to make them children of the selection.";
    }

    auto update(EditorContext& context) -> ToolResult override
    {
        if (!context.viewport.acceptsInput() ||
            !input::isMouseClicked(ImGuiMouseButton_Left)) {
            return {};
        }
        Entity group = m_preview.createEntity("Group");
        group.get<TransformComponent>().translation = context.mouseWorld();
        group.add<PointShapeComponent>();
        group.add<StrokeComponent>(StrokeComponent{
            .color = context.stroke.primary, .thickness = 17});
        return commit(context, group);
    }
};

}  // namespace

auto makeTool(ToolKind kind) -> std::unique_ptr<Tool>
{
    switch (kind) {
    case ToolKind::Line:
        return std::make_unique<LineTool>();
    case ToolKind::Ellipse:
        return std::make_unique<EllipseTool>();
    case ToolKind::Arc:
        return std::make_unique<ArcTool>();
    case ToolKind::Polygon:
        return std::make_unique<PolygonTool>();
    case ToolKind::Curve:
        return std::make_unique<CurveTool>();
    case ToolKind::Group:
        return std::make_unique<GroupTool>();
    case ToolKind::None:
    case ToolKind::SeedFill:
        return nullptr;
    }
    return nullptr;
}

}  // namespace startraveler
