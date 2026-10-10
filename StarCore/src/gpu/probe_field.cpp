#include <algorithm>
#include <cmath>
#include <random>
#include <span>

#include <glm/common.hpp>
#include <glm/geometric.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/matrix.hpp>

#include "probe_field.hpp"
#include "starcore/vulkan/context.hpp"

namespace starcore::gpu
{

namespace
{

#include "shaders/probe_trace.comp.spv.h"
#include "shaders/probe_update.comp.spv.h"

constexpr VkDescriptorType kBuffer = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
constexpr VkDescriptorType kUniform = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
constexpr VkDescriptorType kImage = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
constexpr VkDescriptorType kTlas = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
// Bindings 0-6 as the hybrid pipeline's (the scene, its frame uniforms), 7 the
// rays, 8-11 the probes (probes.glsl).
constexpr VkDescriptorType kTraceBindings[] = {kTlas,   kUniform, kBuffer, kImage,
                                               kBuffer, kBuffer,  kBuffer, kBuffer,
                                               kBuffer, kImage,   kImage,  kBuffer};
constexpr VkDescriptorType kUpdateBindings[] = {kBuffer, kUniform, kBuffer, kImage,
                                                kBuffer, kBuffer,  kBuffer, kBuffer,
                                                kBuffer, kImage,   kImage,  kBuffer};
constexpr std::uint32_t kProbeBinding = 8;

// Texels per side of a probe's maps with their border (probes.glsl).
constexpr std::uint32_t kIrradianceTile = 6 + 2;
constexpr std::uint32_t kDistanceTile = 14 + 2;
constexpr std::uint32_t kAtlasWidth = 128;  // Probes per row of the atlases.
constexpr std::uint32_t kMaxVolumes = 8;
constexpr std::uint32_t kMaxProbes = 65535;  // A row of workgroups each (probe_trace.comp).
constexpr std::uint32_t kTraceGroup = 64;    // local_size_x of probe_trace.comp.
constexpr std::uint32_t kMinRays = 32;
constexpr std::uint32_t kMaxRays = 256;  // probe_update.comp keeps them in shared memory.
// Lookups start off the surface, by these fractions of a volume's smallest cell
// side (DDGI's biases): along the normal and towards the viewer.
constexpr float kNormalBias = 0.1F;
constexpr float kViewBias = 0.3F;

/** @brief Head of the volumes buffer (probes.glsl). */
struct ProbeHeader
{
    std::uint32_t info[4];  // Volumes, rays per probe, probes, atlas width.
    Vec4 rotation[3];       // Rows.
    Vec4 blend;             // Hysteresis.
};
static_assert(sizeof(ProbeHeader) == 80, "std430 layout of ProbeVolumes in probes.glsl");

void toRows(const Mat4& m, Vec4 (&rows)[3])
{
    for (int row = 0; row < 3; ++row) {
        rows[row] = Vec4(m[0][row], m[1][row], m[2][row], m[3][row]);
    }
}

/** @brief A volume of a frame's layout. */
struct Volume
{
    Mat4 gridToWorld{1.0F};
    IVec3 probes{2};
    IVec3 origin{0};          // Grid cell of probe (0, 0, 0): the grid around the camera scrolls.
    std::int64_t entity = -1;  // -1: around the camera.
};

auto probesIn(const Volume& volume) -> std::uint32_t
{
    return static_cast<std::uint32_t>(volume.probes.x * volume.probes.y * volume.probes.z);
}

auto makeRecord(const Volume& volume, std::uint32_t firstProbe) -> ProbeVolumeRecord
{
    ProbeVolumeRecord record{};
    toRows(volume.gridToWorld, record.gridToWorld);
    toRows(glm::inverse(volume.gridToWorld), record.worldToGrid);
    Vec3 cell(0.0F);
    Mat4 axes(1.0F);
    for (int i = 0; i < 3; ++i) {
        cell[i] = glm::length(Vec3(volume.gridToWorld[i]));
        axes[i] = Vec4(Vec3(volume.gridToWorld[i]) / cell[i], 0.0F);
    }
    toRows(axes, record.axes);
    record.probes[0] = volume.probes.x;
    record.probes[1] = volume.probes.y;
    record.probes[2] = volume.probes.z;
    record.probes[3] = static_cast<std::int32_t>(firstProbe);
    record.origin[0] = volume.origin.x;
    record.origin[1] = volume.origin.y;
    record.origin[2] = volume.origin.z;
    const float smallest = std::min({cell.x, cell.y, cell.z});
    // Rays reach 1.5 cell diagonals for the distance maps (DDGI): beyond that, a probe
    // sees whatever its neighbours see.
    record.bias = Vec4(kNormalBias * smallest, kViewBias * smallest, 1.5F * glm::length(cell),
                       0.0F);
    return record;
}

/** @brief A random rotation for a frame's rays (Shoemake): over the frames they cover all directions. */
auto rayRotation(std::uint32_t frameIndex) -> Mat3
{
    std::mt19937 random(frameIndex);
    std::uniform_real_distribution<float> uniform(0.0F, 1.0F);
    const float u1 = uniform(random);
    const float u2 = 2.0F * kPi * uniform(random);
    const float u3 = 2.0F * kPi * uniform(random);
    const float a = std::sqrt(1.0F - u1);
    const float b = std::sqrt(u1);
    return glm::mat3_cast(
        Quat(b * std::cos(u3), a * std::sin(u2), a * std::cos(u2), b * std::sin(u3)));
}

}  // namespace

ProbeField::ProbeField(vulkan::Context& context)
    : m_context(&context),
      m_trace(context, probe_trace_comp_spv, kTraceBindings),
      m_update(context, probe_update_comp_spv, kUpdateBindings)
{
    for (vulkan::Buffer& buffer : m_volumes) {
        buffer = vulkan::Buffer(context, sizeof(ProbeHeader) + kMaxVolumes * sizeof(ProbeVolumeRecord),
                                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, vulkan::MemoryUsage::Upload);
    }
}

void ProbeField::allocate(VkCommandBuffer commandBuffer, std::uint32_t probes, std::uint32_t rays)
{
    if (m_states.isValid()) {
        m_context->waitIdle();  // The frame in flight may still use them.
    }
    const std::uint32_t rows = (probes + kAtlasWidth - 1) / kAtlasWidth;
    m_irradiance = vulkan::Image(*m_context, kAtlasWidth * kIrradianceTile, rows * kIrradianceTile,
                                 VK_FORMAT_R32G32B32A32_SFLOAT, VK_IMAGE_USAGE_STORAGE_BIT);
    m_distance = vulkan::Image(*m_context, kAtlasWidth * kDistanceTile, rows * kDistanceTile,
                               VK_FORMAT_R32G32_SFLOAT, VK_IMAGE_USAGE_STORAGE_BIT);
    m_states = vulkan::Buffer(*m_context, VkDeviceSize{probes} * 32,
                              VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                              vulkan::MemoryUsage::DeviceLocal);
    m_rays = vulkan::Buffer(*m_context, VkDeviceSize{probes} * rays * sizeof(Vec4),
                            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, vulkan::MemoryUsage::DeviceLocal);
    m_capacity = probes;

    // The maps are undefined until a probe's first update writes them; the states say
    // no probe has any (updates 0).
    for (const vulkan::Image* image : {&m_irradiance, &m_distance}) {
        vulkan::transitionImage(commandBuffer, image->handle(), VK_IMAGE_LAYOUT_UNDEFINED,
                                VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0,
                                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
    }
    vkCmdFillBuffer(commandBuffer, m_states.handle(), 0, VK_WHOLE_SIZE, 0);
    vulkan::memoryBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                          VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                          VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
}

void ProbeField::record(VkCommandBuffer commandBuffer, const ProbeFrame& frame,
                        const GpuScene& scene)
{
    const ProbeSettings& settings = frame.settings;
    std::vector<Volume> layout;
    if (settings.enabled) {
        for (const RenderProbeVolume& volume : frame.scene->probeVolumes) {
            layout.push_back(Volume{.gridToWorld = volume.gridToWorld,
                                    .probes = glm::max(volume.probes, IVec3(2)),
                                    .entity = volume.entityId});
        }
        // The most detailed first (the smallest cells): they take precedence.
        std::ranges::stable_sort(layout, {}, [](const Volume& volume) {
            return std::abs(glm::determinant(Mat3(volume.gridToWorld)));
        });
        if (layout.size() >= kMaxVolumes) {
            layout.resize(kMaxVolumes - 1);
        }
        // The grid around the camera, moved in whole cells: its probes stay where they are.
        const IVec3 probes = glm::max(settings.probes, IVec3(2));
        const float spacing = std::max(settings.spacing, 1e-3F);
        const IVec3 origin =
            IVec3(glm::floor(frame.camera->position() / spacing + 0.5F)) - probes / 2;
        Mat4 gridToWorld(1.0F);
        gridToWorld[0][0] = spacing;
        gridToWorld[1][1] = spacing;
        gridToWorld[2][2] = spacing;
        gridToWorld[3] = Vec4(Vec3(origin) * spacing, 1.0F);
        layout.push_back(Volume{.gridToWorld = gridToWorld, .probes = probes, .origin = origin});
    }

    // All probes start over when the layout changes (scrolling restarts probes one by one).
    const std::uint32_t rays = std::clamp(settings.raysPerProbe, kMinRays, kMaxRays);
    std::vector<std::int64_t> key{rays};
    std::vector<ProbeVolumeRecord> records;
    std::uint32_t total = 0;
    for (const Volume& volume : layout) {
        if (total + probesIn(volume) > kMaxProbes) {
            continue;
        }
        records.push_back(makeRecord(volume, total));
        total += probesIn(volume);
        key.insert(key.end(), {volume.entity, volume.probes.x, volume.probes.y, volume.probes.z});
    }
    if (key != m_layout || !m_states.isValid()) {
        allocate(commandBuffer, std::max(total, 1U), rays);
        m_layout = std::move(key);
    }
    m_probeCount = total;

    ProbeHeader header{};
    header.info[0] = static_cast<std::uint32_t>(records.size());
    header.info[1] = rays;
    header.info[2] = total;
    header.info[3] = kAtlasWidth;
    const Mat3 rotation = rayRotation(frame.frameIndex);
    for (int row = 0; row < 3; ++row) {
        header.rotation[row] = Vec4(rotation[0][row], rotation[1][row], rotation[2][row], 0.0F);
    }
    header.blend = Vec4(std::clamp(settings.hysteresis, 0.0F, 0.999F), 0.0F, 0.0F, 0.0F);
    vulkan::Buffer& volumes = m_volumes[frame.frameSlot];
    volumes.write(std::as_bytes(std::span(&header, 1)));
    volumes.write(std::as_bytes(std::span(records)), sizeof(ProbeHeader));
    if (total == 0) {
        return;
    }

    auto bindAll = [&](const vulkan::ComputePipeline& pipeline, bool tlas) {
        vulkan::DescriptorWrites writes;
        if (tlas) {
            writes.accelerationStructure(0, scene.tlas());
        }
        writes.uniformBuffer(1, frame.uniforms)
            .storageBuffer(2, scene.transformBuffer())
            .storageBuffer(4, scene.instanceBuffer())
            .storageBuffer(5, scene.materialBuffer())
            .storageBuffer(6, scene.lightBuffer())
            .storageBuffer(7, m_rays.handle());
        bind(writes, kProbeBinding, frame.frameSlot);
        pipeline.bind(commandBuffer);
        pipeline.pushDescriptors(commandBuffer, writes.writes());
    };
    auto barrier = [&] {
        vulkan::memoryBarrier(commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                              VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                              VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
    };
    bindAll(m_trace, true);
    vkCmdDispatch(commandBuffer, (rays + kTraceGroup - 1) / kTraceGroup, total, 1);
    barrier();
    bindAll(m_update, false);
    vkCmdDispatch(commandBuffer, std::min(total, kMaxProbes), (total + kMaxProbes - 1) / kMaxProbes,
                  1);
    barrier();
}

void ProbeField::bind(vulkan::DescriptorWrites& writes, std::uint32_t first,
                      std::uint32_t frameSlot) const
{
    writes.storageBuffer(first, m_volumes[frameSlot].handle())
        .storageImage(first + 1, m_irradiance.view())
        .storageImage(first + 2, m_distance.view())
        .storageBuffer(first + 3, m_states.handle());
}

}  // namespace starcore::gpu
