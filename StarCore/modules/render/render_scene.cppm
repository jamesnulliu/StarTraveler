// Flattened, render-ready copy of a Scene: what the renderers read instead of
// the ECS (world matrices instead of a hierarchy, materials packed as the GPU
// reads them, nothing but traced geometry).
//
// The layout is the one of Vulkan ray tracing:
//   - geometries  ~ BLAS (a shared MeshData with its BVH, or the procedural
//                   unit sphere = AABB [-1, 1]^3),
//   - instances   ~ TLAS entries (object<->world matrices, geometry/material/
//                   entity ids and an 8-bit visibility mask),
//   - tlas        ~ the top-level acceleration structure over the instances
//                   (the CPU's; the GPU builds its own),
//   - materials   ~ a storage buffer indexed by instance.
// The CPU and GPU renderers consume exactly this data, so their images can be
// compared.
//
// RenderSceneBuilder keeps one up to date incrementally: every entity keeps its
// instance slot, and each change is stamped, so the GPU uploads only what
// changed since its last upload.

export module starcore.render:render_scene;

import std;
import starcore.asset;
import starcore.math;
import starcore.scene;

export namespace starcore
{

/** @brief Ray/instance mask matching every instance (Vulkan cull masks are 8 bits). */
inline constexpr std::uint32_t kAllInstances = 0xFFU;

/** @brief RenderMaterial::flags bits. */
inline constexpr std::uint32_t kMaterialThinWalled = 1U << 0U;

struct RenderMaterial
{
    Vec4 albedo{0.8F, 0.8F, 0.8F, 1.0F};
    Vec4 emission{0.0F};  // rgb = color * power
    float roughness = 1.0F;
    float metallic = 0.0F;
    float transmission = 0.0F;
    float ior = 1.5F;
    std::uint32_t flags = 0;
    std::uint32_t padding[3] = {};

    auto operator==(const RenderMaterial&) const -> bool = default;
};
static_assert(sizeof(RenderMaterial) == 64, "std430-compatible layout");

/**
 * @brief An emissive sphere sampled directly by the path tracer (next event
 *        estimation). Other emitters (meshes, ellipsoids) are only found by
 *        BSDF sampling.
 */
struct RenderLight
{
    Vec3 center{0.0F};
    float radius = 0.0F;
    Vec3 emission{0.0F};
    std::uint32_t instanceIndex = 0;

    auto operator==(const RenderLight&) const -> bool = default;
};

enum class GeometryKind : std::uint32_t
{
    Triangles = 0,
    Sphere = 1,  // Unit sphere at the origin.
};

struct RenderGeometry
{
    GeometryKind kind = GeometryKind::Triangles;
    MeshHandle mesh;  // Triangles only; null: an unused slot.
    Aabb localBounds;
};

struct RenderInstance
{
    Mat4 objectToWorld{1.0F};
    Mat4 worldToObject{1.0F};
    Aabb worldBounds;
    std::uint32_t geometryIndex = 0;
    std::uint32_t materialIndex = 0;
    std::uint32_t entityId = 0;  // entt::to_integral of the source entity.
    std::uint32_t mask = kAllInstances;
};

/**
 * @brief A new change stamp: one counter for all render scenes, so equal
 *        stamps mean equal data (never 0).
 */
[[nodiscard]] auto nextRenderStamp() noexcept -> std::uint64_t;

/**
 * @brief Identity of a RenderScene's history of changes, for consumers that
 *        upload only what changed since they last looked (GpuScene). A copy
 *        starts a history of its own -- changed apart from the original, it
 *        must not pass for it -- and a move hands it over.
 */
class RenderSceneLineage
{
public:
    RenderSceneLineage() noexcept : m_value(nextRenderStamp()) {}
    ~RenderSceneLineage() = default;
    RenderSceneLineage(const RenderSceneLineage& /*other*/) noexcept : m_value(nextRenderStamp())
    {
    }
    RenderSceneLineage(RenderSceneLineage&& other) noexcept
        : m_value(std::exchange(other.m_value, nextRenderStamp()))
    {
    }
    auto operator=(const RenderSceneLineage& other) noexcept -> RenderSceneLineage&
    {
        if (this != &other) {
            m_value = nextRenderStamp();
        }
        return *this;
    }
    auto operator=(RenderSceneLineage&& other) noexcept -> RenderSceneLineage&
    {
        if (this != &other) {
            m_value = std::exchange(other.m_value, nextRenderStamp());
        }
        return *this;
    }

    [[nodiscard]] auto value() const noexcept -> std::uint64_t
    {
        return m_value;
    }

private:
    std::uint64_t m_value;
};

struct RenderScene
{
    // Slot 0 is the unit sphere; each mesh in use has one slot (freed slots
    // have no mesh and are reused).
    std::vector<RenderGeometry> geometries;
    // One slot per (entity, geometry kind), kept while the entity exists;
    // removing one moves the last instance into its slot.
    std::vector<RenderInstance> instances;
    std::vector<RenderMaterial> materials;  // materials[i] is instances[i]'s.
    std::vector<RenderLight> lights;
    // Per instance: index into `lights`, or -1.
    std::vector<std::int32_t> lightOfInstance;
    Vec3 skyColor{0.0F};

