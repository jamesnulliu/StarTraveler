#pragma once
// A RenderScene on the GPU: storage buffers with the per-instance data
// (records, materials, transforms), the lights and the meshes' vertex and
// index buffers; with ray queries also one bottom-level acceleration structure
// (BLAS) per mesh, kept across frames, and a top-level one (TLAS) over the
// instances. Layouts in shaders/scene.glsl and hybrid.glsl.
//
// Updates are incremental: while the render scene is the one of the previous
// update (its lineage, see RenderSceneBuilder), only the instances stamped
// since are uploaded.

#include <array>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

#include <vulkan/vulkan.h>

#include "starcore/render/render_scene.hpp"
#include "starcore/vulkan/resources.hpp"

namespace starcore::vulkan
{
class Context;
}

namespace starcore::gpu
{

inline constexpr std::uint32_t kFramesInFlight = 2;

/** @brief Per-instance shader data (`Instance` in scene.glsl). */
struct InstanceRecord
{
    std::uint64_t positions = 0;  // Buffer device addresses (0 = none).
    std::uint64_t normals = 0;
    std::uint64_t indices = 0;
    std::uint32_t materialIndex = 0;
    std::uint32_t entityId = 0;
    std::int32_t lightIndex = -1;
    std::uint32_t kind = 0;  // GeometryKind.
};
static_assert(sizeof(InstanceRecord) == 40, "std430 layout of Instance in scene.glsl");

/**
 * @brief Per-instance transforms for rasterization and motion vectors
 *        (`InstanceTransform` in hybrid.glsl): rows of 3x4 matrices.
 */
struct InstanceTransformRecord
{
    Vec4 objectToWorld[3];
    Vec4 worldToObject[3];
};
static_assert(sizeof(InstanceTransformRecord) == 96,
              "std430 layout of InstanceTransform in hybrid.glsl");

/**
 * @brief A light as the shaders sample it (`Light` in scene.glsl): a
 *        RenderLight with its instance's transform and the address of its
 *        triangle distribution.
 */
struct LightRecord
{
    Vec3 center{0.0F};
    float radius = 0.0F;
    Vec3 emission{0.0F};
    std::uint32_t instanceIndex = 0;
    std::uint64_t triangleCdf = 0;  // Buffer device address (0: a sphere).
    std::uint32_t triangleCount = 0;
    float area = 0.0F;
    Vec4 objectToWorld[3];  // Rows.
};
static_assert(sizeof(LightRecord) == 96, "std430 layout of Light in scene.glsl");

/** @brief A planet as the shaders draw it (`Planet` in planet.glsl): a RenderPlanet. */
struct PlanetRecord
{
    Vec4 frame[3] = {};  // Rows of RenderPlanet::worldToLocal.
    Vec3 origin{0.0F};   // planetOrigin.
    std::uint32_t kind = 0;
    Vec3 sun{0.0F, 1.0F, 0.0F};  // The sun's direction in the planet's frame.
    std::uint32_t seed = 0;
    Vec3 sunIrradiance{0.0F};
    float atmosphereHeight = 0.0F;
    Vec3 atmosphere{0.0F};  // planetAtmosphere.
    float haze = 0.0F;
    Vec3 color{0.0F};
    float ringInner = 0.0F;
    Vec3 secondaryColor{0.0F};
    float ringOuter = 0.0F;
    Vec3 ringColor{0.0F};
    float ringOpacity = 0.0F;
};
static_assert(sizeof(PlanetRecord) == 160, "std430 layout of Planet in planet.glsl");

/**
 * @brief The sky as the shaders look it up (`SkyRecord` in sky.glsl): RenderScene's
 *        skyColor and space sky.
 */
struct SkyRecord
{
    Vec4 color{0.0F};          // rgb: constant radiance; w: the map's brightness.
    Vec4 ambient{0.0F};        // rgb: mean radiance over all directions (skyAmbient).
    Vec4 galactic[3] = {};     // Rows of the rotation from world to galactic directions.
    std::uint64_t map = 0;     // Buffer device address of the map's texels (0: none).
    std::uint32_t width = 0;   // Of the map.
    std::uint32_t height = 0;
    std::uint32_t planetCount = 0;
    std::uint32_t padding[3] = {};
    PlanetRecord planets[kMaxPlanets] = {};  // RenderSky::planets, farthest first.
};
static_assert(sizeof(SkyRecord) == 752, "std430 layout of SkyRecord in sky.glsl");

/** @brief An acceleration structure and the buffer holding it. */
struct AccelerationStructure
{
    vulkan::Buffer storage;
    VkAccelerationStructureKHR handle = VK_NULL_HANDLE;
    VkDeviceAddress address = 0;
};

class GpuScene
{
public:
    /** @brief Acceleration structures are built if the device has ray queries. */
    explicit GpuScene(vulkan::Context& context);
    ~GpuScene();
    GpuScene(const GpuScene&) = delete;
    auto operator=(const GpuScene&) -> GpuScene& = delete;

    /**
     * @brief Make `scene` the current scene; once per frame.
     *
     * First the transforms of the previous update become the previous
     * transforms. Then the instances of `scene` stamped since the previous
     * update are uploaded -- all of them if `scene` has another lineage.
     * Meshes seen for the first time are uploaded (and get their BLAS) right
     * away, blocking; the rest goes through the staging buffer of `frameSlot`:
     * copies and the TLAS build are recorded into `commandBuffer`, followed by
     * a barrier for vertex, fragment and compute shaders.
     *
     * @param frameSlot Frame in flight whose previous submission has completed.
     * @param frameNumber Increases by one per submitted frame (resource retirement).
     */
    void update(const RenderScene& scene, VkCommandBuffer commandBuffer,
                std::uint32_t frameSlot, std::uint64_t frameNumber);

