#include <cstddef>
#include <span>
#include <type_traits>
#include <unordered_map>

#include <glm/geometric.hpp>
#include <glm/matrix.hpp>

#include "starcore/render/render_scene.hpp"
#include "starcore/scene/components.hpp"
#include "starcore/scene/scene.hpp"

namespace starcore
{

namespace
{

constexpr std::uint32_t kNone = ~0U;

auto toRenderMaterial(const MaterialComponent* material) -> RenderMaterial
{
    RenderMaterial result;
    if (material != nullptr) {
        result.albedo = Vec4(material->albedo, 1.0F);
        result.emission = Vec4(material->emission(), 0.0F);
        result.roughness = material->roughness;
        result.metallic = material->metallic;
    }
    return result;
}

void addInstance(RenderScene& out, std::uint32_t geometryIndex, const Mat4& objectToWorld,
                 const MaterialComponent* material, entt::entity entity)
{
    RenderInstance instance;
    instance.objectToWorld = objectToWorld;
    instance.worldToObject = glm::inverse(objectToWorld);
    instance.worldBounds = out.geometries[geometryIndex].localBounds.transformed(objectToWorld);
    instance.geometryIndex = geometryIndex;
    instance.materialIndex = static_cast<std::uint32_t>(out.materials.size());
    instance.entityId = entt::to_integral(entity);
    out.materials.push_back(toRenderMaterial(material));
    out.instances.push_back(instance);
}

/** @brief The ray in an instance's object space (direction NOT renormalized: same t). */
auto toObjectSpace(const RenderInstance& instance, const Ray& ray) -> Ray
{
    return {Vec3(instance.worldToObject * Vec4(ray.origin, 1.0F)),
            Vec3(instance.worldToObject * Vec4(ray.direction, 0.0F))};
}

}  // namespace

auto RenderScene::triangleCount() const -> std::size_t
{
    std::size_t count = 0;
    for (const RenderGeometry& geometry : geometries) {
        if (geometry.mesh) {
            count += geometry.mesh->triangleCount();
        }
    }
    return count;
}

auto buildRenderScene(const Scene& scene) -> RenderScene
{
    RenderScene out;
    out.skyColor = scene.environment().skyColor;
    out.sceneVersion = scene.version();
    const auto& registry = scene.registry();

    // One shared procedural unit sphere; the radius goes into the transform.
    const auto sphereGeometry = static_cast<std::uint32_t>(out.geometries.size());
    out.geometries.push_back(RenderGeometry{
        .kind = GeometryKind::Sphere,
        .localBounds = Aabb{Vec3(-1.0F), Vec3(1.0F)},
    });

    for (const auto [entity, sphere] : registry.view<const SphereComponent>().each()) {
        Mat4 objectToWorld = scene.worldTransform(entity);
        objectToWorld[0] *= sphere.radius;
        objectToWorld[1] *= sphere.radius;
        objectToWorld[2] *= sphere.radius;
        addInstance(out, sphereGeometry, objectToWorld,
                    registry.try_get<MaterialComponent>(entity), entity);
    }

    // Each MeshData becomes one geometry (BLAS), shared by all its instances.
    std::unordered_map<const MeshData*, std::uint32_t> meshGeometries;
    for (const auto [entity, component] : registry.view<const MeshComponent>().each()) {
        if (!component.mesh || component.mesh->triangleCount() == 0) {
            continue;
        }
        const auto [it, inserted] = meshGeometries.try_emplace(
            component.mesh.get(), static_cast<std::uint32_t>(out.geometries.size()));
        if (inserted) {
            out.geometries.push_back(RenderGeometry{
                .kind = GeometryKind::Triangles,
                .mesh = component.mesh,
                .localBounds = component.mesh->bounds,
            });
        }
        addInstance(out, it->second, scene.worldTransform(entity),
                    registry.try_get<MaterialComponent>(entity), entity);
    }

    std::vector<Aabb> instanceBounds;
    instanceBounds.reserve(out.instances.size());
    for (const RenderInstance& instance : out.instances) {
        instanceBounds.push_back(instance.worldBounds);
    }
    // Instance tests are expensive (transform + BLAS): keep TLAS leaves small.
    out.tlas = Bvh::build(instanceBounds, {.maxLeafSize = 2});
    return out;
}

namespace
{

/** @brief FNV-1a over the raw bytes of trivially copyable data. */
class Fnv1a
{
public:
    template <typename T>
    void add(std::span<const T> values)
    {
        static_assert(std::is_trivially_copyable_v<T>);
        const auto bytes = std::as_bytes(values);
        for (const std::byte b : bytes) {
            m_hash = (m_hash ^ static_cast<std::uint64_t>(b)) * 1099511628211ULL;
        }
    }

    template <typename T>
    void add(const T& value)
    {
        add(std::span<const T>(&value, 1));
    }

