#pragma once
// A RenderScene on the GPU: one bottom-level acceleration structure (BLAS)
// per mesh, kept across frames, plus a top-level one (TLAS) over the
// instances and storage buffers with per-instance data, materials and lights
// (layouts in shaders/scene.glsl).

#include <array>
#include <cstdint>
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
    Vec4 previousObjectToWorld[3];  // Where the instance was the frame before.
};
static_assert(sizeof(InstanceTransformRecord) == 144,
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
    /** @pre context.deviceInfo().rayQuery */
    explicit GpuScene(vulkan::Context& context);
    ~GpuScene();
    GpuScene(const GpuScene&) = delete;
    auto operator=(const GpuScene&) -> GpuScene& = delete;

    /**
     * @brief Make `scene` the current scene.
     *
     * Meshes seen for the first time are uploaded and get a BLAS right away
     * (blocking); instance data, materials and lights go through the staging
     * buffer of `frameSlot`, and their copies plus the TLAS build are recorded
     * into `commandBuffer` (followed by a barrier for compute shaders).
     *
     * @param frameSlot Frame in flight whose previous submission has completed.
     * @param frameNumber Increases by one per submitted frame (resource retirement).
     */
    void update(const RenderScene& scene, VkCommandBuffer commandBuffer,
                std::uint32_t frameSlot, std::uint64_t frameNumber);

    /** @brief Destroy resources replaced at least kFramesInFlight frames ago. */
    void collectGarbage(std::uint64_t completedFrameNumber);

    /**
     * @brief Write the transforms of `scene`'s instances, with each one's
     *        transform of the previous call as the previous transform (matched
     *        by entity and geometry kind; new instances have not moved), into
     *        the host-visible buffer of `frameSlot`.
     * @return That buffer (InstanceTransformRecord[], one per instance).
     */
    auto writeTransforms(const RenderScene& scene, std::uint32_t frameSlot) -> VkBuffer;

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
    /** @brief Meshes with a BLAS (cached across updates). */
    [[nodiscard]] auto meshCount() const noexcept -> std::size_t
    {
        return m_meshes.size();
    }

private:
    struct MeshEntry
    {
        MeshHandle mesh;  // Keeps the address (the cache key) from being reused.
        vulkan::Buffer positions;
        vulkan::Buffer normals;
        vulkan::Buffer indices;
        AccelerationStructure blas;
        std::uint64_t lastUsed = 0;  // Update counter.
    };

    struct Retired
    {
        vulkan::Buffer buffer;
        VkAccelerationStructureKHR accelerationStructure = VK_NULL_HANDLE;
        std::uint64_t frameNumber = 0;
    };

    void buildSphereBlas();
    void buildMeshes(const std::vector<MeshHandle>& meshes);
    void retire(vulkan::Buffer buffer, VkAccelerationStructureKHR accelerationStructure);
    void ensureCapacity(vulkan::Buffer& buffer, VkDeviceSize size, VkBufferUsageFlags usage);
    void recordTlasBuild(VkCommandBuffer commandBuffer, std::uint32_t instanceCount);
    void destroyAccelerationStructure(VkAccelerationStructureKHR handle);

    vulkan::Context* m_context;
    std::uint64_t m_updateCount = 0;
    std::uint64_t m_frameNumber = 0;

    std::unordered_map<const MeshData*, MeshEntry> m_meshes;
    AccelerationStructure m_sphereBlas;
    vulkan::Buffer m_sphereAabb;

    // Device-local scene data, rewritten by every update().
    vulkan::Buffer m_instances;
    vulkan::Buffer m_materials;
    vulkan::Buffer m_lights;
    vulkan::Buffer m_tlasInstances;  // VkAccelerationStructureInstanceKHR[]
    std::uint32_t m_lightCount = 0;

    AccelerationStructure m_tlas;
    std::uint32_t m_tlasCapacity = 0;  // Instances the TLAS storage fits.
    vulkan::Buffer m_tlasScratch;

    std::array<vulkan::Buffer, kFramesInFlight> m_staging;
    std::vector<Retired> m_retired;

    // writeTransforms(): per frame slot, and each instance's last transform.
    std::array<vulkan::Buffer, kFramesInFlight> m_transforms;
    std::unordered_map<std::uint64_t, Mat4> m_lastTransforms;
};

}  // namespace starcore::gpu
