#include <glm/geometric.hpp>
#include <glm/matrix.hpp>

#include "starcore/render/render_scene.hpp"
#include "starcore/scene/components.hpp"
#include "starcore/scene/scene.hpp"

namespace starcore
{

namespace
{

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

void addInstance(RenderScene& out, std::uint32_t geometryIndex,
                 const Mat4& objectToWorld, const MaterialComponent* material,
                 entt::entity entity)
{
    RenderInstance instance;
    instance.objectToWorld = objectToWorld;
    instance.worldToObject = glm::inverse(objectToWorld);
    instance.worldBounds =
        out.geometries[geometryIndex].localBounds.transformed(objectToWorld);
    instance.geometryIndex = geometryIndex;
    instance.materialIndex = static_cast<std::uint32_t>(out.materials.size());
    instance.entityId = entt::to_integral(entity);
    out.materials.push_back(toRenderMaterial(material));
    out.instances.push_back(instance);
}

}  // namespace

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

    for (const auto [entity, sphere] :
         registry.view<const SphereComponent>().each()) {
        Mat4 objectToWorld = scene.worldTransform(entity);
        objectToWorld[0] *= sphere.radius;
        objectToWorld[1] *= sphere.radius;
        objectToWorld[2] *= sphere.radius;
        addInstance(out, sphereGeometry, objectToWorld,
                    registry.try_get<MaterialComponent>(entity), entity);
    }

    for (const auto [entity, mesh] :
         registry.view<const MeshComponent>().each()) {
        const std::size_t triangleCount = mesh.indices.size() / 3;
        if (triangleCount == 0 || mesh.positions.empty()) {
            continue;
        }
        RenderGeometry geometry;
        geometry.kind = GeometryKind::Triangles;
        geometry.firstIndex = static_cast<std::uint32_t>(out.indices.size());
        geometry.indexCount = static_cast<std::uint32_t>(triangleCount * 3);

        const auto baseVertex = static_cast<std::uint32_t>(out.positions.size());
        for (const Vec3& position : mesh.positions) {
            out.positions.push_back(position);
            geometry.localBounds.expand(position);
        }
        bool valid = true;
        for (std::size_t i = 0; i < triangleCount * 3; ++i) {
            if (mesh.indices[i] >= mesh.positions.size()) {
                valid = false;
                break;
            }
            out.indices.push_back(baseVertex + mesh.indices[i]);
        }
        if (!valid) {
            // Drop malformed meshes instead of reading out of bounds.
            out.indices.resize(geometry.firstIndex);
            out.positions.resize(baseVertex);
            continue;
        }

        const auto geometryIndex = static_cast<std::uint32_t>(out.geometries.size());
        out.geometries.push_back(geometry);
        addInstance(out, geometryIndex, scene.worldTransform(entity),
                    registry.try_get<MaterialComponent>(entity), entity);
    }
    return out;
}

auto traceClosestHit(const RenderScene& scene, const Ray& ray, float tMin,
                     float tMax) -> std::optional<SurfaceHit>
{
    const Vec3 inverseDirection = 1.0F / ray.direction;
    std::optional<SurfaceHit> closest;
    Vec3 closestObjectNormal{0.0F};

    for (std::uint32_t i = 0; i < scene.instances.size(); ++i) {
        const RenderInstance& instance = scene.instances[i];
        if (!intersectAabb(ray, inverseDirection, instance.worldBounds, tMin,
                           tMax)) {
            continue;
        }
        // Object-space ray; the direction is NOT renormalized so that t is
        // the same in both spaces.
        const Ray local{Vec3(instance.worldToObject * Vec4(ray.origin, 1.0F)),
                        Vec3(instance.worldToObject * Vec4(ray.direction, 0.0F))};
        const RenderGeometry& geometry = scene.geometries[instance.geometryIndex];

        if (geometry.kind == GeometryKind::Sphere) {
            if (const auto t = intersectSphere(local, Vec3(0.0F), 1.0F, tMin, tMax)) {
                tMax = *t;
                closest = SurfaceHit{.t = *t, .instanceIndex = i};
                closestObjectNormal = local.at(*t);
            }
            continue;
        }

        const std::uint32_t end = geometry.firstIndex + geometry.indexCount;
        for (std::uint32_t k = geometry.firstIndex; k < end; k += 3) {
            const Vec3& v0 = scene.positions[scene.indices[k]];
            const Vec3& v1 = scene.positions[scene.indices[k + 1]];
            const Vec3& v2 = scene.positions[scene.indices[k + 2]];
            if (const auto hit = intersectTriangle(local, v0, v1, v2, tMin, tMax)) {
                tMax = hit->t;
                closest = SurfaceHit{.t = hit->t, .instanceIndex = i};
                closestObjectNormal = glm::cross(v1 - v0, v2 - v0);
            }
        }
    }

    if (closest.has_value()) {
        const RenderInstance& instance = scene.instances[closest->instanceIndex];
        // Normals transform with the inverse transpose.
        Vec3 normal = glm::normalize(
            Mat3(glm::transpose(instance.worldToObject)) * closestObjectNormal);
        closest->position = ray.at(closest->t);
        closest->frontFace = glm::dot(ray.direction, normal) < 0.0F;
        closest->normal = closest->frontFace ? normal : -normal;
    }
    return closest;
}

}  // namespace starcore