    [[nodiscard]] auto value() const -> std::uint64_t
    {
        return m_hash;
    }

private:
    std::uint64_t m_hash = 14695981039346656037ULL;
};

}  // namespace

auto hashRenderScene(const RenderScene& scene) -> std::uint64_t
{
    Fnv1a hash;
    for (const RenderGeometry& geometry : scene.geometries) {
        // Meshes are immutable: their identity (plus size/bounds against
        // address reuse) stands for their contents.
        hash.add(geometry.kind);
        hash.add(reinterpret_cast<std::uintptr_t>(geometry.mesh.get()));
        hash.add(geometry.mesh ? geometry.mesh->triangleCount() : std::size_t{0});
        hash.add(geometry.localBounds);
    }
    hash.add(std::span<const RenderInstance>(scene.instances));
    hash.add(std::span<const RenderMaterial>(scene.materials));
    hash.add(scene.skyColor);
    return hash.value();
}

auto traceClosestHit(const RenderScene& scene, const Ray& ray, float tMin, float tMax,
                     std::uint32_t mask) -> std::optional<SurfaceHit>
{
    struct Candidate
    {
        std::uint32_t instance = kNone;
        std::uint32_t triangle = kNone;
        float t = 0.0F;
        float u = 0.0F;
        float v = 0.0F;
        Vec3 objectPoint{0.0F};
    };
    Candidate best;

    scene.tlas.traverse(ray, tMin, tMax, [&](std::uint32_t instanceIndex, float& closest) {
        const RenderInstance& instance = scene.instances[instanceIndex];
        if ((instance.mask & mask) == 0) {
            return;
        }
        const Ray local = toObjectSpace(instance, ray);
        const RenderGeometry& geometry = scene.geometries[instance.geometryIndex];
        if (geometry.kind == GeometryKind::Sphere) {
            if (const auto t = intersectSphere(local, Vec3(0.0F), 1.0F, tMin, closest)) {
                closest = *t;
                best = Candidate{instanceIndex, kNone, *t, 0.0F, 0.0F, local.at(*t)};
            }
            return;
        }
        const MeshData& mesh = *geometry.mesh;
        mesh.bvh.traverse(local, tMin, closest, [&](std::uint32_t triangle, float& t) {
            if (const auto hit = intersectTriangle(local, mesh.vertex(triangle, 0),
                                                   mesh.vertex(triangle, 1),
                                                   mesh.vertex(triangle, 2), tMin, t)) {
                t = hit->t;
                best = Candidate{instanceIndex, triangle, hit->t, hit->u, hit->v, Vec3(0.0F)};
            }
        });
    });

    if (best.instance == kNone) {
        return std::nullopt;
    }

    const RenderInstance& instance = scene.instances[best.instance];
    const RenderGeometry& geometry = scene.geometries[instance.geometryIndex];
    Vec3 objectGeometric = best.objectPoint;  // Unit sphere: normal = point.
    Vec3 objectShading = best.objectPoint;
    if (geometry.kind == GeometryKind::Triangles) {
        const MeshData& mesh = *geometry.mesh;
        const Vec3& v0 = mesh.vertex(best.triangle, 0);
        objectGeometric = glm::cross(mesh.vertex(best.triangle, 1) - v0,
                                     mesh.vertex(best.triangle, 2) - v0);
        objectShading = objectGeometric;
        if (!mesh.normals.empty()) {
            const std::uint32_t* corner = &mesh.indices[std::size_t{best.triangle} * 3];
            objectShading = (1.0F - best.u - best.v) * mesh.normals[corner[0]] +
                            best.u * mesh.normals[corner[1]] + best.v * mesh.normals[corner[2]];
        }
    }

    // Normals transform with the inverse transpose.
    const Mat3 normalMatrix = Mat3(glm::transpose(instance.worldToObject));
    Vec3 geometric = glm::normalize(normalMatrix * objectGeometric);
    Vec3 shading = normalMatrix * objectShading;
    const float shadingLength = glm::length(shading);
    shading = shadingLength > 0.0F ? shading / shadingLength : geometric;

    SurfaceHit hit;
    hit.t = best.t;
    hit.position = ray.at(best.t);
    hit.instanceIndex = best.instance;
    hit.frontFace = glm::dot(ray.direction, geometric) < 0.0F;
    if (!hit.frontFace) {
        geometric = -geometric;
        shading = -shading;
    }
    // Interpolated normals may face away from the ray near silhouettes.
    if (glm::dot(shading, ray.direction) >= 0.0F) {
        shading = geometric;
    }
    hit.geometricNormal = geometric;
    hit.normal = shading;
    return hit;
}

auto traceAnyHit(const RenderScene& scene, const Ray& ray, float tMin, float tMax,
                 std::uint32_t mask) -> bool
{
    return scene.tlas.traverseAny(ray, tMin, tMax, [&](std::uint32_t instanceIndex) {
        const RenderInstance& instance = scene.instances[instanceIndex];
        if ((instance.mask & mask) == 0) {
            return false;
        }
        const Ray local = toObjectSpace(instance, ray);
        const RenderGeometry& geometry = scene.geometries[instance.geometryIndex];
        if (geometry.kind == GeometryKind::Sphere) {
            return intersectSphere(local, Vec3(0.0F), 1.0F, tMin, tMax).has_value();
        }
        const MeshData& mesh = *geometry.mesh;
        return mesh.bvh.traverseAny(local, tMin, tMax, [&](std::uint32_t triangle) {
            return intersectTriangle(local, mesh.vertex(triangle, 0), mesh.vertex(triangle, 1),
                                     mesh.vertex(triangle, 2), tMin, tMax)
                .has_value();
        });
    });
}

}  // namespace starcore
