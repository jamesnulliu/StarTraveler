module;

#include <entt/entity/registry.hpp>

module starcore.render;

import std;
import glm;
import starcore.asset;
import starcore.math;
import starcore.scene;

namespace starcore
{

namespace
{

constexpr std::uint32_t kNone = ~0U;
// The procedural unit sphere shared by all spheres (radius in the transform).
constexpr std::uint32_t kSphereGeometry = 0;

auto toRenderMaterial(const MaterialComponent* material) -> RenderMaterial
{
    RenderMaterial result;
    if (material != nullptr) {
        result.albedo = Vec4(material->albedo, 1.0F);
        result.emission = Vec4(material->emission(), 0.0F);
        result.roughness = material->roughness;
        result.metallic = material->metallic;
        result.transmission = material->transmission;
        result.ior = material->ior;
        result.flags = material->thinWalled ? kMaterialThinWalled : 0U;
    }
    return result;
}

/** @brief Instance slot key: an entity may have both a sphere and a mesh. */
auto instanceKey(entt::entity entity, GeometryKind kind) -> std::uint64_t
{
    return (static_cast<std::uint64_t>(entt::to_integral(entity)) << 1U) |
           static_cast<std::uint64_t>(kind);
}

/** @brief The ray in an instance's object space (direction NOT renormalized: same t). */
auto toObjectSpace(const RenderInstance& instance, const Ray& ray) -> Ray
{
    return {Vec3(instance.worldToObject * Vec4(ray.origin, 1.0F)),
            Vec3(instance.worldToObject * Vec4(ray.direction, 0.0F))};
}

/**
 * @brief Calls `visit(instanceIndex, tMax)` for the instances whose bounds the
 *        ray enters before tMax: through the TLAS when it is current, else
 *        all of them.
 */
template <typename Visit>
void forEachInstance(const RenderScene& scene, const Ray& ray, float tMin, float& tMax,
                     Visit&& visit)
{
    if (scene.tlasCurrent()) {
        scene.tlas.traverse(ray, tMin, tMax, visit);
        return;
    }
    const Vec3 inverseDirection = 1.0F / ray.direction;
    for (std::uint32_t i = 0; i < scene.instances.size(); ++i) {
        if (rayAabbEntry(ray, inverseDirection, scene.instances[i].worldBounds, tMin, tMax) !=
            kInfinity) {
            visit(i, tMax);
        }
    }
}

/** @brief Whether `intersect(instanceIndex)` holds for an instance the ray enters. */
template <typename Intersect>
auto anyInstance(const RenderScene& scene, const Ray& ray, float tMin, float tMax,
                 Intersect&& intersect) -> bool
{
    if (scene.tlasCurrent()) {
        return scene.tlas.traverseAny(ray, tMin, tMax, intersect);
    }
    const Vec3 inverseDirection = 1.0F / ray.direction;
    for (std::uint32_t i = 0; i < scene.instances.size(); ++i) {
        if (rayAabbEntry(ray, inverseDirection, scene.instances[i].worldBounds, tMin, tMax) !=
                kInfinity &&
            intersect(i)) {
            return true;
        }
    }
    return false;
}

}  // namespace

auto nextRenderStamp() noexcept -> std::uint64_t
{
    static std::atomic<std::uint64_t> counter{0};
    return counter.fetch_add(1, std::memory_order_relaxed) + 1;
}

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

void RenderScene::updateTlas()
{
    if (tlasCurrent()) {
        return;
    }
    std::vector<Aabb> instanceBounds;
    instanceBounds.reserve(instances.size());
    for (const RenderInstance& instance : instances) {
        instanceBounds.push_back(instance.worldBounds);
    }
    // Instance tests are expensive (transform + BLAS): keep TLAS leaves small.
    tlas = Bvh::build(instanceBounds, {.maxLeafSize = 2});
    tlasVersion = version;
}

// RenderSceneBuilder --------------------------------------------------------------------------------

/** @brief What one update changed; all its changes share one stamp. */
struct RenderSceneBuilder::Update
{
    std::uint64_t stamp = 0;
    bool changed = false;         // Anything traced.
    bool contentChanged = false;  // Anything but transforms.
    bool lightsChanged = false;   // Sphere instances: the lights may differ.

