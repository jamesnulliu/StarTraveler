#include <algorithm>
#include <cmath>
#include <span>
#include <vector>

#include <glm/geometric.hpp>

#include "starcore/math/color.hpp"
#include "starcore/raster/clip.hpp"
#include "starcore/raster/curve.hpp"
#include "starcore/raster/ellipse.hpp"
#include "starcore/raster/line.hpp"
#include "starcore/raster/polygon.hpp"
#include "starcore/raster/stroke.hpp"
#include "starcore/render/overlay.hpp"
#include "starcore/scene/components.hpp"
#include "starcore/scene/scene.hpp"

namespace starcore
{

namespace
{

constexpr std::size_t kEllipseSegments = 128;
constexpr std::size_t kCurveSegments = 256;
constexpr std::size_t kFullCircleArcSegments = 128;

class OverlayRenderer
{
public:
    OverlayRenderer(const Scene& scene, const Camera& camera,
                    ColorImage& color, IdImage* ids)
        : m_scene(scene), m_camera(camera), m_color(color), m_ids(ids),
          m_viewport{Vec2(0.0F), Vec2(color.size())}
    {
    }

    void run()
    {
        const auto& registry = m_scene.registry();

        for (const auto [entity, polygon] :
             registry.view<const PolygonShapeComponent>().each()) {
            if (polygon.filled) {
                fillPolygon(entity, polygon);
            }
        }

        for (const auto [entity, polygon] :
             registry.view<const PolygonShapeComponent>().each()) {
            if (const auto* stroke = registry.try_get<StrokeComponent>(entity)) {
                const auto world = toWorld(entity, polygon.vertices);
                auto writer = makeWriter(entity, *stroke);
                drawPolyline(world, /*closed=*/true, LineAlgorithm::Bresenham,
                             *stroke, writer);
            }
        }

        for (const auto [entity, line] :
             registry.view<const LineShapeComponent>().each()) {
            const auto stroke = strokeOf(entity);
            auto writer = makeWriter(entity, stroke);
            const Mat4 world = m_scene.worldTransform(entity);
            drawSegment(Vec3(world * Vec4(line.start, 1.0F)),
                        Vec3(world * Vec4(line.end, 1.0F)), line.algorithm,
                        stroke, writer);
        }

        for (const auto [entity, ellipse] :
             registry.view<const EllipseShapeComponent>().each()) {
            drawEllipse(entity, ellipse);
        }

        for (const auto [entity, arc] :
             registry.view<const ArcShapeComponent>().each()) {
            drawArc(entity, arc);
        }

        for (const auto [entity, curve] :
             registry.view<const CurveShapeComponent>().each()) {
            const auto local = raster::sampleCurve<Vec3>(
                curve.type, curve.controlPoints, curve.order, kCurveSegments);
            const auto stroke = strokeOf(entity);
            auto writer = makeWriter(entity, stroke);
            drawPolyline(toWorld(entity, local), /*closed=*/false,
                         LineAlgorithm::Bresenham, stroke, writer);
        }

        for (const auto [entity, point] :
             registry.view<const PointShapeComponent>().each()) {
            const auto stroke = strokeOf(entity);
            auto writer = makeWriter(entity, stroke);
            const Vec3 world = Vec3(m_scene.worldTransform(entity) *
                                    Vec4(point.position, 1.0F));
            if (const auto screen = m_camera.worldToScreen(world)) {
                writer(raster::toPixel(*screen));
            }
        }
    }

private:
    [[nodiscard]] auto strokeOf(entt::entity entity) const -> StrokeComponent
    {
        const auto* stroke = m_scene.registry().try_get<StrokeComponent>(entity);
        return stroke != nullptr ? *stroke : StrokeComponent{};
    }

    [[nodiscard]] auto makeWriter(entt::entity entity,
                                  const StrokeComponent& stroke)
        -> raster::StrokeWriter
    {
        return {m_color,         m_ids,
                packRgba8(stroke.color), entt::to_integral(entity),
                stroke.thickness, stroke.pattern};
    }

    [[nodiscard]] auto toWorld(entt::entity entity,
                               std::span<const Vec3> local) const
        -> std::vector<Vec3>
    {
        const Mat4 world = m_scene.worldTransform(entity);
        std::vector<Vec3> result;
        result.reserve(local.size());
        for (const Vec3& p : local) {
            result.emplace_back(world * Vec4(p, 1.0F));
        }
        return result;
    }

