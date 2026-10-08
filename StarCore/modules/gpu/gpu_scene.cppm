// A RenderScene on the GPU: storage buffers with the per-instance data
// (records, materials, transforms), the lights and the meshes' vertex and
// index buffers; with ray queries also one bottom-level acceleration structure
// (BLAS) per mesh, kept across frames, and a top-level one (TLAS) over the
// instances. Layouts in shaders/scene.glsl and hybrid.glsl.
//
// Updates are incremental: while the render scene is the one of the previous
// update (its lineage, see RenderSceneBuilder), only the instances stamped
// since are uploaded.

module;

#include <vulkan/vulkan.h>

export module starcore.gpu:gpu_scene;

import std;
import starcore.asset;
import starcore.math;
import starcore.render;
import starcore.vulkan;

export namespace starcore::gpu
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
    vulkan::Buffer m_lights;
    std::uint32_t m_lightCapacity = 0;
    std::uint32_t m_lightCount = 0;

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
    std::uint32_t m_uploadedInstances = 0;
};

}  // namespace starcore::gpu