    // Change tracking (RenderSceneBuilder): stamps of nextRenderStamp(), 0
    // for never. Equal stamps mean equal data, also across copies.
    RenderSceneLineage lineage;
    std::uint64_t version = 0;         // Last change of anything traced.
    std::uint64_t contentVersion = 0;  // Of anything but the instances' transforms.
    std::uint64_t lightsVersion = 0;   // Of `lights`.
    std::vector<std::uint64_t> instanceVersions;  // Per instance: its last change.

    // The CPU's TLAS over instances[i].worldBounds, as of tlasVersion (~0:
    // never built); the queries below test every instance while it is out of
    // date.
    Bvh tlas;
    std::uint64_t tlasVersion = ~0ULL;

    /** @brief Rebuild the TLAS if the instances changed since it was built. */
    void updateTlas();
    [[nodiscard]] auto tlasCurrent() const noexcept -> bool
    {
        return tlasVersion == version;
    }

    [[nodiscard]] auto triangleCount() const -> std::size_t;
};

/**
 * @brief Keeps a RenderScene in line with a Scene, incrementally: each entity
 *        with a SphereComponent or MeshComponent owns an instance slot (world
 *        transform, MaterialComponent or a default material, layer mask), and
 *        an update rewrites and stamps only the slots whose data changed.
 *
 * Updates compare every renderable entity with its slot (cheap next to a
 * rebuild: no allocations, no BVH, no full upload); while Scene::version()
 * stays the same they do nothing. Follows one Scene at a time.
 */
class RenderSceneBuilder
{
public:
    /** @return The updated render scene (valid until the next update or take). */
    auto update(const Scene& scene) -> const RenderScene&;
    /** @brief Build the CPU TLAS if out of date (for CPU path tracing). */
    void updateTlas()
    {
        m_scene.updateTlas();
    }

    [[nodiscard]] auto renderScene() const noexcept -> const RenderScene&
    {
        return m_scene;
    }
    /** @brief Hand over the render scene; the builder starts over. */
    [[nodiscard]] auto take() -> RenderScene;

private:
    struct Update;

    void syncInstance(Update& update, std::uint64_t key, GeometryKind kind, const MeshHandle& mesh,
                      const Mat4& objectToWorld, const RenderMaterial& material,
                      std::uint32_t entityId, std::uint32_t mask);
    void removeInstance(Update& update, std::uint32_t index);
    void syncLights(Update& update);
    [[nodiscard]] auto acquireGeometry(const MeshHandle& mesh) -> std::uint32_t;
    void releaseGeometry(std::uint32_t index);

    RenderScene m_scene;
    const Scene* m_source = nullptr;
    std::uint64_t m_sourceVersion = 0;
    std::uint64_t m_pass = 0;  // Update count, to find instances whose entity is gone.

    // Instance slots: key (entity, kind) <-> index.
    std::unordered_map<std::uint64_t, std::uint32_t> m_instanceOf;
    std::vector<std::uint64_t> m_keys;
    std::vector<std::uint64_t> m_seen;  // The update that last saw each instance.

    // Geometry slots of meshes and their users.
    std::unordered_map<const MeshData*, std::uint32_t> m_geometryOf;
    std::vector<std::uint32_t> m_geometryUsers;
    std::vector<std::uint32_t> m_freeGeometries;
};

/**
 * @brief A RenderScene of `scene` with its TLAS (one-off; see
 *        RenderSceneBuilder to follow a changing scene).
 */
[[nodiscard]] auto buildRenderScene(const Scene& scene) -> RenderScene;

struct SurfaceHit
{
    float t = 0.0F;
    Vec3 position{0.0F};
    // Shading normal (interpolated vertex normals when the mesh has them),
    // unit length, on the side of the incoming ray (surfaces are two-sided).
    Vec3 normal{0.0F};
    // Geometric (face) normal, unit length, on the side of the incoming ray.
    Vec3 geometricNormal{0.0F};
    // The ray hit the side the face/outward normal points to.
    bool frontFace = true;
    std::uint32_t instanceIndex = 0;
};

/**
 * @brief Closest hit in (tMin, tMax) among instances whose mask intersects
 *        `mask` -- the CPU equivalent of a Vulkan ray query (TLAS -> BLAS;
 *        without a current TLAS every instance's bounds are tested, fine for
 *        a few rays such as picking).
 */
[[nodiscard]] auto traceClosestHit(const RenderScene& scene, const Ray& ray, float tMin,
                                   float tMax, std::uint32_t mask = kAllInstances)
    -> std::optional<SurfaceHit>;

/** @brief Whether anything is hit in (tMin, tMax) (shadow rays). */
[[nodiscard]] auto traceAnyHit(const RenderScene& scene, const Ray& ray, float tMin,
                               float tMax, std::uint32_t mask = kAllInstances) -> bool;

}  // namespace starcore