    /** @brief Clip a world segment to the part in front of the near plane. */
    [[nodiscard]] auto clipNear(Vec3& a, Vec3& b) const -> bool
    {
        const float nearDepth = m_camera.nearClip();
        const float da = m_camera.depthOf(a);
        const float db = m_camera.depthOf(b);
        if (da < nearDepth && db < nearDepth) {
            return false;
        }
        if (da < nearDepth) {
            a = a + (b - a) * ((nearDepth - da) / (db - da));
        } else if (db < nearDepth) {
            b = a + (b - a) * ((nearDepth - da) / (db - da));
        }
        return true;
    }

    void drawSegment(Vec3 a, Vec3 b, LineAlgorithm algorithm,
                     const StrokeComponent& stroke,
                     raster::StrokeWriter& writer) const
    {
        if (!clipNear(a, b)) {
            return;
        }
        const auto screenA = m_camera.worldToScreen(a);
        const auto screenB = m_camera.worldToScreen(b);
        if (!screenA || !screenB) {
            return;
        }
        // Keep a margin so thick strokes near the border are not cut short.
        const float margin = static_cast<float>(stroke.thickness) + 1.0F;
        const Rect clipRect{m_viewport.min - margin, m_viewport.max + margin};
        if (const auto segment =
                raster::clipCohenSutherland(*screenA, *screenB, clipRect)) {
            raster::drawLine(algorithm, raster::toPixel(segment->first),
                             raster::toPixel(segment->second), writer);
        }
    }

    void drawPolyline(std::span<const Vec3> world, bool closed,
                      LineAlgorithm algorithm, const StrokeComponent& stroke,
                      raster::StrokeWriter& writer) const
    {
        if (world.size() < 2) {
            return;
        }
        const std::size_t segments = closed ? world.size() : world.size() - 1;
        for (std::size_t i = 0; i < segments; ++i) {
            drawSegment(world[i], world[(i + 1) % world.size()], algorithm,
                        stroke, writer);
        }
    }

    void fillPolygon(entt::entity entity, const PolygonShapeComponent& polygon)
    {
        if (polygon.vertices.size() < 3) {
            return;
        }
        // Sutherland-Hodgman against the near plane, in world space.
        const auto world = toWorld(entity, polygon.vertices);
        const float nearDepth = m_camera.nearClip();
        std::vector<Vec3> clipped;
        for (std::size_t i = 0; i < world.size(); ++i) {
            const Vec3 current = world[i];
            const Vec3 next = world[(i + 1) % world.size()];
            const float dc = m_camera.depthOf(current) - nearDepth;
            const float dn = m_camera.depthOf(next) - nearDepth;
            if (dc >= 0.0F) {
                clipped.push_back(current);
            }
            if ((dc >= 0.0F) != (dn >= 0.0F)) {
                clipped.push_back(current + (next - current) * (dc / (dc - dn)));
            }
        }

        std::vector<Vec2> screen;
        screen.reserve(clipped.size());
        for (const Vec3& p : clipped) {
            if (const auto s = m_camera.worldToScreen(p)) {
                screen.push_back(*s);
            }
        }
        const auto visible = raster::clipSutherlandHodgman(screen, m_viewport);

        auto writer = makeWriter(entity, StrokeComponent{.color = polygon.fillColor});
        raster::scanlineFill(visible, static_cast<int>(m_color.width()),
                             static_cast<int>(m_color.height()), writer);
    }

