#pragma once
// Plain-data components. An "entity type" is just a combination of these
// (e.g. a ray-traced ball = Transform + Sphere + Material; a drawn line =
// Transform + LineShape + Stroke). Systems (renderers, serializer, editor)
// operate on views over the components they care about.

#include <cstdint>
#include <string>
#include <vector>

#include <entt/entity/entity.hpp>

#include "starcore/math/transform.hpp"
#include "starcore/math/types.hpp"
#include "starcore/raster/types.hpp"
#include "starcore/scene/uuid.hpp"

namespace starcore
{

// Identity -----------------------------------------------------------------------------------------

struct IdComponent
{
    Uuid id;
};

struct NameComponent
{
    std::string name;
};

// Spatial ------------------------------------------------------------------------------------------

/** @brief Local transform (relative to the parent, if any). */
struct TransformComponent
{
    Vec3 translation{0.0F};
    Quat rotation{1.0F, 0.0F, 0.0F, 0.0F};
    Vec3 scale{1.0F};

    [[nodiscard]] auto matrix() const -> Mat4
    {
        return composeTransform(translation, rotation, scale);
    }
};

/** @brief Parent/child links; maintained by Scene::setParent(). */
struct HierarchyComponent
{
    entt::entity parent{entt::null};
    std::vector<entt::entity> children;
};

// Ray-traced geometry ------------------------------------------------------------------------------

struct MaterialComponent
{
    Vec3 albedo{0.8F};
    float roughness = 1.0F;
    float metallic = 0.0F;
    Vec3 emissionColor{0.0F};
    float emissionPower = 0.0F;

    [[nodiscard]] auto emission() const -> Vec3
    {
        return emissionColor * emissionPower;
    }
};

/** @brief Sphere centered at the entity origin (scaled by its transform). */
struct SphereComponent
{
    float radius = 1.0F;
};

/** @brief Triangle list in local space (rendered double-sided). */
struct MeshComponent
{
    std::vector<Vec3> positions;
    std::vector<std::uint32_t> indices;
};

// 2D overlay shapes --------------------------------------------------------------------------------
// Points are in local space; shapes are projected with the camera and
// rasterized in screen space by the algorithms in `starcore/raster`.

struct StrokeComponent
{
    Vec4 color{1.0F};
    std::uint32_t thickness = 1;
    LinePattern pattern = LinePattern::Solid;
};

struct LineShapeComponent
{
    Vec3 start{0.0F};
    Vec3 end{1.0F, 0.0F, 0.0F};
    LineAlgorithm algorithm = LineAlgorithm::Bresenham;
};

/** @brief Ellipse in the local XY plane, centered at the origin. */
struct EllipseShapeComponent
{
    float radiusX = 1.0F;
    float radiusY = 0.5F;
    EllipseAlgorithm algorithm = EllipseAlgorithm::Parametric;
};

/**
 * @brief Circular arc in the local XY plane, centered at the origin.
 *        Angles are in radians; a negative sweep goes clockwise.
 */
struct ArcShapeComponent
{
    float radius = 1.0F;
    float startAngle = 0.0F;
    float sweepAngle = kPi / 2.0F;
};

struct CurveShapeComponent
{
    std::vector<Vec3> controlPoints;
    CurveType type = CurveType::BSpline;
    // B-spline order (degree + 1); clamped to the number of control points.
    std::uint32_t order = 3;
};

/** @brief Filled polygon (scan-line fill); outline uses StrokeComponent. */
struct PolygonShapeComponent
{
    std::vector<Vec3> vertices;
    Vec4 fillColor{1.0F};
    bool filled = true;
};

/** @brief A single marker point (size/color from StrokeComponent). */
struct PointShapeComponent
{
    Vec3 position{0.0F};  // Local space.
};

// Gameplay -----------------------------------------------------------------------------------------

/**
 * @brief A rectangular portal in the local XY plane (normal = local +Z).
 *        Walking through it teleports to the linked portal.
 */
struct PortalComponent
{
    Vec2 size{1.0F, 2.0F};
    Uuid linkedPortal{0};
};

}  // namespace starcore
