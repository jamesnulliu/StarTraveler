#pragma once
// Irradiance probes of the hybrid pipeline (private; see ProbeSettings in
// gpu_renderer.hpp and shaders/probes.glsl): the volumes laid out for a frame,
// the probes' maps and states on the GPU (buffers, their addresses in the frame
// uniforms), and the passes updating them (probe_trace.comp, probe_update.comp).

#include <array>
#include <cstdint>
#include <vector>

#include <vulkan/vulkan.h>

#include "rasterization.hpp"
#include "starcore/gpu/gpu_renderer.hpp"
#include "starcore/gpu/gpu_scene.hpp"
#include "starcore/vulkan/pipeline.hpp"
#include "starcore/vulkan/resources.hpp"

namespace starcore::gpu
{

/** @brief One volume (`ProbeVolume` in probes.glsl, std430). */
struct ProbeVolumeRecord
{
    Vec4 worldToGrid[3];  // Rows.
    Vec4 gridToWorld[3];
    Vec4 axes[3];            // Rows: grid directions to world ones.
    std::int32_t probes[4];  // Per axis, first probe.
    std::int32_t origin[4];  // Grid cell of the probes at (0, 0, 0).
    Vec4 bias;               // Along the normal, towards the viewer, max ray distance, -.
};
static_assert(sizeof(ProbeVolumeRecord) == 192, "std430 layout of ProbeVolume in probes.glsl");

/** @brief What the probes of a frame need of it. */
struct ProbeFrame
{
    const RenderScene* scene = nullptr;
    const Camera* camera = nullptr;
    ProbeSettings settings;
    std::uint32_t frameSlot = 0;
    std::uint32_t frameIndex = 0;  // Turns the rays.
};

class ProbeField
{
public:
    explicit ProbeField(vulkan::Context& context);

    /**
     * @brief Lay the volumes out for a frame: the grid around the camera and the
     *        scene's probe volumes (when that changes, their buffers are made anew,
     *        once the device is idle); write the volumes and the buffers' addresses
     *        into the frame uniforms. All probes start over then, and with other
     *        settings, new content (RenderScene::contentVersion) or after frames
     *        without them.
     */
    void prepare(const ProbeFrame& frame, FrameUniforms& uniforms);

    /**
     * @brief Record the probes' update (after prepare(), with the frame uniforms it
     *        wrote): their rays, then their maps, then a barrier for compute shaders
     *        looking them up.
     */
    void record(VkCommandBuffer commandBuffer, const GpuScene& scene, VkBuffer uniforms);

    /** @brief Probes in this frame's volumes. */
    [[nodiscard]] auto probeCount() const noexcept -> std::uint32_t
    {
        return m_probeCount;
    }

private:
    void allocate(std::uint32_t probes, std::uint32_t rays);

    vulkan::Context* m_context;
    vulkan::ComputePipeline m_trace;
    vulkan::ComputePipeline m_update;
    std::array<vulkan::Buffer, kFramesInFlight> m_volumes;  // Header and volumes, per frame.
    vulkan::Buffer m_irradiance;  // vec4 per texel.
    vulkan::Buffer m_distance;    // vec2 per texel.
    vulkan::Buffer m_states;
    vulkan::Buffer m_rays;  // Per probe and ray: light, distance.
    std::vector<std::int64_t> m_layout;  // What the probes are allocated and started for.
    std::uint32_t m_probeCount = 0;
    std::uint32_t m_rayCount = 0;  // Per probe.
    bool m_fresh = true;           // To start over (their states cleared) this frame.
    // What they learnt of: settings, content, the last frame they were updated.
    ProbeSettings m_settings;
    std::uint64_t m_contentVersion = 0;
    std::uint32_t m_lastFrameIndex = 0;
};

}  // namespace starcore::gpu
