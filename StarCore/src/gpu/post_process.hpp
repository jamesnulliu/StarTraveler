#pragma once
// The last stage of every GpuRenderer pipeline (private). Per frame:
//   1. particles (particle.vert/.frag): additive sprites, streaked along their
//      screen motion, into a particle image, tested against the visibility
//      buffer's depth;
//   2. bloom (bloom_prefilter/down/up.comp): the frame downsampled into a
//      chain of half-size levels, then upsampled back summing them;
//   3. motion tiles (motion_tiles/neighbors.comp): the largest blur per 16x16
//      tile and its neighborhood;
//   4. final (post.comp): motion blur of the radiance (McGuire's
//      reconstruction filter), particles, bloom, vignette, tone mapping into
//      the RGBA8 output.

#include <array>
#include <cstdint>
#include <span>
#include <vector>

#include <vulkan/vulkan.h>

#include "rasterization.hpp"
#include "starcore/gpu/gpu_renderer.hpp"
#include "starcore/render/particles.hpp"
#include "starcore/vulkan/pipeline.hpp"
#include "starcore/vulkan/resources.hpp"

namespace starcore::gpu
{

struct PostFrame
{
    const Camera* camera = nullptr;
    const Camera* previousCamera = nullptr;  // For the particles' streaks.
    PathTracerSettings settings;             // Exposure, tone mapper, sRGB.
    EffectSettings effects;
    bool raw = false;  // A debug image: clamped, no tone mapping, no effects.
    std::span<const RenderParticle> particles;
    // Reversed depth of the nearest surfaces (VisibilityPass); null: no particles.
    const vulkan::Image* depth = nullptr;
    // RGBA32F motion vectors and depths (motion_blur.glsl); null: no motion blur.
    const vulkan::Image* motion = nullptr;
    std::uint32_t frameSlot = 0;
};

class PostProcess
{
public:
    explicit PostProcess(vulkan::Context& context);

    /** @brief (Re)create the particle image and the bloom levels. */
    void resize(std::uint32_t width, std::uint32_t height);

    /**
     * @brief Record the effects and tone mapping of `radiance` (RGBA32F,
     *        written by compute shaders) into `output` (RGBA8 storage image).
     */
    void record(VkCommandBuffer commandBuffer, const PostFrame& frame,
                const vulkan::Image& radiance, const vulkan::Image& output);

private:
    /** @return Whether particles were drawn. */
    auto drawParticles(VkCommandBuffer commandBuffer, const PostFrame& frame) -> bool;
    void recordBloom(VkCommandBuffer commandBuffer, const PostFrame& frame,
                     const vulkan::Image& radiance, bool particles);
    void recordMotionTiles(VkCommandBuffer commandBuffer, const PostFrame& frame);

    vulkan::Context* m_context;
    vulkan::GraphicsPipeline m_particlePipeline;
    vulkan::ComputePipeline m_bloomPrefilter;
    vulkan::ComputePipeline m_bloomDown;
    vulkan::ComputePipeline m_bloomUp;
    vulkan::ComputePipeline m_motionTiles;
    vulkan::ComputePipeline m_motionNeighbors;
    vulkan::ComputePipeline m_final;
    vulkan::Sampler m_sampler;
    vulkan::Image m_particles;           // RGBA16F, cleared and drawn each frame.
    std::vector<vulkan::Image> m_bloom;  // RGBA16F, half the size of the one before.
    vulkan::Image m_tileBlur;            // RGBA32F per motion tile: its largest blur...
    vulkan::Image m_neighborBlur;        // ... and its neighborhood's.
    std::array<vulkan::Buffer, kFramesInFlight> m_uniforms;
    std::array<vulkan::Buffer, kFramesInFlight> m_particleBuffers;
};

}  // namespace starcore::gpu
