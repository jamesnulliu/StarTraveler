#include <algorithm>
#include <cstddef>
#include <span>

#include "starcore/common/assert.hpp"
#include "starcore/gpu/gpu_scene.hpp"
#include "starcore/vulkan/check.hpp"
#include "starcore/vulkan/context.hpp"

namespace starcore::gpu
{

using vulkan::Buffer;
using vulkan::check;
using vulkan::MemoryUsage;

namespace
{

static_assert(sizeof(RenderMaterial) == 64, "std430 layout of Material in scene.glsl");
static_assert(sizeof(RenderLight) == 32 && offsetof(RenderLight, radius) == 12 &&
                  offsetof(RenderLight, emission) == 16 &&
                  offsetof(RenderLight, instanceIndex) == 28,
              "std430 layout of Light in scene.glsl");

// Meshes not referenced by this many consecutive updates lose their BLAS.
constexpr std::uint64_t kMeshGracePeriod = 8;

constexpr VkBufferUsageFlags kBuildInputUsage =
    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
    VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
    VK_BUFFER_USAGE_TRANSFER_DST_BIT;
constexpr VkBufferUsageFlags kScratchUsage =
    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;

auto alignUp(VkDeviceSize value, VkDeviceSize alignment) -> VkDeviceSize
{
    return alignment <= 1 ? value : (value + alignment - 1) / alignment * alignment;
}

template <typename T>
auto bytesOf(const std::vector<T>& values) -> std::span<const std::byte>
{
    return std::as_bytes(std::span<const T>(values));
}

auto createAccelerationStructure(vulkan::Context& context, VkAccelerationStructureTypeKHR type,
                                 VkDeviceSize size) -> AccelerationStructure
{
    AccelerationStructure result;
    result.storage = Buffer(context, size,
                            VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR |
                                VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                            MemoryUsage::DeviceLocal);
    VkAccelerationStructureCreateInfoKHR info{};
    info.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR;
    info.buffer = result.storage.handle();
    info.size = size;
    info.type = type;
    const auto& functions = context.functions();
    check(functions.createAccelerationStructure(context.device(), &info, nullptr,
                                                &result.handle));
    VkAccelerationStructureDeviceAddressInfoKHR addressInfo{};
    addressInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR;
    addressInfo.accelerationStructure = result.handle;
    result.address = functions.getAccelerationStructureDeviceAddress(context.device(),
                                                                     &addressInfo);
    return result;
}

auto buildSizes(vulkan::Context& context, const VkAccelerationStructureBuildGeometryInfoKHR& info,
                std::uint32_t primitiveCount) -> VkAccelerationStructureBuildSizesInfoKHR
{
    VkAccelerationStructureBuildSizesInfoKHR sizes{};
    sizes.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
    context.functions().getAccelerationStructureBuildSizes(
        context.device(), VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &info, &primitiveCount,
        &sizes);
    return sizes;
}

/** @brief Scratch memory for builds, aligned as the device requires. */
struct Scratch
{
    Buffer buffer;
    VkDeviceAddress address = 0;

    Scratch() = default;
    Scratch(vulkan::Context& context, VkDeviceSize size)
    {
        const VkDeviceSize alignment = context.deviceInfo().scratchAlignment;
        buffer = Buffer(context, size + alignment, kScratchUsage, MemoryUsage::DeviceLocal);
        address = alignUp(buffer.deviceAddress(), alignment);
    }
};

/** @brief Build inputs (written by transfers) -> acceleration structure builds. */
void inputsToBuildBarrier(VkCommandBuffer commandBuffer)
{
    vulkan::memoryBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                          VK_ACCESS_TRANSFER_WRITE_BIT,
                          VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR |
                              VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                          VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR);
}

/** @brief Built acceleration structures -> later builds (TLAS) and ray queries. */
void buildToTraceBarrier(VkCommandBuffer commandBuffer)
{
    vulkan::memoryBarrier(commandBuffer, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                          VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
                          VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR |
                              VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                          VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR);
}

auto toVkTransform(const Mat4& m) -> VkTransformMatrixKHR
{
    VkTransformMatrixKHR transform{};
    for (int row = 0; row < 3; ++row) {
        for (int column = 0; column < 4; ++column) {
            transform.matrix[row][column] = m[column][row];  // glm is column-major.
        }
    }
    return transform;
}

void toRows(const Mat4& m, Vec4 (&rows)[3])
{
    for (int row = 0; row < 3; ++row) {
        rows[row] = Vec4(m[0][row], m[1][row], m[2][row], m[3][row]);
    }
}

}  // namespace