    auto stampNow() -> std::uint64_t
    {
        if (stamp == 0) {
            stamp = nextRenderStamp();
        }
        return stamp;
    }
};

auto RenderSceneBuilder::update(const Scene& scene) -> const RenderScene&
{
    if (&scene == m_source && scene.version() == m_sourceVersion && m_scene.version != 0) {
        return m_scene;
    }
    m_source = &scene;
    m_sourceVersion = scene.version();
    ++m_pass;
    Update update;

    if (m_scene.geometries.empty()) {
        m_scene.geometries.push_back(RenderGeometry{
            .kind = GeometryKind::Sphere,
            .localBounds = Aabb{Vec3(-1.0F), Vec3(1.0F)},
        });
        m_geometryUsers.push_back(0);
        update.changed = true;
        update.contentChanged = true;
    }

    const auto& registry = scene.registry();
    for (const auto [entity, sphere] : registry.view<const SphereComponent>().each()) {
        Mat4 objectToWorld = scene.worldTransform(entity);
        objectToWorld[0] *= sphere.radius;
        objectToWorld[1] *= sphere.radius;
        objectToWorld[2] *= sphere.radius;
        syncInstance(update, instanceKey(entity, GeometryKind::Sphere), GeometryKind::Sphere, {},
                     objectToWorld, toRenderMaterial(registry.try_get<MaterialComponent>(entity)),
                     entt::to_integral(entity), layerBit(scene.layerOf(entity)));
    }
    for (const auto [entity, component] : registry.view<const MeshComponent>().each()) {
        if (!component.mesh || component.mesh->triangleCount() == 0) {
            continue;
        }
        syncInstance(update, instanceKey(entity, GeometryKind::Triangles), GeometryKind::Triangles,
                     component.mesh, scene.worldTransform(entity),
                     toRenderMaterial(registry.try_get<MaterialComponent>(entity)),
                     entt::to_integral(entity), layerBit(scene.layerOf(entity)));
    }
    // Instances whose entity, sphere or mesh is gone; from the back, so the
    // instance moved into a removed slot was already seen.
    for (auto index = static_cast<std::uint32_t>(m_scene.instances.size()); index-- > 0;) {
        if (m_seen[index] != m_pass) {
            removeInstance(update, index);
        }
    }

    if (scene.environment().skyColor != m_scene.skyColor) {
        m_scene.skyColor = scene.environment().skyColor;
        update.changed = true;
        update.contentChanged = true;
    }
    if (update.lightsChanged) {
        syncLights(update);
    }
    if (update.changed) {
        m_scene.version = update.stampNow();
    }
    if (update.contentChanged) {
        m_scene.contentVersion = update.stampNow();
    }
    return m_scene;
}

void RenderSceneBuilder::syncInstance(Update& update, std::uint64_t key, GeometryKind kind,
                                      const MeshHandle& mesh, const Mat4& objectToWorld,
                                      const RenderMaterial& material, std::uint32_t entityId,
                                      std::uint32_t mask)
{
    RenderScene& scene = m_scene;
    const auto [slot, inserted] =
        m_instanceOf.try_emplace(key, static_cast<std::uint32_t>(scene.instances.size()));
    const std::uint32_t index = slot->second;
    if (inserted) {
        scene.instances.emplace_back();
        scene.materials.emplace_back();
        scene.lightOfInstance.push_back(-1);
        scene.instanceVersions.push_back(0);
        m_keys.push_back(key);
        m_seen.push_back(0);
    }
    m_seen[index] = m_pass;
    RenderInstance& instance = scene.instances[index];

    std::uint32_t geometry = kSphereGeometry;
    if (kind == GeometryKind::Triangles) {
        if (!inserted && scene.geometries[instance.geometryIndex].mesh == mesh) {
            geometry = instance.geometryIndex;
        } else {
            geometry = acquireGeometry(mesh);
            if (!inserted) {
                releaseGeometry(instance.geometryIndex);
            }
        }
    }

    const bool moved = inserted || instance.objectToWorld != objectToWorld;
    const bool content = inserted || geometry != instance.geometryIndex ||
                         instance.entityId != entityId || instance.mask != mask ||
                         scene.materials[index] != material;
    if (!moved && !content) {
        return;
    }
    if (moved) {
        instance.objectToWorld = objectToWorld;
        instance.worldToObject = glm::inverse(objectToWorld);
    }
    instance.geometryIndex = geometry;
    instance.materialIndex = index;
    instance.entityId = entityId;
    instance.mask = mask;
    instance.worldBounds = scene.geometries[geometry].localBounds.transformed(instance.objectToWorld);
    scene.materials[index] = material;
    scene.instanceVersions[index] = update.stampNow();
    update.changed = true;
    update.contentChanged = update.contentChanged || content;
    update.lightsChanged = update.lightsChanged || kind == GeometryKind::Sphere;
}

void RenderSceneBuilder::removeInstance(Update& update, std::uint32_t index)
{
    RenderScene& scene = m_scene;
    const std::uint32_t geometry = scene.instances[index].geometryIndex;
    if (geometry != kSphereGeometry) {
        releaseGeometry(geometry);
    }
    m_instanceOf.erase(m_keys[index]);
    const auto last = static_cast<std::uint32_t>(scene.instances.size() - 1);
    if (index != last) {
        scene.instances[index] = scene.instances[last];
        scene.instances[index].materialIndex = index;
        scene.materials[index] = scene.materials[last];
        scene.lightOfInstance[index] = scene.lightOfInstance[last];
        scene.instanceVersions[index] = update.stampNow();  // Another instance now.
        m_keys[index] = m_keys[last];
        m_seen[index] = m_seen[last];
        m_instanceOf[m_keys[index]] = index;
    }
    scene.instances.pop_back();
    scene.materials.pop_back();
    scene.lightOfInstance.pop_back();
    scene.instanceVersions.pop_back();
    m_keys.pop_back();
    m_seen.pop_back();
    update.changed = true;
    update.contentChanged = true;
    update.lightsChanged = true;
}

void RenderSceneBuilder::syncLights(Update& update)
{
    RenderScene& scene = m_scene;
    // Emissive, (nearly) uniformly scaled spheres are sampled as lights.
    std::vector<RenderLight> lights;
    for (std::uint32_t i = 0; i < scene.instances.size(); ++i) {
        const RenderInstance& instance = scene.instances[i];
        if (instance.geometryIndex != kSphereGeometry) {
            continue;
        }
        const Vec3 emission(scene.materials[i].emission);
        const Mat4& m = instance.objectToWorld;
        const Vec3 axes{glm::length(Vec3(m[0])), glm::length(Vec3(m[1])), glm::length(Vec3(m[2]))};
        const float radius = std::max({axes.x, axes.y, axes.z});
        const bool uniform = radius > 0.0F && std::min({axes.x, axes.y, axes.z}) > 0.999F * radius;
        if (uniform && std::max({emission.r, emission.g, emission.b}) > 0.0F) {
            lights.push_back(RenderLight{
                .center = Vec3(m[3]),
                .radius = radius,
                .emission = emission,
                .instanceIndex = i,
            });
        }
    }
    if (lights != scene.lights) {
        scene.lights = std::move(lights);
        scene.lightsVersion = update.stampNow();
        update.changed = true;
    }
    // Light indices are part of the instances (their GPU records).
    std::vector<std::int32_t> lightOfInstance(scene.instances.size(), -1);
    for (std::size_t i = 0; i < scene.lights.size(); ++i) {
        lightOfInstance[scene.lights[i].instanceIndex] = static_cast<std::int32_t>(i);
    }
    for (std::size_t i = 0; i < lightOfInstance.size(); ++i) {
        if (scene.lightOfInstance[i] != lightOfInstance[i]) {
            scene.lightOfInstance[i] = lightOfInstance[i];
            scene.instanceVersions[i] = update.stampNow();
            update.changed = true;
        }
    }
}

auto RenderSceneBuilder::acquireGeometry(const MeshHandle& mesh) -> std::uint32_t
{
    const auto [slot, inserted] = m_geometryOf.try_emplace(mesh.get(), 0U);
    if (inserted) {
        std::uint32_t index = 0;
        if (!m_freeGeometries.empty()) {
            index = m_freeGeometries.back();
            m_freeGeometries.pop_back();
        } else {
            index = static_cast<std::uint32_t>(m_scene.geometries.size());
            m_scene.geometries.emplace_back();
            m_geometryUsers.push_back(0);
        }
        m_scene.geometries[index] = RenderGeometry{
            .kind = GeometryKind::Triangles,
            .mesh = mesh,
            .localBounds = mesh->bounds,
        };
        slot->second = index;
    }
    ++m_geometryUsers[slot->second];
    return slot->second;
}

void RenderSceneBuilder::releaseGeometry(std::uint32_t index)
{
    if (--m_geometryUsers[index] > 0) {
        return;
    }
    m_geometryOf.erase(m_scene.geometries[index].mesh.get());
    m_scene.geometries[index] = RenderGeometry{};
    m_freeGeometries.push_back(index);
}

auto RenderSceneBuilder::take() -> RenderScene
{
    RenderScene scene = std::move(m_scene);
    *this = RenderSceneBuilder{};
    return scene;
}

auto buildRenderScene(const Scene& scene) -> RenderScene
{
    RenderSceneBuilder builder;
    builder.update(scene);
    builder.updateTlas();
    return builder.take();
}

// Queries ---------------------------------------------------------------------------------------------

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

    forEachInstance(scene, ray, tMin, tMax, [&](std::uint32_t instanceIndex, float& closest) {
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
    return anyInstance(scene, ray, tMin, tMax, [&](std::uint32_t instanceIndex) {
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
