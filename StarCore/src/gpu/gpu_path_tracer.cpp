#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <format>

#include <glm/trigonometric.hpp>

#include "hybrid_renderer.hpp"
#include "starcore/common/assert.hpp"
#include "starcore/gpu/gpu_path_tracer.hpp"
#include "starcore/vulkan/check.hpp"
#include "starcore/vulkan/context.hpp"

namespace starcore
{

namespace
{

#include "shaders/path_trace.comp.spv.h"

using vulkan::check;

constexpr std::uint32_t kWorkgroupSize = 8;  // local_size of path_trace.comp.

// path_trace.comp flags.
constexpr std::uint32_t kFlagPreview = 1U;
constexpr std::uint32_t kFlagNextEvent = 2U;
constexpr std::uint32_t kFlagSrgb = 4U;
constexpr std::uint32_t kFlagWriteIds = 8U;

// Timestamps per frame slot.
constexpr std::uint32_t kQueriesPerSlot = 4;
constexpr std::uint32_t kQueryFrameStart = 0;
constexpr std::uint32_t kQueryTraceStart = 1;
constexpr std::uint32_t kQueryTraceEnd = 2;
constexpr std::uint32_t kQueryFrameEnd = 3;

/** @brief Push constants of path_trace.comp. */
struct PathTraceParams
{
    Vec4 cameraPosition;  // w: exposure
    Vec4 cameraForward;   // w: firefly clamp
    Vec4 cameraRight;     // right * aspect * tan(fov / 2)
    Vec4 cameraUp;        // up * tan(fov / 2)
    Vec4 skyColor;
    std::uint32_t frame[4];    // frame index, seed, samples per pixel, max bounces
    std::uint32_t options[4];  // flags, instance mask, light count, tone mapper
};
static_assert(sizeof(PathTraceParams) == 112, "push constant block of path_trace.comp");

// Bindings of path_trace.comp (set 0).
constexpr VkDescriptorType kBindings[] = {
    VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR,  // 0 tlas
    VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,               // 1 accumulation
    VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,               // 2 output
    VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,               // 3 entity ids
    VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,              // 4 instances
    VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,              // 5 materials
    VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,              // 6 lights
};

/** @brief Settings that only change how the result is displayed, not what is sampled. */
auto withoutDisplaySettings(PathTracerSettings settings, const PathTracerSettings& reference)
    -> PathTracerSettings
{
    settings.samplesPerPixel = reference.samplesPerPixel;  // Frames are weighted by samples.
    settings.exposure = reference.exposure;
    settings.toneMapper = reference.toneMapper;
    settings.srgbOutput = reference.srgbOutput;
    return settings;
}

}  // namespace

auto GpuPathTracer::create(vulkan::Context& context)
    -> std::expected<std::unique_ptr<GpuPathTracer>, std::string>
{
    if (!context.deviceInfo().rayQuery) {
        return std::unexpected(std::format("{} has no ray query support",
                                           context.deviceInfo().name));
    }
    return std::unique_ptr<GpuPathTracer>(new GpuPathTracer(context));
}

GpuPathTracer::GpuPathTracer(vulkan::Context& context)
    : m_context(&context), m_scene(context),
      m_pipeline(context, path_trace_comp_spv, kBindings, sizeof(PathTraceParams))
{
    const VkDevice device = context.device();
    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = context.queueFamily();
    check(vkCreateCommandPool(device, &poolInfo, nullptr, &m_commandPool));

    for (FrameSlot& slot : m_slots) {
        VkCommandBufferAllocateInfo allocateInfo{};
        allocateInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocateInfo.commandPool = m_commandPool;
        allocateInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocateInfo.commandBufferCount = 1;
        check(vkAllocateCommandBuffers(device, &allocateInfo, &slot.commandBuffer));
        VkFenceCreateInfo fenceInfo{};
        fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        check(vkCreateFence(device, &fenceInfo, nullptr, &slot.fence));
    }

    if (context.deviceInfo().timestampPeriod > 0.0F) {
        VkQueryPoolCreateInfo queryInfo{};
        queryInfo.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
        queryInfo.queryType = VK_QUERY_TYPE_TIMESTAMP;
        queryInfo.queryCount = kQueriesPerSlot * gpu::kFramesInFlight;
        check(vkCreateQueryPool(device, &queryInfo, nullptr, &m_queryPool));
    }
}

GpuPathTracer::~GpuPathTracer()
{
    waitIdle();
    m_hybrid.reset();
    const VkDevice device = m_context->device();
    for (FrameSlot& slot : m_slots) {
        vkDestroyFence(device, slot.fence, nullptr);
    }
    if (m_queryPool != VK_NULL_HANDLE) {
        vkDestroyQueryPool(device, m_queryPool, nullptr);
    }
    vkDestroyCommandPool(device, m_commandPool, nullptr);
}

void GpuPathTracer::waitForSlot(std::uint32_t index)
{
    FrameSlot& slot = m_slots[index];
    if (!slot.submitted) {
        return;
    }
    check(vkWaitForFences(m_context->device(), 1, &slot.fence, VK_TRUE, UINT64_MAX));
    check(vkResetFences(m_context->device(), 1, &slot.fence));
    slot.submitted = false;
    if (slot.timed) {
        std::uint64_t timestamps[kQueriesPerSlot] = {};
        if (vkGetQueryPoolResults(m_context->device(), m_queryPool, kQueriesPerSlot * index,
                                  kQueriesPerSlot, sizeof(timestamps), timestamps,
                                  sizeof(std::uint64_t), VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
            const double period = m_context->deviceInfo().timestampPeriod * 1e-6;
            auto milliseconds = [&](std::uint32_t from, std::uint32_t to) {
                return static_cast<float>(static_cast<double>(timestamps[to] - timestamps[from]) *
                                          period);
            };
            m_gpuTimeMs = milliseconds(kQueryFrameStart, kQueryFrameEnd);
            m_traceTimeMs = milliseconds(kQueryTraceStart, kQueryTraceEnd);
            if (slot.tracedSamples > 0) {
                m_msPerSample = m_traceTimeMs / static_cast<float>(slot.tracedSamples);
                m_overheadMs = std::max(0.0F, m_gpuTimeMs - m_traceTimeMs);
            }
        }
        slot.timed = false;
    }
}

auto GpuPathTracer::samplesForBudget(float budgetMs, std::uint32_t maxSamples) const
    -> std::uint32_t
{
    if (m_msPerSample <= 0.0F || budgetMs <= 0.0F) {
        return 1;
    }
    const float fitting = std::floor((budgetMs - m_overheadMs) / m_msPerSample);
    const float limit = static_cast<float>(std::min(maxSamples, 2 * m_lastSamples));
    return static_cast<std::uint32_t>(std::clamp(fitting, 1.0F, std::max(1.0F, limit)));
}

void GpuPathTracer::waitIdle()
{
    for (std::uint32_t i = 0; i < gpu::kFramesInFlight; ++i) {
        waitForSlot(i);
    }
}

void GpuPathTracer::resize(std::uint32_t width, std::uint32_t height)
{
    if (width == m_output.width() && height == m_output.height()) {
        return;
    }
    if (m_output.isValid()) {
        // The UI of frames in flight may still sample the old output.
        m_context->waitIdle();
    }
    waitIdle();
    m_accumulation = {};
    m_output = {};
    m_entityIds = {};
    if (m_hybrid) {
        m_hybrid->resize(width, height);
    }
    if (width == 0 || height == 0) {
        return;
    }
    constexpr VkImageUsageFlags kCopyUsage =
        VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    m_accumulation = vulkan::Image(*m_context, width, height, VK_FORMAT_R32G32B32A32_SFLOAT,
                                   VK_IMAGE_USAGE_STORAGE_BIT);
    m_output = vulkan::Image(*m_context, width, height, VK_FORMAT_R8G8B8A8_UNORM,
                             VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | kCopyUsage);
    m_entityIds = vulkan::Image(*m_context, width, height, VK_FORMAT_R32_UINT,
                                VK_IMAGE_USAGE_STORAGE_BIT | kCopyUsage);

    // GENERAL from now on; black and "no entity" until the first frame, so the
    // UI may show the output before anything was rendered.
    m_context->submitAndWait([&](VkCommandBuffer commandBuffer) {
        for (const vulkan::Image* image : {&m_accumulation, &m_output, &m_entityIds}) {
            vulkan::transitionImage(commandBuffer, image->handle(), VK_IMAGE_LAYOUT_UNDEFINED,
                                    VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0,
                                    VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
        }
        const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkClearColorValue black{};
        black.float32[3] = 1.0F;
        vkCmdClearColorImage(commandBuffer, m_output.handle(), VK_IMAGE_LAYOUT_GENERAL, &black, 1,
                             &range);
        VkClearColorValue noEntity{};
        noEntity.uint32[0] = kNoEntityId;
        vkCmdClearColorImage(commandBuffer, m_entityIds.handle(), VK_IMAGE_LAYOUT_GENERAL,
                             &noEntity, 1, &range);
        vulkan::memoryBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                              VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                              VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);
    });
    resetAccumulation();
}

auto GpuPathTracer::needsReset(const RenderScene& scene, std::uint64_t sceneHash,
                               std::uint64_t contentHash, const Camera& camera,
                               const PathTracerSettings& settings,
                               const GpuRenderOptions& options, bool& still) const -> bool
{
    const bool cameraMoved =
        camera.view() != m_lastView || camera.projection() != m_lastProjection;
    const bool settingsChanged =
        withoutDisplaySettings(settings, m_lastSettings) != m_lastSettings;
    still = false;
    if (m_frameIndex == 0 || !settings.accumulate || options.pipeline != m_lastPipeline) {
        return true;
    }
    if (options.pipeline == GpuPipeline::PathTracing) {
        return !m_sceneUploaded || sceneHash != m_uploadedSceneHash ||
               scene.sceneVersion != m_lastSceneVersion || cameraMoved || settingsChanged ||
               settings.exposure != m_lastSettings.exposure ||
               settings.toneMapper != m_lastSettings.toneMapper ||
               settings.srgbOutput != m_lastSettings.srgbOutput;
    }
    // Hybrid: only new content discards samples; motion reprojects them.
    if (settingsChanged || !options.denoiser.temporal || !m_lastTemporal ||
        contentHash != m_lastContentHash) {
        return true;
    }
    still = !cameraMoved && sceneHash == m_lastSceneHash;
    return false;
}

void GpuPathTracer::recordPathTracing(VkCommandBuffer commandBuffer, std::uint32_t slotIndex,
                                      const RenderScene& scene, const Camera& camera,
                                      const PathTracerSettings& settings)
{
    // Camera rays: forward + right * ndcX + up * ndcY (Camera::generateRay).
    const float tanHalfFov = std::tan(glm::radians(camera.verticalFov()) * 0.5F);
    std::uint32_t flags = 0;
    if (settings.mode == RenderMode::Preview) {
        flags |= kFlagPreview;
    }
    if (settings.nextEventEstimation) {
        flags |= kFlagNextEvent;
    }
    if (settings.srgbOutput) {
        flags |= kFlagSrgb;
    }
    if (m_frameIndex == 0) {
        flags |= kFlagWriteIds;
    }
    const PathTraceParams params{
        .cameraPosition = Vec4(camera.position(), settings.exposure),
        .cameraForward = Vec4(camera.forward(), settings.fireflyClamp),
        .cameraRight = Vec4(camera.right() * (camera.aspectRatio() * tanHalfFov), 0.0F),
        .cameraUp = Vec4(camera.up() * tanHalfFov, 0.0F),
        .skyColor = Vec4(scene.skyColor, 0.0F),
        .frame = {m_frameIndex, settings.seed, settings.samplesPerPixel, settings.maxBounces},
        .options = {flags, settings.instanceMask & kAllInstances, m_scene.lightCount(),
                    static_cast<std::uint32_t>(settings.toneMapper)},
    };

    vulkan::DescriptorWrites writes;
    writes.accelerationStructure(0, m_scene.tlas())
        .storageImage(1, m_accumulation.view())
        .storageImage(2, m_output.view())
        .storageImage(3, m_entityIds.view())
        .storageBuffer(4, m_scene.instanceBuffer())
        .storageBuffer(5, m_scene.materialBuffer())
        .storageBuffer(6, m_scene.lightBuffer());
    if (m_queryPool != VK_NULL_HANDLE) {
        vkCmdWriteTimestamp(commandBuffer, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, m_queryPool,
                            kQueriesPerSlot * slotIndex + kQueryTraceStart);
    }
    m_pipeline.bind(commandBuffer);
    m_pipeline.pushDescriptors(commandBuffer, writes.writes());
    m_pipeline.pushConstants(commandBuffer, &params, sizeof(params));
    vkCmdDispatch(commandBuffer, (m_output.width() + kWorkgroupSize - 1) / kWorkgroupSize,
                  (m_output.height() + kWorkgroupSize - 1) / kWorkgroupSize, 1);
    if (m_queryPool != VK_NULL_HANDLE) {
        vkCmdWriteTimestamp(commandBuffer, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, m_queryPool,
                            kQueriesPerSlot * slotIndex + kQueryTraceEnd);
    }
}

void GpuPathTracer::render(const RenderScene& scene, const Camera& camera,
                           const PathTracerSettings& settings, const GpuRenderOptions& options)
{
    if (!m_output.isValid()) {
        return;
    }
    const auto slotIndex = static_cast<std::uint32_t>(m_frameNumber % gpu::kFramesInFlight);
    waitForSlot(slotIndex);
    m_scene.collectGarbage(m_frameNumber);
    FrameSlot& slot = m_slots[slotIndex];
    const bool hybrid = options.pipeline == GpuPipeline::Hybrid;
    if (hybrid && !m_hybrid) {
        m_hybrid = std::make_unique<gpu::HybridRenderer>(*m_context);
        m_hybrid->resize(m_output.width(), m_output.height());
    }

    // The hash, not sceneVersion, decides about uploads: two scenes may share a version.
    const std::uint64_t sceneHash = hashRenderScene(scene);
    const std::uint64_t contentHash = hybrid ? hashRenderSceneContent(scene) : 0;
    const bool upload = !m_sceneUploaded || sceneHash != m_uploadedSceneHash;
    bool still = false;
    if (needsReset(scene, sceneHash, contentHash, camera, settings, options, still)) {
        m_frameIndex = 0;
    }

    const VkCommandBuffer commandBuffer = slot.commandBuffer;
    check(vkResetCommandBuffer(commandBuffer, 0));
    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check(vkBeginCommandBuffer(commandBuffer, &beginInfo));
    if (m_queryPool != VK_NULL_HANDLE) {
        vkCmdResetQueryPool(commandBuffer, m_queryPool, kQueriesPerSlot * slotIndex,
                            kQueriesPerSlot);
        vkCmdWriteTimestamp(commandBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, m_queryPool,
                            kQueriesPerSlot * slotIndex + kQueryFrameStart);
    }

    // Everything earlier on the queue (the previous frame's trace and the UI
    // sampling its output) before this frame rewrites scene data and images.
    vulkan::memoryBarrier(commandBuffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                          VK_ACCESS_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                          VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);
    if (upload) {
        m_scene.update(scene, commandBuffer, slotIndex, m_frameNumber);
        m_uploadedSceneHash = sceneHash;
        m_sceneUploaded = true;
    }

    const std::uint32_t samples =
        settings.mode == RenderMode::Preview ? 1U : std::max(1U, settings.samplesPerPixel);
    if (hybrid) {
        // The rasterizer reads instance data too.
        vulkan::memoryBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                              VK_ACCESS_TRANSFER_WRITE_BIT,
                              VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
                                  VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                              VK_ACCESS_SHADER_READ_BIT);
        const gpu::HybridFrame frame{
            .scene = &scene,
            .camera = &camera,
            .previousCamera = m_frameIndex == 0 ? &camera : &m_lastCamera,
            .settings = settings,
            .options = options,
            .frameSlot = slotIndex,
            // Fresh random numbers and jitter every frame, resets or not.
            .frameIndex = static_cast<std::uint32_t>(m_frameNumber),
            .reset = m_frameIndex == 0,
            .still = still,
            .writeIds = !still,
            .transforms = m_scene.writeTransforms(scene, slotIndex),
            .queryPool = m_queryPool,
            .traceQuery = kQueriesPerSlot * slotIndex + kQueryTraceStart,
        };
        m_hybrid->record(commandBuffer, frame, m_scene, m_output, m_entityIds);
    } else {
        recordPathTracing(commandBuffer, slotIndex, scene, camera, settings);
    }

    // The output is sampled by the UI pass or copied by readImage() later on the queue.
    vulkan::memoryBarrier(commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                          VK_ACCESS_SHADER_WRITE_BIT,
                          VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                          VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT);
    if (m_queryPool != VK_NULL_HANDLE) {
        vkCmdWriteTimestamp(commandBuffer, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, m_queryPool,
                            kQueriesPerSlot * slotIndex + kQueryFrameEnd);
        slot.timed = true;
    }
    slot.tracedSamples = settings.mode == RenderMode::Preview ? 0U : samples;
    m_lastSamples = samples;
    check(vkEndCommandBuffer(commandBuffer));

    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &commandBuffer;
    check(vkQueueSubmit(m_context->queue(), 1, &submitInfo, slot.fence));
    slot.submitted = true;

    m_lastSceneVersion = scene.sceneVersion;
    m_lastSceneHash = sceneHash;
    m_lastContentHash = contentHash;
    m_lastView = camera.view();
    m_lastProjection = camera.projection();
    m_lastSettings = settings;
    m_lastPipeline = options.pipeline;
    m_lastTemporal = options.denoiser.temporal;
    m_lastCamera = camera;
    ++m_frameNumber;
    ++m_frameIndex;
}

void GpuPathTracer::readBack(const vulkan::Image& image, std::span<std::byte> destination)
{
    waitIdle();
    vulkan::Buffer buffer(*m_context, destination.size(), VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                          vulkan::MemoryUsage::Readback);
    m_context->submitAndWait([&](VkCommandBuffer commandBuffer) {
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {image.width(), image.height(), 1};
        vkCmdCopyImageToBuffer(commandBuffer, image.handle(), VK_IMAGE_LAYOUT_GENERAL,
                               buffer.handle(), 1, &region);
        vulkan::memoryBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                              VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                              VK_ACCESS_HOST_READ_BIT);
    });
    buffer.invalidate();
    std::memcpy(destination.data(), buffer.mapped(), destination.size());
}

auto GpuPathTracer::readImage() -> ColorImage
{
    ColorImage image(width(), height());
    if (!image.empty()) {
        readBack(m_output, std::as_writable_bytes(image.pixels()));
    }
    return image;
}

auto GpuPathTracer::readEntityIds() -> IdImage
{
    IdImage ids(width(), height(), kNoEntityId);
    if (!ids.empty()) {
        readBack(m_entityIds, std::as_writable_bytes(ids.pixels()));
    }
    return ids;
}

}  // namespace starcore