    void drawEllipse(entt::entity entity, const EllipseShapeComponent& ellipse)
    {
        const auto stroke = strokeOf(entity);
        auto writer = makeWriter(entity, stroke);
        const Mat4 world = m_scene.worldTransform(entity);

        if (ellipse.algorithm == EllipseAlgorithm::MidPoint) {
            const auto center = m_camera.worldToScreen(Vec3(world[3]));
            const auto xApex = m_camera.worldToScreen(
                Vec3(world * Vec4(ellipse.radiusX, 0.0F, 0.0F, 1.0F)));
            const auto yApex = m_camera.worldToScreen(
                Vec3(world * Vec4(0.0F, ellipse.radiusY, 0.0F, 1.0F)));
            if (center && xApex && yApex) {
                const float rx = glm::distance(*center, *xApex);
                const float ry = glm::distance(*center, *yApex);
                // Huge projected ellipses would iterate millions of pixels;
                // use the (clipped) parametric path for those.
                const float limit = 4.0F * static_cast<float>(std::max(
                                               m_color.width(), m_color.height()));
                if (rx < limit && ry < limit) {
                    raster::midpointEllipse(raster::toPixel(*center),
                                            raster::roundToInt(rx),
                                            raster::roundToInt(ry), writer);
                    return;
                }
            }
        }

        // Parametric (the legacy "Generate" mode fed degrees into cos/sin).
        std::vector<Vec3> local(kEllipseSegments);
        for (std::size_t i = 0; i < kEllipseSegments; ++i) {
            const float angle = kTwoPi * static_cast<float>(i) /
                                static_cast<float>(kEllipseSegments);
            local[i] = {ellipse.radiusX * std::cos(angle),
                        ellipse.radiusY * std::sin(angle), 0.0F};
        }
        drawPolyline(toWorld(entity, local), /*closed=*/true,
                     LineAlgorithm::Bresenham, stroke, writer);
    }

    void drawArc(entt::entity entity, const ArcShapeComponent& arc)
    {
        const auto segments = std::max<std::size_t>(
            8, static_cast<std::size_t>(std::ceil(
                   std::abs(arc.sweepAngle) / kTwoPi *
                   static_cast<float>(kFullCircleArcSegments))));
        std::vector<Vec3> local(segments + 1);
        for (std::size_t i = 0; i <= segments; ++i) {
            const float angle =
                arc.startAngle + arc.sweepAngle * static_cast<float>(i) /
                                     static_cast<float>(segments);
            local[i] = {arc.radius * std::cos(angle),
                        arc.radius * std::sin(angle), 0.0F};
        }
        const auto stroke = strokeOf(entity);
        auto writer = makeWriter(entity, stroke);
        drawPolyline(toWorld(entity, local), /*closed=*/false,
                     LineAlgorithm::Bresenham, stroke, writer);
    }

    const Scene& m_scene;
    const Camera& m_camera;
    ColorImage& m_color;
    IdImage* m_ids;
    Rect m_viewport;
};

}  // namespace

void renderOverlay(const Scene& scene, const Camera& camera, ColorImage& color,
                   IdImage* ids)
{
    if (color.empty()) {
        return;
    }
    SC_ASSERT(ids == nullptr || ids->size() == color.size(),
              "id image must match the color image size");
    OverlayRenderer(scene, camera, color, ids).run();
}

auto pickEntity(const RenderScene& renderScene, const Camera& camera,
                const IdImage* overlayIds, Vec2 pixelPosition, std::uint32_t mask)
    -> std::optional<entt::entity>
{
    const IVec2 pixel = raster::toPixel(pixelPosition);
    if (overlayIds != nullptr && overlayIds->contains(pixel.x, pixel.y)) {
        const std::uint32_t id = overlayIds->at(static_cast<std::uint32_t>(pixel.x),
                                                static_cast<std::uint32_t>(pixel.y));
        if (id != kNoEntityId) {
            return static_cast<entt::entity>(id);
        }
    }
    // Look through window panes (mostly transmissive thin-walled surfaces), like
    // the path tracer's primary entity ids.
    Ray ray = camera.generateRay(pixelPosition);
    for (int layer = 0; layer < 8; ++layer) {
        const auto hit = traceClosestHit(renderScene, ray, 0.0F, kInfinity, mask);
        if (!hit) {
            return std::nullopt;
        }
        const RenderInstance& instance = renderScene.instances[hit->instanceIndex];
        const RenderMaterial& material = renderScene.materials[instance.materialIndex];
        const bool seeThrough = (material.flags & kMaterialThinWalled) != 0U &&
                                material.transmission * (1.0F - material.metallic) >= 0.5F;
        if (!seeThrough) {
            return static_cast<entt::entity>(instance.entityId);
        }
        ray.origin = hit->position + ray.direction * (1e-4F + 1e-4F * hit->t);
    }
    return std::nullopt;
}

}  // namespace starcore