GpuScene::GpuScene(vulkan::Context& context) : m_context(&context)
{
    SC_VERIFY(context.deviceInfo().rayQuery, "GpuScene needs ray query support");
    buildSphereBlas();
}

GpuScene::~GpuScene()
{
    m_context->waitIdle();
    for (Retired& retired : m_retired) {
        destroyAccelerationStructure(retired.accelerationStructure);
    }
    for (auto& [mesh, entry] : m_meshes) {
        destroyAccelerationStructure(entry.blas.handle);
    }
    destroyAccelerationStructure(m_sphereBlas.handle);
    destroyAccelerationStructure(m_tlas.handle);
}

void GpuScene::destroyAccelerationStructure(VkAccelerationStructureKHR handle)
{
    if (handle != VK_NULL_HANDLE) {
        m_context->functions().destroyAccelerationStructure(m_context->device(), handle, nullptr);
    }
}

void GpuScene::retire(Buffer buffer, VkAccelerationStructureKHR accelerationStructure)
{
    m_retired.push_back(Retired{std::move(buffer), accelerationStructure, m_frameNumber});
}

void GpuScene::collectGarbage(std::uint64_t currentFrameNumber)
{
    // Frames before currentFrameNumber - kFramesInFlight + 1 have completed.
    std::erase_if(m_retired, [&](Retired& retired) {
        if (retired.frameNumber + kFramesInFlight > currentFrameNumber) {
            return false;
        }
        destroyAccelerationStructure(retired.accelerationStructure);
        return true;  // The buffer is destroyed with the entry.
    });
}

void GpuScene::buildSphereBlas()
{
    // The procedural unit sphere: one AABB, intersected in the shader.
    const VkAabbPositionsKHR aabb{-1.0F, -1.0F, -1.0F, 1.0F, 1.0F, 1.0F};
    m_sphereAabb = vulkan::createDeviceBuffer(
        *m_context, std::as_bytes(std::span<const VkAabbPositionsKHR>(&aabb, 1)),
        kBuildInputUsage);

    VkAccelerationStructureGeometryKHR geometry{};
    geometry.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
    geometry.geometryType = VK_GEOMETRY_TYPE_AABBS_KHR;
    geometry.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
    geometry.geometry.aabbs.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_AABBS_DATA_KHR;
    geometry.geometry.aabbs.data.deviceAddress = m_sphereAabb.deviceAddress();
    geometry.geometry.aabbs.stride = sizeof(VkAabbPositionsKHR);

    VkAccelerationStructureBuildGeometryInfoKHR build{};
    build.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
    build.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    build.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    build.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    build.geometryCount = 1;
    build.pGeometries = &geometry;

    const auto sizes = buildSizes(*m_context, build, 1);
    m_sphereBlas = createAccelerationStructure(
        *m_context, VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR, sizes.accelerationStructureSize);
    const Scratch scratch(*m_context, sizes.buildScratchSize);
    build.dstAccelerationStructure = m_sphereBlas.handle;
    build.scratchData.deviceAddress = scratch.address;

    const VkAccelerationStructureBuildRangeInfoKHR range{1, 0, 0, 0};
    const VkAccelerationStructureBuildRangeInfoKHR* ranges = &range;
    m_context->submitAndWait([&](VkCommandBuffer commandBuffer) {
        m_context->functions().cmdBuildAccelerationStructures(commandBuffer, 1, &build, &ranges);
        buildToTraceBarrier(commandBuffer);
    });
}

