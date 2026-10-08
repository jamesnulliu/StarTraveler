module;

#include "starcore/common/assert.hpp"

module starcore.asset;

import std;
import glm;
import starcore.math;

namespace starcore
{

auto computeSmoothNormals(std::span<const Vec3> positions,
                          std::span<const std::uint32_t> indices) -> std::vector<Vec3>
{
    std::vector<Vec3> normals(positions.size(), Vec3(0.0F));
    for (std::size_t i = 0; i + 2 < indices.size(); i += 3) {
        const Vec3& a = positions[indices[i]];
        const Vec3& b = positions[indices[i + 1]];
        const Vec3& c = positions[indices[i + 2]];
        const Vec3 areaNormal = glm::cross(b - a, c - a);  // Length = 2 * area.
        for (std::size_t k = 0; k < 3; ++k) {
            normals[indices[i + k]] += areaNormal;
        }
    }
    for (Vec3& normal : normals) {
        const float length = glm::length(normal);
        normal = length > 0.0F ? normal / length : Vec3(0.0F, 1.0F, 0.0F);
    }
    return normals;
}

auto makeMesh(std::vector<Vec3> positions, std::vector<std::uint32_t> indices,
              std::vector<Vec3> normals) -> std::expected<MeshHandle, std::string>
{
    if (indices.size() % 3 != 0) {
        return std::unexpected(
            std::format("index count {} is not a multiple of 3", indices.size()));
    }
    if (const auto it = std::ranges::find_if(
            indices, [&](std::uint32_t index) { return index >= positions.size(); });
        it != indices.end()) {
        return std::unexpected(std::format("index {} out of range ({} vertices)", *it,
                                           positions.size()));
    }
    if (!normals.empty() && normals.size() != positions.size()) {
        return std::unexpected(std::format("{} normals for {} vertices", normals.size(),
                                           positions.size()));
    }

    auto mesh = std::make_shared<MeshData>();
    if (!normals.empty()) {
        std::vector<Vec3> fallback;
        for (std::size_t i = 0; i < normals.size(); ++i) {
            const float length = glm::length(normals[i]);
            if (length > 1e-12F && std::isfinite(length)) {
                normals[i] /= length;
                continue;
            }
            if (fallback.empty()) {
                fallback = computeSmoothNormals(positions, indices);
            }
            normals[i] = fallback[i];
        }
    }
    for (const Vec3& position : positions) {
        mesh->bounds.expand(position);
    }

    std::vector<Aabb> triangleBounds(indices.size() / 3);
    for (std::size_t i = 0; i < triangleBounds.size(); ++i) {
        for (std::size_t k = 0; k < 3; ++k) {
            triangleBounds[i].expand(positions[indices[i * 3 + k]]);
        }
    }
    mesh->bvh = Bvh::build(triangleBounds);
    mesh->positions = std::move(positions);
    mesh->normals = std::move(normals);
    mesh->indices = std::move(indices);
    return MeshHandle(std::move(mesh));
}

// MeshBuilder --------------------------------------------------------------------------------------

auto MeshBuilder::addVertex(Vec3 position, Vec3 normal) -> std::uint32_t
{
    m_positions.push_back(position);
    m_normals.push_back(normal);
    return static_cast<std::uint32_t>(m_positions.size() - 1);
}

void MeshBuilder::addTriangle(std::uint32_t a, std::uint32_t b, std::uint32_t c)
{
    m_indices.insert(m_indices.end(), {a, b, c});
}

void MeshBuilder::addQuad(std::uint32_t a, std::uint32_t b, std::uint32_t c, std::uint32_t d)
{
    m_indices.insert(m_indices.end(), {a, b, c, a, c, d});
}

void MeshBuilder::append(const MeshBuilder& other, const Mat4& transform)
{
    MeshBuilder part = other;
    part.transform(transform);
    const auto base = static_cast<std::uint32_t>(m_positions.size());
    m_positions.insert(m_positions.end(), part.m_positions.begin(), part.m_positions.end());
    m_normals.insert(m_normals.end(), part.m_normals.begin(), part.m_normals.end());
    for (const std::uint32_t index : part.m_indices) {
        m_indices.push_back(base + index);
    }
}

void MeshBuilder::transform(const Mat4& matrix)
{
    const Mat3 normalMatrix = glm::transpose(glm::inverse(Mat3(matrix)));
    for (Vec3& position : m_positions) {
        position = Vec3(matrix * Vec4(position, 1.0F));
    }
    for (Vec3& normal : m_normals) {
        const Vec3 transformed = normalMatrix * normal;
        const float length = glm::length(transformed);
        normal = length > 0.0F ? transformed / length : normal;
    }
    // Mirroring transforms turn counter-clockwise faces clockwise; restore.
    if (glm::determinant(Mat3(matrix)) < 0.0F) {
        for (std::size_t i = 0; i + 2 < m_indices.size(); i += 3) {
            std::swap(m_indices[i + 1], m_indices[i + 2]);
        }
    }
}

void MeshBuilder::flip()
{
    for (std::size_t i = 0; i + 2 < m_indices.size(); i += 3) {
        std::swap(m_indices[i + 1], m_indices[i + 2]);
    }
    for (Vec3& normal : m_normals) {
        normal = -normal;
    }
}

auto MeshBuilder::build() const -> MeshHandle
{
    auto mesh = makeMesh(m_positions, m_indices, m_normals);
    SC_VERIFY(mesh.has_value(), "MeshBuilder produced an invalid mesh: {}", mesh.error());
    return *std::move(mesh);
}

// Primitives -----------------------------------------------------------------------------------------

namespace primitives
{

namespace
{

/** @brief Flat rectangle center +- u +- v facing `normal` (u x v points along it). */
void addFace(MeshBuilder& builder, Vec3 center, Vec3 u, Vec3 v, Vec3 normal)
{
    const std::uint32_t a = builder.addVertex(center - u - v, normal);
    const std::uint32_t b = builder.addVertex(center + u - v, normal);
    const std::uint32_t c = builder.addVertex(center + u + v, normal);
    const std::uint32_t d = builder.addVertex(center - u + v, normal);
    builder.addQuad(a, b, c, d);
}

/** @brief Direction for an azimuth angle around +Y, starting at +X towards -Z. */
auto azimuthDirection(float angle) -> Vec3
{
    return {std::cos(angle), 0.0F, -std::sin(angle)};
}

auto signedArea(std::span<const Vec2> polygon) -> float
{
    float area = 0.0F;
    for (std::size_t i = 0; i < polygon.size(); ++i) {
        const Vec2 a = polygon[i];
        const Vec2 b = polygon[(i + 1) % polygon.size()];
        area += a.x * b.y - b.x * a.y;
    }
    return area * 0.5F;
}

auto cross2(Vec2 a, Vec2 b) -> float
{
    return a.x * b.y - a.y * b.x;
}

/** @brief Ear clipping of a simple counter-clockwise polygon; triangles as index triples. */
auto triangulate(std::span<const Vec2> polygon) -> std::vector<std::uint32_t>
{
    std::vector<std::uint32_t> remaining(polygon.size());
    std::iota(remaining.begin(), remaining.end(), 0U);
    std::vector<std::uint32_t> triangles;

    auto inside = [](Vec2 p, Vec2 a, Vec2 b, Vec2 c) {
        return cross2(b - a, p - a) >= 0.0F && cross2(c - b, p - b) >= 0.0F &&
               cross2(a - c, p - c) >= 0.0F;
    };

    while (remaining.size() > 3) {
        bool clipped = false;
        const std::size_t n = remaining.size();
        for (std::size_t i = 0; i < n && !clipped; ++i) {
            const std::uint32_t prev = remaining[(i + n - 1) % n];
            const std::uint32_t current = remaining[i];
            const std::uint32_t next = remaining[(i + 1) % n];
            const Vec2 a = polygon[prev];
            const Vec2 b = polygon[current];
            const Vec2 c = polygon[next];
            if (cross2(b - a, c - b) <= 0.0F) {
                continue;  // Reflex (or collinear) corner: not an ear.
            }
            const bool blocked = std::ranges::any_of(remaining, [&](std::uint32_t k) {
                return k != prev && k != current && k != next &&
                       inside(polygon[k], a, b, c);
            });
            if (!blocked) {
                triangles.insert(triangles.end(), {prev, current, next});
                remaining.erase(remaining.begin() + static_cast<std::ptrdiff_t>(i));
                clipped = true;
            }
        }
        if (!clipped) {
            break;  // Degenerate input: fan the rest.
        }
    }
    for (std::size_t i = 1; i + 1 < remaining.size(); ++i) {
        triangles.insert(triangles.end(), {remaining[0], remaining[i], remaining[i + 1]});
    }
    return triangles;
}

}  // namespace

auto box(Vec3 h) -> MeshBuilder
{
    MeshBuilder builder;
    addFace(builder, {h.x, 0, 0}, {0, 0, -h.z}, {0, h.y, 0}, {1, 0, 0});
    addFace(builder, {-h.x, 0, 0}, {0, 0, h.z}, {0, h.y, 0}, {-1, 0, 0});
    addFace(builder, {0, h.y, 0}, {h.x, 0, 0}, {0, 0, -h.z}, {0, 1, 0});
    addFace(builder, {0, -h.y, 0}, {h.x, 0, 0}, {0, 0, h.z}, {0, -1, 0});
    addFace(builder, {0, 0, h.z}, {h.x, 0, 0}, {0, h.y, 0}, {0, 0, 1});
    addFace(builder, {0, 0, -h.z}, {-h.x, 0, 0}, {0, h.y, 0}, {0, 0, -1});
    return builder;
}

auto plane(Vec2 size) -> MeshBuilder
{
    MeshBuilder builder;
    addFace(builder, Vec3(0.0F), {size.x * 0.5F, 0, 0}, {0, 0, -size.y * 0.5F}, {0, 1, 0});
    return builder;
}

auto quad(Vec2 size) -> MeshBuilder
{
    MeshBuilder builder;
    addFace(builder, Vec3(0.0F), {size.x * 0.5F, 0, 0}, {0, size.y * 0.5F, 0}, {0, 0, 1});
    return builder;
}

auto sphere(float radius, std::uint32_t segments, std::uint32_t rings, Vec2 polar,
            Vec2 azimuth) -> MeshBuilder
{
    segments = std::max(3U, segments);
    rings = std::max(2U, rings);
    MeshBuilder builder;
    for (std::uint32_t i = 0; i <= rings; ++i) {
        const float theta = polar.x + (polar.y - polar.x) * static_cast<float>(i) /
                                          static_cast<float>(rings);
        for (std::uint32_t j = 0; j <= segments; ++j) {
            const float phi = azimuth.x + (azimuth.y - azimuth.x) * static_cast<float>(j) /
                                              static_cast<float>(segments);
            const Vec3 normal = std::sin(theta) * azimuthDirection(phi) +
                                Vec3(0.0F, std::cos(theta), 0.0F);
            builder.addVertex(normal * radius, normal);
        }
    }
    const std::uint32_t stride = segments + 1;
    constexpr float kPoleEpsilon = 1e-6F;
    for (std::uint32_t i = 0; i < rings; ++i) {
        const float thetaTop = polar.x + (polar.y - polar.x) * static_cast<float>(i) /
                                             static_cast<float>(rings);
        const float thetaBottom = polar.x + (polar.y - polar.x) *
                                                static_cast<float>(i + 1) /
                                                static_cast<float>(rings);
        const bool topIsPole = std::abs(std::sin(thetaTop)) < kPoleEpsilon;
        const bool bottomIsPole = std::abs(std::sin(thetaBottom)) < kPoleEpsilon;
        for (std::uint32_t j = 0; j < segments; ++j) {
            const std::uint32_t a = i * stride + j;
            const std::uint32_t b = (i + 1) * stride + j;
            const std::uint32_t c = (i + 1) * stride + j + 1;
            const std::uint32_t d = i * stride + j + 1;
            if (topIsPole) {
                builder.addTriangle(a, b, c);
            } else if (bottomIsPole) {
                builder.addTriangle(a, b, d);
            } else {
                builder.addQuad(a, b, c, d);
            }
        }
    }
    return builder;
}

auto cylinder(float radius, float height, std::uint32_t segments, bool caps, Vec2 azimuth)
    -> MeshBuilder
{
    segments = std::max(3U, segments);
    const float halfHeight = height * 0.5F;
    const bool fullCircle = std::abs(azimuth.y - azimuth.x) >= kTwoPi - 1e-4F;
    MeshBuilder builder;
    for (std::uint32_t j = 0; j <= segments; ++j) {
        const float phi = azimuth.x + (azimuth.y - azimuth.x) * static_cast<float>(j) /
                                          static_cast<float>(segments);
        const Vec3 normal = azimuthDirection(phi);
        builder.addVertex(normal * radius - Vec3(0, halfHeight, 0), normal);
        builder.addVertex(normal * radius + Vec3(0, halfHeight, 0), normal);
    }
    for (std::uint32_t j = 0; j < segments; ++j) {
        builder.addQuad(2 * j, 2 * (j + 1), 2 * (j + 1) + 1, 2 * j + 1);
    }
    if (caps && fullCircle) {
        for (const float side : {1.0F, -1.0F}) {
            const Vec3 normal{0.0F, side, 0.0F};
            const std::uint32_t center = builder.addVertex(normal * halfHeight, normal);
            const std::uint32_t first = center + 1;
            for (std::uint32_t j = 0; j <= segments; ++j) {
                const float phi = azimuth.x + (azimuth.y - azimuth.x) *
                                                  static_cast<float>(j) /
                                                  static_cast<float>(segments);
                builder.addVertex(azimuthDirection(phi) * radius + normal * halfHeight, normal);
            }
            for (std::uint32_t j = 0; j < segments; ++j) {
                if (side > 0.0F) {
                    builder.addTriangle(center, first + j, first + j + 1);
                } else {
                    builder.addTriangle(center, first + j + 1, first + j);
                }
            }
        }
    }
    return builder;
}

auto torus(float majorRadius, float minorRadius, std::uint32_t segments, std::uint32_t sides)
    -> MeshBuilder
{
    segments = std::max(3U, segments);
    sides = std::max(3U, sides);
    MeshBuilder builder;
    for (std::uint32_t i = 0; i <= segments; ++i) {
        const float phi = kTwoPi * static_cast<float>(i) / static_cast<float>(segments);
        const Vec3 radial = azimuthDirection(phi);
        for (std::uint32_t j = 0; j <= sides; ++j) {
            const float psi = kTwoPi * static_cast<float>(j) / static_cast<float>(sides);
            const Vec3 normal = std::cos(psi) * radial + Vec3(0.0F, std::sin(psi), 0.0F);
            builder.addVertex(radial * majorRadius + normal * minorRadius, normal);
        }
    }
    const std::uint32_t stride = sides + 1;
    for (std::uint32_t i = 0; i < segments; ++i) {
        for (std::uint32_t j = 0; j < sides; ++j) {
            builder.addQuad(i * stride + j, (i + 1) * stride + j, (i + 1) * stride + j + 1,
                            i * stride + j + 1);
        }
    }
    return builder;
}

auto extrude(std::span<const Vec2> profileInput, float depth) -> MeshBuilder
{
    MeshBuilder builder;
    if (profileInput.size() < 3) {
        return builder;
    }
    std::vector<Vec2> profile(profileInput.begin(), profileInput.end());
    if (signedArea(profile) < 0.0F) {
        std::ranges::reverse(profile);
    }
    const float halfDepth = depth * 0.5F;
    const std::size_t n = profile.size();

    // Side walls (flat shaded).
    for (std::size_t i = 0; i < n; ++i) {
        const Vec2 p = profile[i];
        const Vec2 q = profile[(i + 1) % n];
        const Vec2 edge = q - p;
        const float length = glm::length(edge);
        if (length <= 0.0F) {
            continue;
        }
        const Vec3 normal{edge.y / length, -edge.x / length, 0.0F};
        const std::uint32_t a = builder.addVertex({p, -halfDepth}, normal);
        const std::uint32_t b = builder.addVertex({q, -halfDepth}, normal);
        const std::uint32_t c = builder.addVertex({q, halfDepth}, normal);
        const std::uint32_t d = builder.addVertex({p, halfDepth}, normal);
        builder.addQuad(a, b, c, d);
    }

    // Caps.
    const auto triangles = triangulate(profile);
    for (const float side : {1.0F, -1.0F}) {
        const Vec3 normal{0.0F, 0.0F, side};
        const auto base = static_cast<std::uint32_t>(builder.positions().size());
        for (const Vec2 p : profile) {
            builder.addVertex({p, side * halfDepth}, normal);
        }
        for (std::size_t i = 0; i + 2 < triangles.size(); i += 3) {
            if (side > 0.0F) {
                builder.addTriangle(base + triangles[i], base + triangles[i + 1],
                                    base + triangles[i + 2]);
            } else {
                builder.addTriangle(base + triangles[i], base + triangles[i + 2],
                                    base + triangles[i + 1]);
            }
        }
    }
    return builder;
}

}  // namespace primitives

}  // namespace starcore
