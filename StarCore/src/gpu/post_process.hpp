#pragma once
// The last stage of every GpuRenderer pipeline (private). Per frame:
//   0. exposure (exposure_histogram/adapt.comp): with auto exposure, a histogram of the
//      frame's luminance metered and the exposure adapted; else the settings' exposure. The
//      passes below read it from a buffer (it never comes back to the CPU);
//   1. particles (particle.vert/.frag): additive sprites, streaked along their
//      screen motion, into a particle image, tested against the visibility
//      buffer's depth (at the rendered size: lower when the frame is upscaled);
//      and the space sky's stars (star.vert/.frag), points through a lens where no surface
//      is, at the output's size (temporal upscalers would smear or drop sub-pixel points);
//   2. bloom (bloom_prefilter/down/up.comp): the frame downsampled into a
//      chain of half-size levels, then upsampled back summing them;
//   3. motion tiles (motion_tiles/neighbors.comp): the largest blur per 16x16
//      tile and its neighborhood;
//   4. light shafts (light_shafts.comp): dust lit by the sun along the view rays, traced;
//   5. how much of the sun is seen (lens_visibility.comp), for
//   6. final (post.comp): motion blur of the radiance (McGuire's
//      reconstruction filter), chromatic aberration, particles, the shafts, bloom, the sun's
//      flares (lens.glsl), vignette, tone mapping into the RGBA8 output, film grain.

#include <array>
#include <cstdint>
#include <memory>
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
    // Auto exposure: seconds since the last frame (adaptation), or jump to the metered
    // exposure.
    float seconds = 0.0F;
    bool resetExposure = false;
    bool raw = false;  // A debug image: clamped, no tone mapping, no effects.
    std::span<const RenderParticle> particles;
    // The space sky (RenderScene::sky), whose stars are drawn with the particles; null:
    // none.
    const RenderSky* sky = nullptr;
    // Reversed depth of the nearest surfaces (VisibilityPass); null: no particles, no stars.
    const vulkan::Image* depth = nullptr;
    // RGBA32F motion vectors and depths (motion_blur.glsl); null: no motion blur.
    const vulkan::Image* motion = nullptr;
    // The size of those (rendered) over the output's: less than 1 when upscaled.
    Vec2 renderScale{1.0F};
    // The scene's lights (RenderScene::lights): the sun of the lens effects is among them.
    std::span<const RenderLight> lights;
    // What light shafts are traced against; none: no shafts.
    VkAccelerationStructureKHR tlas = VK_NULL_HANDLE;
    std::uint32_t instanceMask = kAllInstances;
    std::uint32_t frameSlot = 0;
};

/** @brief A sun on the image (the lens effects'). */
struct LensSun
{
    Vec2 uv{0.5F};
    float radius = 0.0F;     // Of its disc, image heights.
    Vec3 color{1.0F};        // Brightest component 1.
    float luminance = 0.0F;  // Of its radiance.
    Vec3 direction{0.0F, 0.0F, -1.0F};  // From the camera, unit.
    Vec3 irradiance{0.0F};              // At the camera.
    bool inFront = false;               // Of the camera: `uv` and `radius` mean something.
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
    /** @brief Into m_exposure: the exposure of the frame. */
    void recordExposure(VkCommandBuffer commandBuffer, const PostFrame& frame,
                        const vulkan::Image& radiance);
    /** @return Whether particles or stars were drawn. */
    auto drawParticles(VkCommandBuffer commandBuffer, const PostFrame& frame) -> bool;
    /** @brief The stars of `map` on the GPU (uploaded when it is another map). */
    void syncStars(const std::shared_ptr<const SkyMap>& map);
    void recordBloom(VkCommandBuffer commandBuffer, const vulkan::Image& radiance,
                     bool particles);
    void recordMotionTiles(VkCommandBuffer commandBuffer, const PostFrame& frame);
    /** @brief Into m_lensState: how much of the sun shows, from `sun` (its place). */
    void recordLensVisibility(VkCommandBuffer commandBuffer, const PostFrame& frame,
                              const vulkan::Image& radiance, const LensSun& sun);
    /** @brief Into m_shafts[m_shaftTarget]: the sun's light on the dust along the view. */
    void recordLightShafts(VkCommandBuffer commandBuffer, const PostFrame& frame,
                           const LensSun& sun);
    void writeFrameUniforms(const PostFrame& frame);
    /** @brief post.comp's bloomScale: the bloom levels' sum to an average. */
    [[nodiscard]] auto bloomScale() const -> float;

    vulkan::Context* m_context;
    vulkan::GraphicsPipeline m_particlePipeline;
    vulkan::GraphicsPipeline m_starPipeline;
    vulkan::ComputePipeline m_exposureHistogram;
    vulkan::ComputePipeline m_exposureAdapt;
    vulkan::ComputePipeline m_bloomPrefilter;
    vulkan::ComputePipeline m_bloomDown;
    vulkan::ComputePipeline m_bloomUp;
    vulkan::ComputePipeline m_motionTiles;
    vulkan::ComputePipeline m_motionNeighbors;
    vulkan::ComputePipeline m_lensVisibility;
    vulkan::ComputePipeline m_final;
    std::unique_ptr<vulkan::ComputePipeline> m_lightShafts;  // Created when first used.
    vulkan::Sampler m_sampler;
    vulkan::Sampler m_nearest;  // The depth.
    vulkan::Image m_particles;           // RGBA16F, cleared and drawn each frame.
    std::vector<vulkan::Image> m_bloom;  // RGBA16F, half the size of the one before.
    std::array<vulkan::Image, 2> m_shafts;  // RGBA16F, a quarter of the size: this frame's, the last.
    std::uint32_t m_shaftTarget = 0;
    bool m_shaftHistory = false;  // The other one holds the last frame's.
    vulkan::Image m_tileBlur;            // RGBA32F per motion tile: its largest blur...
    vulkan::Image m_neighborBlur;        // ... and its neighborhood's.
    std::array<vulkan::Buffer, kFramesInFlight> m_uniforms;
    std::array<vulkan::Buffer, kFramesInFlight> m_particleBuffers;
    vulkan::Buffer m_histogram;  // kExposureBins counts.
    vulkan::Buffer m_exposure;   // exposure_adapt.comp's state: the exposure first.
    vulkan::Buffer m_lensState;  // lens_visibility.comp's.
    std::uint32_t m_frames = 0;  // Recorded so far: the film grain's seed.
    vulkan::Buffer m_stars;                       // SkyStar[] of m_starSource.
    std::shared_ptr<const SkyMap> m_starSource;
};

}  // namespace starcore::gpu