void GpuScene::buildMeshes(const std::vector<MeshHandle>& meshes)
{
    if (meshes.empty()) {
        return;
    }

    // Vertex/index buffers, filled from one staging buffer.
    struct Copy
    {
        VkBuffer destination;
        VkDeviceSize sourceOffset;
        std::span<const std::byte> data;
    };
    std::vector<Copy> copies;
    std::vector<MeshEntry*> entries;
    VkDeviceSize stagingSize = 0;
    auto addBuffer = [&](Buffer& buffer, std::span<const std::byte> data) {
        buffer = Buffer(*m_context, data.size(), kBuildInputUsage, MemoryUsage::DeviceLocal);
        copies.push_back({buffer.handle(), stagingSize, data});
        stagingSize = alignUp(stagingSize + data.size(), 16);
    };
    for (const MeshHandle& mesh : meshes) {
        MeshEntry& entry = m_meshes[mesh.get()];
        entry.mesh = mesh;
        entry.lastUsed = m_updateCount;
        addBuffer(entry.positions, bytesOf(mesh->positions));
        if (!mesh->normals.empty()) {
            addBuffer(entry.normals, bytesOf(mesh->normals));
        }
        addBuffer(entry.indices, bytesOf(mesh->indices));
        entries.push_back(&entry);
    }
    Buffer staging(*m_context, stagingSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, MemoryUsage::Upload);
    for (const Copy& copy : copies) {
        staging.write(copy.data, copy.sourceOffset);
    }

    // One BLAS per mesh, all built by one command (disjoint scratch ranges).
    const VkDeviceSize scratchAlignment = m_context->deviceInfo().scratchAlignment;
    std::vector<VkAccelerationStructureGeometryKHR> geometries(entries.size());
    std::vector<VkAccelerationStructureBuildGeometryInfoKHR> builds(entries.size());
    std::vector<VkAccelerationStructureBuildRangeInfoKHR> ranges(entries.size());
    std::vector<VkDeviceSize> scratchOffsets(entries.size());
    VkDeviceSize scratchSize = 0;
    for (std::size_t i = 0; i < entries.size(); ++i) {
        MeshEntry& entry = *entries[i];
        const MeshData& mesh = *entry.mesh;

        VkAccelerationStructureGeometryKHR& geometry = geometries[i];
        geometry.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
        geometry.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
        geometry.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
        auto& triangles = geometry.geometry.triangles;
        triangles.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
        triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
        triangles.vertexData.deviceAddress = entry.positions.deviceAddress();
        triangles.vertexStride = sizeof(Vec3);
        triangles.maxVertex = static_cast<std::uint32_t>(mesh.positions.size() - 1);
        triangles.indexType = VK_INDEX_TYPE_UINT32;
        triangles.indexData.deviceAddress = entry.indices.deviceAddress();

        VkAccelerationStructureBuildGeometryInfoKHR& build = builds[i];
        build.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
        build.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
        build.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
        build.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
        build.geometryCount = 1;
        build.pGeometries = &geometry;

        const auto triangleCount = static_cast<std::uint32_t>(mesh.triangleCount());
        ranges[i] = {triangleCount, 0, 0, 0};
        const auto sizes = buildSizes(*m_context, build, triangleCount);
        entry.blas = createAccelerationStructure(
            *m_context, VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR,
            sizes.accelerationStructureSize);
        build.dstAccelerationStructure = entry.blas.handle;
        scratchOffsets[i] = scratchSize;
        scratchSize = alignUp(scratchSize + sizes.buildScratchSize, scratchAlignment);
    }
    const Scratch scratch(*m_context, scratchSize);
    std::vector<const VkAccelerationStructureBuildRangeInfoKHR*> rangePointers;
    for (std::size_t i = 0; i < builds.size(); ++i) {
        builds[i].scratchData.deviceAddress = scratch.address + scratchOffsets[i];
        rangePointers.push_back(&ranges[i]);
    }

    m_context->submitAndWait([&](VkCommandBuffer commandBuffer) {
        for (const Copy& copy : copies) {
            const VkBufferCopy region{copy.sourceOffset, 0, copy.data.size()};
            vkCmdCopyBuffer(commandBuffer, staging.handle(), copy.destination, 1, &region);
        }
        inputsToBuildBarrier(commandBuffer);
        m_context->functions().cmdBuildAccelerationStructures(
            commandBuffer, static_cast<std::uint32_t>(builds.size()), builds.data(),
            rangePointers.data());
        buildToTraceBarrier(commandBuffer);
    });
}

void GpuScene::ensureCapacity(Buffer& buffer, VkDeviceSize size, VkBufferUsageFlags usage)
{
    if (buffer.isValid() && buffer.size() >= size) {
        return;
    }
    const VkDeviceSize capacity = std::max(size, buffer.size() * 2);
    if (buffer.isValid()) {
        retire(std::move(buffer), VK_NULL_HANDLE);
    }
    buffer = Buffer(*m_context, capacity, usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                    MemoryUsage::DeviceLocal);
}