    /** @brief Destroy resources replaced at least kFramesInFlight frames ago. */
    void collectGarbage(std::uint64_t completedFrameNumber);

    /** @brief Whether acceleration structures are built (ray queries). */
    [[nodiscard]] auto rayTracing() const noexcept -> bool
    {
        return m_rayTracing;
    }
    /** @brief VK_NULL_HANDLE without ray tracing. */
    [[nodiscard]] auto tlas() const noexcept -> VkAccelerationStructureKHR
    {
        return m_tlas.handle;
    }
    [[nodiscard]] auto instanceBuffer() const noexcept -> VkBuffer
    {
        return m_instances.handle();
    }
    [[nodiscard]] auto materialBuffer() const noexcept -> VkBuffer
    {
        return m_materials.handle();
    }
    [[nodiscard]] auto lightBuffer() const noexcept -> VkBuffer
    {
        return m_lights.handle();
    }
    [[nodiscard]] auto lightCount() const noexcept -> std::uint32_t
    {
        return m_lightCount;
    }
    /** @brief InstanceTransformRecord per instance. */
    [[nodiscard]] auto transformBuffer() const noexcept -> VkBuffer
    {
        return m_transforms.handle();
    }
    /**
     * @brief The transforms as of the previous update (for motion vectors);
     *        meaningless for instances that are new since or of another lineage.
     */
    [[nodiscard]] auto previousTransformBuffer() const noexcept -> VkBuffer
    {
        return m_previousTransforms.handle();
    }
    /** @brief Buffer device address of the sky's SkyRecord (sky.glsl). */
    [[nodiscard]] auto skyAddress() const noexcept -> VkDeviceAddress
    {
        return m_skyRecordAddress;
    }
    /** @brief Meshes on the GPU (cached across updates). */
    [[nodiscard]] auto meshCount() const noexcept -> std::size_t
    {
        return m_meshes.size();
    }
    /** @brief Instances uploaded by the last update. */
    [[nodiscard]] auto uploadedInstances() const noexcept -> std::uint32_t
    {
        return m_uploadedInstances;
    }

private:
    struct MeshEntry
    {
        MeshHandle mesh;  // Keeps the address (the cache key) from being reused.
        vulkan::Buffer positions;
        vulkan::Buffer normals;
        vulkan::Buffer indices;
        AccelerationStructure blas;
        std::uint64_t lastUsed = 0;  // Mesh sync counter.
    };

    struct Retired
    {
        vulkan::Buffer buffer;
        VkAccelerationStructureKHR accelerationStructure = VK_NULL_HANDLE;
        std::uint64_t frameNumber = 0;
    };

    void buildSphereBlas();
    /** @brief Upload new meshes, drop those unused for a while. */
    void syncMeshes(const RenderScene& scene);
    void buildMeshes(const std::vector<MeshHandle>& meshes);
    /** @return Whether the per-instance buffers were replaced (their contents lost). */
    auto reserveInstances(std::uint32_t count) -> bool;
    void retire(vulkan::Buffer buffer, VkAccelerationStructureKHR accelerationStructure);
    void recordTlasBuild(VkCommandBuffer commandBuffer, std::uint32_t instanceCount);
    void destroyAccelerationStructure(VkAccelerationStructureKHR handle);

    vulkan::Context* m_context;
    bool m_rayTracing = false;
    std::uint64_t m_meshSyncs = 0;
    std::uint64_t m_frameNumber = 0;

    std::unordered_map<const MeshData*, MeshEntry> m_meshes;
    AccelerationStructure m_sphereBlas;
    vulkan::Buffer m_sphereAabb;

    // Device-local, per instance (m_instanceCapacity of each).
    std::uint32_t m_instanceCapacity = 0;
    vulkan::Buffer m_instances;
    vulkan::Buffer m_materials;
    vulkan::Buffer m_transforms;
    vulkan::Buffer m_previousTransforms;
    vulkan::Buffer m_tlasInstances;  // VkAccelerationStructureInstanceKHR[]
    vulkan::Buffer m_lights;  // LightRecord[]
    std::uint32_t m_lightCapacity = 0;
    vulkan::Buffer m_lightTriangleCdf;  // RenderScene::lightTriangleCdf.
    std::size_t m_lightTriangleCapacity = 0;
    std::uint32_t m_lightCount = 0;

    vulkan::Buffer m_skyRecord;  // SkyRecord.
    VkDeviceAddress m_skyRecordAddress = 0;
    vulkan::Buffer m_skyMap;                      // The space sky's texels.
    std::shared_ptr<const SkyMap> m_skyMapSource;  // What it holds.

    AccelerationStructure m_tlas;
    std::uint32_t m_tlasCapacity = 0;  // Instances the TLAS storage fits.
    vulkan::Buffer m_tlasScratch;

    std::array<vulkan::Buffer, kFramesInFlight> m_staging;
    std::vector<Retired> m_retired;

    // What the buffers hold: the render scene's lineage and stamps.
    std::uint64_t m_lineage = 0;
    std::uint64_t m_version = 0;
    std::uint64_t m_contentVersion = 0;
    std::uint64_t m_lightsVersion = 0;
    std::uint64_t m_skyVersion = 0;
    std::uint32_t m_uploadedInstances = 0;
};

}  // namespace starcore::gpu