void GpuScene::update(const RenderScene& scene, VkCommandBuffer commandBuffer,
                      std::uint32_t frameSlot, std::uint64_t frameNumber)
{
    ++m_updateCount;
    m_frameNumber = frameNumber;

    // New meshes get their BLAS now.
    std::vector<MeshHandle> newMeshes;
    for (const RenderGeometry& geometry : scene.geometries) {
        if (geometry.kind == GeometryKind::Triangles && geometry.mesh &&
            geometry.mesh->triangleCount() > 0 && !m_meshes.contains(geometry.mesh.get())) {
            newMeshes.push_back(geometry.mesh);
        }
    }
    buildMeshes(newMeshes);

    // Instance records and TLAS instances.
    const auto instanceCount = static_cast<std::uint32_t>(scene.instances.size());
    std::vector<InstanceRecord> records(instanceCount);
    std::vector<VkAccelerationStructureInstanceKHR> tlasInstances(instanceCount);
    for (std::uint32_t i = 0; i < instanceCount; ++i) {
        const RenderInstance& instance = scene.instances[i];
        const RenderGeometry& geometry = scene.geometries[instance.geometryIndex];
        InstanceRecord& record = records[i];
        record.materialIndex = instance.materialIndex;
        record.entityId = instance.entityId;
        record.lightIndex = scene.lightOfInstance[i];
        record.kind = static_cast<std::uint32_t>(geometry.kind);

        VkDeviceAddress blas = m_sphereBlas.address;
        if (geometry.kind == GeometryKind::Triangles) {
            MeshEntry& entry = m_meshes.at(geometry.mesh.get());
            entry.lastUsed = m_updateCount;
            record.positions = entry.positions.deviceAddress();
            record.normals = entry.normals.isValid() ? entry.normals.deviceAddress() : 0;
            record.indices = entry.indices.deviceAddress();
            blas = entry.blas.address;
        }

        VkAccelerationStructureInstanceKHR& tlasInstance = tlasInstances[i];
        tlasInstance.transform = toVkTransform(instance.objectToWorld);
        tlasInstance.instanceCustomIndex = i;
        tlasInstance.mask = instance.mask & 0xFFU;
        tlasInstance.instanceShaderBindingTableRecordOffset = 0;
        tlasInstance.flags = static_cast<std::uint32_t>(
            VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR);
        tlasInstance.accelerationStructureReference = blas;
    }

    // Meshes no longer in use (in-flight frames may still trace them: retire).
    std::erase_if(m_meshes, [&](auto& item) {
        MeshEntry& entry = item.second;
        if (entry.lastUsed + kMeshGracePeriod > m_updateCount) {
            return false;
        }
        retire(std::move(entry.positions), VK_NULL_HANDLE);
        retire(std::move(entry.normals), VK_NULL_HANDLE);
        retire(std::move(entry.indices), VK_NULL_HANDLE);
        retire(std::move(entry.blas.storage), entry.blas.handle);
        return true;
    });

    // Upload through this frame slot's staging buffer.
    const auto recordBytes = bytesOf(records);
    const auto materialBytes = bytesOf(scene.materials);
    const auto lightBytes = bytesOf(scene.lights);
    const auto tlasInstanceBytes = bytesOf(tlasInstances);
    const VkDeviceSize materialOffset = alignUp(recordBytes.size(), 256);
    const VkDeviceSize lightOffset = materialOffset + alignUp(materialBytes.size(), 256);
    const VkDeviceSize tlasInstanceOffset = lightOffset + alignUp(lightBytes.size(), 256);
    const VkDeviceSize stagingSize = tlasInstanceOffset + tlasInstanceBytes.size();
    Buffer& staging = m_staging[frameSlot];
    if (!staging.isValid() || staging.size() < stagingSize) {
        // The slot's previous submission completed: no need to retire.
        staging = Buffer(*m_context, std::max(stagingSize, staging.size() * 2),
                         VK_BUFFER_USAGE_TRANSFER_SRC_BIT, MemoryUsage::Upload);
    }
    staging.write(recordBytes, 0);
    staging.write(materialBytes, materialOffset);
    staging.write(lightBytes, lightOffset);
    staging.write(tlasInstanceBytes, tlasInstanceOffset);

    constexpr VkBufferUsageFlags kStorage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    ensureCapacity(m_instances, recordBytes.size(), kStorage);
    ensureCapacity(m_materials, materialBytes.size(), kStorage);
    ensureCapacity(m_lights, lightBytes.size(), kStorage);
    ensureCapacity(m_tlasInstances, tlasInstanceBytes.size(),
                   VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                       VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR);
    m_lightCount = static_cast<std::uint32_t>(scene.lights.size());

    auto copy = [&](const Buffer& destination, VkDeviceSize offset, VkDeviceSize size) {
        if (size > 0) {
            const VkBufferCopy region{offset, 0, size};
            vkCmdCopyBuffer(commandBuffer, staging.handle(), destination.handle(), 1, &region);
        }
    };
    copy(m_instances, 0, recordBytes.size());
    copy(m_materials, materialOffset, materialBytes.size());
    copy(m_lights, lightOffset, lightBytes.size());
    copy(m_tlasInstances, tlasInstanceOffset, tlasInstanceBytes.size());
    inputsToBuildBarrier(commandBuffer);
    recordTlasBuild(commandBuffer, instanceCount);
}

auto GpuScene::writeTransforms(const RenderScene& scene, std::uint32_t frameSlot) -> VkBuffer
{
    std::vector<InstanceTransformRecord> records(scene.instances.size());
    std::unordered_map<std::uint64_t, Mat4> transforms;
    transforms.reserve(scene.instances.size());
    for (std::size_t i = 0; i < scene.instances.size(); ++i) {
        const RenderInstance& instance = scene.instances[i];
        // An entity may have a sphere and a mesh (with different transforms).
        const auto kind = static_cast<std::uint64_t>(scene.geometries[instance.geometryIndex].kind);
        const std::uint64_t key = (static_cast<std::uint64_t>(instance.entityId) << 1U) | kind;
        const auto last = m_lastTransforms.find(key);
        const Mat4& previous = last != m_lastTransforms.end() ? last->second
                                                               : instance.objectToWorld;
        toRows(instance.objectToWorld, records[i].objectToWorld);
        toRows(instance.worldToObject, records[i].worldToObject);
        toRows(previous, records[i].previousObjectToWorld);
        transforms.emplace(key, instance.objectToWorld);
    }
    m_lastTransforms = std::move(transforms);

    // The slot's previous submission completed: its buffer may be rewritten.
    const auto bytes = bytesOf(records);
    vulkan::Buffer& buffer = m_transforms[frameSlot];
    if (!buffer.isValid() || buffer.size() < bytes.size()) {
        buffer = Buffer(*m_context, std::max<VkDeviceSize>(bytes.size(), buffer.size() * 2),
                        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, MemoryUsage::Upload);
    }
    buffer.write(bytes);
    return buffer.handle();
}

void GpuScene::recordTlasBuild(VkCommandBuffer commandBuffer, std::uint32_t instanceCount)
{
    VkAccelerationStructureGeometryKHR geometry{};
    geometry.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
    geometry.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
    geometry.geometry.instances.sType =
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
    geometry.geometry.instances.arrayOfPointers = VK_FALSE;
    geometry.geometry.instances.data.deviceAddress = m_tlasInstances.deviceAddress();

    VkAccelerationStructureBuildGeometryInfoKHR build{};
    build.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
    build.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    build.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    build.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    build.geometryCount = 1;
    build.pGeometries = &geometry;

    if (m_tlas.handle == VK_NULL_HANDLE || instanceCount > m_tlasCapacity) {
        // Grow: a new TLAS (and scratch) sized for twice the instances.
        m_tlasCapacity = std::max({instanceCount, m_tlasCapacity * 2, 16U});
        const auto sizes = buildSizes(*m_context, build, m_tlasCapacity);
        if (m_tlas.handle != VK_NULL_HANDLE) {
            retire(std::move(m_tlas.storage), m_tlas.handle);
        }
        m_tlas = createAccelerationStructure(*m_context, VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR,
                                             sizes.accelerationStructureSize);
        const VkDeviceSize alignment = m_context->deviceInfo().scratchAlignment;
        if (m_tlasScratch.isValid()) {
            retire(std::move(m_tlasScratch), VK_NULL_HANDLE);
        }
        m_tlasScratch = Buffer(*m_context, sizes.buildScratchSize + alignment, kScratchUsage,
                               MemoryUsage::DeviceLocal);
    }
    build.dstAccelerationStructure = m_tlas.handle;
    build.scratchData.deviceAddress =
        alignUp(m_tlasScratch.deviceAddress(), m_context->deviceInfo().scratchAlignment);

    const VkAccelerationStructureBuildRangeInfoKHR range{instanceCount, 0, 0, 0};
    const VkAccelerationStructureBuildRangeInfoKHR* ranges = &range;
    m_context->functions().cmdBuildAccelerationStructures(commandBuffer, 1, &build, &ranges);
    buildToTraceBarrier(commandBuffer);
}

}  // namespace starcore::gpu
