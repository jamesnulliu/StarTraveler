#include <algorithm>
#include <cmath>
#include <optional>

#include <glm/geometric.hpp>
#include <glm/matrix.hpp>

#include "post_process.hpp"
#include "starcore/render/exposure.hpp"
#include "starcore/vulkan/context.hpp"

namespace starcore::gpu
{

namespace
{

#include "shaders/bloom_down.comp.spv.h"
#include "shaders/bloom_prefilter.comp.spv.h"
#include "shaders/bloom_up.comp.spv.h"
#include "shaders/exposure_adapt.comp.spv.h"
#include "shaders/exposure_histogram.comp.spv.h"
#include "shaders/lens_visibility.comp.spv.h"
#include "shaders/light_shafts.comp.spv.h"
#include "shaders/motion_neighbors.comp.spv.h"
#include "shaders/motion_tiles.comp.spv.h"
#include "shaders/particle.frag.spv.h"
#include "shaders/particle.vert.spv.h"
#include "shaders/post.comp.spv.h"
#include "shaders/star.frag.spv.h"
#include "shaders/star.vert.spv.h"

constexpr VkFormat kParticleFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
constexpr VkFormat kBloomFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
constexpr std::uint32_t kMaxBloomLevels = 6;
constexpr std::uint32_t kMinBloomSize = 8;  // Pixels of the smallest level.
constexpr float kBloomFalloff = 0.6F;       // bloom_up.comp's: each wider level's weight.
constexpr std::uint32_t kMotionTile = 16;   // motion_blur.glsl.
// The reconstruction filter looks one tile around: larger blurs would be cut.
constexpr float kMaxBlurRadius = 1.5F * static_cast<float>(kMotionTile);

constexpr VkDescriptorType kBuffer = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
constexpr VkDescriptorType kImage = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
constexpr VkDescriptorType kSampled = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
// particle.vert/.frag and star.vert/.frag: 1 frame uniforms, 2 the exposure (stars), 3
// particles or stars, 4 the surfaces' depth (0 unused).
constexpr VkDescriptorType kParticleBindings[] = {kBuffer, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                                                  kBuffer, kBuffer, kSampled};
constexpr VkDescriptorType kHistogramBindings[] = {kImage, kBuffer};
constexpr VkDescriptorType kAdaptBindings[] = {kBuffer, kBuffer};
constexpr VkDescriptorType kPrefilterBindings[] = {kImage, kImage, kImage, kBuffer};
constexpr VkDescriptorType kResampleBindings[] = {kSampled, kImage};
constexpr VkDescriptorType kTileBindings[] = {kImage, kImage};
constexpr VkDescriptorType kFinalBindings[] = {kImage, kImage,  kImage,  kSampled, kImage,
                                               kImage, kBuffer, kBuffer, kSampled};
constexpr VkDescriptorType kLightShaftBindings[] = {
    VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, kSampled,
    kSampled, kImage};
constexpr VkDescriptorType kLensVisibilityBindings[] = {kImage, kBuffer};
constexpr std::uint32_t kHistogramGroup = 16;  // exposure_histogram.comp's local size.

/** @brief Push constants of exposure_adapt.comp. */
struct AdaptParams
{
    float seconds = 0.0F;
    std::uint32_t flags = 0;  // kAdaptAutomatic, kAdaptReset.
    float exposure = 1.0F;
    float padding = 0.0F;
};
constexpr std::uint32_t kAdaptAutomatic = 1U;
constexpr std::uint32_t kAdaptReset = 2U;

/** @brief Push constants of particle.vert/.frag. */
struct ParticleParams
{
    float shutter = 0.0F;
    float padding = 0.0F;
    Vec2 depthScale{1.0F};  // The depth's size over the particle image's.
};

/** @brief Push constants of star.vert (star.frag: the first 16 bytes). */
struct StarParams
{
    float shutter = 0.0F;
    float brightness = 1.0F;
    Vec2 depthScale{1.0F};
    Vec4 galacticToWorld[3] = {};  // Rows.
    // The planets' discs, which hide the stars: the direction to the center from the origin,
    // the cosine of the angular radius (above 1: none).
    Vec4 planets[kMaxPlanets] = {Vec4(0.0F, 0.0F, 1.0F, 2.0F), Vec4(0.0F, 0.0F, 1.0F, 2.0F),
                                 Vec4(0.0F, 0.0F, 1.0F, 2.0F), Vec4(0.0F, 0.0F, 1.0F, 2.0F)};
};
static_assert(sizeof(StarParams) <= 128, "Vulkan's guaranteed push constant size");

/** @brief Push constants of bloom_prefilter.comp. */
struct PrefilterParams
{
    std::uint32_t particles = 0;
    std::uint32_t padding = 0;
};

/** @brief Push constants of motion_tiles.comp. */
struct TileParams
{
    float shutter = 0.0F;
    float maxRadius = 0.0F;
    Vec2 motionScale{1.0F};  // The motion image's size over the output's.
};

/** @brief Push constants of lens_visibility.comp. */
struct LensVisibilityParams
{
    Vec2 sun{0.5F};
    float radius = 0.0F;
    float threshold = 0.0F;
    float seconds = 0.0F;
    std::uint32_t reset = 0;
};

/** @brief Push constants of light_shafts.comp. */
struct LightShaftParams
{
    Vec4 sunDirection{0.0F};
    Vec4 sunIrradiance{0.0F};  // w: the dust's scattering per unit of length.
    std::uint32_t frame = 0;
    std::uint32_t mask = kAllInstances;
    float historyWeight = 0.0F;
    float padding = 0.0F;
};

/** @brief Push constants of post.comp. */
struct FinalParams
{
    float grain = 0.0F;
    std::uint32_t toneMapper = 0;
    std::uint32_t flags = 0;
    float bloomStrength = 0.0F;
    float bloomScale = 1.0F;
    float shutter = 0.0F;
    float maxRadius = 0.0F;
    float vignette = 0.0F;
    Vec2 motionScale{1.0F};  // The motion image's size over the output's.
    float chromatic = 0.0F;
    std::uint32_t frame = 0;
    Vec4 sun{0.5F, 0.5F, 0.0F, 1.0F};  // uv, unused, the image's aspect.
    Vec4 sunLight{1.0F};               // Its color, how exposed its disc is (0 to 1).
    Vec4 lens{0.0F};                   // Ghosts, starburst, anamorphic streak, dirt.
    float halo = 0.0F;
};
static_assert(sizeof(FinalParams) <= 128, "Vulkan's guaranteed push constant size");

auto blurRadius(const EffectSettings& effects) -> float
{
    return std::clamp(0.5F * effects.maxBlur, 0.5F, kMaxBlurRadius);
}

// post.comp flags.
constexpr std::uint32_t kPostRaw = 1U;
constexpr std::uint32_t kPostSrgb = 2U;
constexpr std::uint32_t kPostParticles = 4U;
constexpr std::uint32_t kPostMotionBlur = 8U;
constexpr std::uint32_t kPostBloom = 16U;
constexpr std::uint32_t kPostLens = 32U;
constexpr std::uint32_t kPostShafts = 64U;

// Of light shafts at strength 1: the dust's scattering per unit of length, and how much of
// each frame's light is the last frames' average.
constexpr float kDustScattering = 0.00045F;
constexpr float kShaftHistory = 0.88F;

// Of the chromatic aberration's strength 1: uv shift of red and blue in the corners, half
// of it from the corners' distance (post.comp).
constexpr float kChromaticShift = 0.0028F;
// A sphere light seen from the camera as large as this (sine of the angular radius), or as
// near, is a lamp nearby, not a sun.
constexpr float kLargestSun = 0.2F;
constexpr float kNearestSun = 200.0F;
// The exposed luminance of a sun's disc from which its flares have their full strength.
constexpr float kFullFlare = 15.0F;

auto lightLuminance(Vec3 color) -> float
{
    return glm::dot(color, Vec3(0.2126F, 0.7152F, 0.0722F));
}

/** @brief The sphere light that lights the camera most. */
auto findSun(std::span<const RenderLight> lights, const Camera& camera) -> std::optional<LensSun>
{
    Vec4 position;
    Vec4 forward;
    Vec4 right;
    Vec4 up;
    cameraBasis(camera, position, forward, right, up);
    const RenderLight* brightest = nullptr;
    float most = 0.0F;
    float brightestSine = 0.0F;
    for (const RenderLight& light : lights) {
        if (light.triangleCount != 0) {
            break;  // The meshes, after the spheres.
        }
        const Vec3 toLight = light.center - Vec3(position);
        const float distance2 = glm::dot(toLight, toLight);
        const float sine2 = light.radius * light.radius / std::max(distance2, 1e-6F);
        const float irradiance = lightLuminance(light.emission) * sine2;
        if (sine2 < kLargestSun * kLargestSun && distance2 > kNearestSun * kNearestSun &&
            irradiance > most) {
            most = irradiance;
            brightest = &light;
            brightestSine = std::sqrt(sine2);
        }
    }
    if (brightest == nullptr) {
        return std::nullopt;
    }
    const Vec3 toSun = brightest->center - Vec3(position);
    const Vec3 emission = brightest->emission;
    LensSun sun{.color = emission / std::max({emission.r, emission.g, emission.b}),
                .luminance = lightLuminance(emission),
                .direction = glm::normalize(toSun),
                .irradiance = emission * (kPi * brightestSine * brightestSine)};
    const float depth = glm::dot(toSun, Vec3(forward));
    if (depth > 0.0F) {
        // Camera rays: forward + right * x + up * y, from image x, y of -1 to 1 (y up).
        const float x = glm::dot(toSun, Vec3(right)) / (glm::dot(right, right) * depth);
        const float y = glm::dot(toSun, Vec3(up)) / (glm::dot(up, up) * depth);
        const float tangent = brightestSine / std::sqrt(1.0F - brightestSine * brightestSine);
        sun.uv = {0.5F + 0.5F * x, 0.5F - 0.5F * y};
        sun.radius = tangent / (2.0F * glm::length(Vec3(up)));
        sun.inFront = true;
    }
    return sun;
}

auto groups(std::uint32_t size) -> std::uint32_t
{
    return (size + kWorkgroupSize - 1) / kWorkgroupSize;
}

void computeBarrier(VkCommandBuffer commandBuffer)
{
    vulkan::memoryBarrier(commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                          VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                          VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
}

}  // namespace

PostProcess::PostProcess(vulkan::Context& context)
    : m_context(&context),
      // The fragment shader tests the depth (it may be smaller than the particle image).
      m_particlePipeline(context, {.vertexSpirv = particle_vert_spv,
                                   .fragmentSpirv = particle_frag_spv,
                                   .bindings = kParticleBindings,
                                   .pushConstantSize = sizeof(ParticleParams),
                                   .colorFormats = std::span(&kParticleFormat, 1),
                                   .additiveBlending = true}),
      m_starPipeline(context, {.vertexSpirv = star_vert_spv,
                               .fragmentSpirv = star_frag_spv,
                               .bindings = kParticleBindings,
                               .pushConstantSize = sizeof(StarParams),
                               .colorFormats = std::span(&kParticleFormat, 1),
                               .additiveBlending = true}),
      m_exposureHistogram(context, exposure_histogram_comp_spv, kHistogramBindings),
      m_exposureAdapt(context, exposure_adapt_comp_spv, kAdaptBindings,
                      static_cast<std::uint32_t>(sizeof(AdaptParams))),
      m_bloomPrefilter(context, bloom_prefilter_comp_spv, kPrefilterBindings,
                       static_cast<std::uint32_t>(sizeof(PrefilterParams))),
      m_bloomDown(context, bloom_down_comp_spv, kResampleBindings),
      m_bloomUp(context, bloom_up_comp_spv, kResampleBindings),
      m_motionTiles(context, motion_tiles_comp_spv, kTileBindings,
                    static_cast<std::uint32_t>(sizeof(TileParams))),
      m_motionNeighbors(context, motion_neighbors_comp_spv, kTileBindings),
      m_lensVisibility(context, lens_visibility_comp_spv, kLensVisibilityBindings,
                       static_cast<std::uint32_t>(sizeof(LensVisibilityParams))),
      m_final(context, post_comp_spv, kFinalBindings,
              static_cast<std::uint32_t>(sizeof(FinalParams))),
      m_sampler(context), m_nearest(context, VK_FILTER_NEAREST)
{
    constexpr VkBufferUsageFlags kUsage =
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    const std::array<std::uint32_t, kExposureBins> zeros{};
    m_histogram = vulkan::createDeviceBuffer(context, std::as_bytes(std::span(zeros)), kUsage);
    // No exposure yet: the first frame meters it.
    m_exposure = vulkan::createDeviceBuffer(
        context, std::as_bytes(std::span(zeros).first(4)), kUsage);
    m_lensState = vulkan::createDeviceBuffer(
        context, std::as_bytes(std::span(zeros).first(1)), kUsage);
}

void PostProcess::resize(std::uint32_t width, std::uint32_t height)
{
    m_particles = {};
    m_bloom.clear();
    m_shafts = {};
    m_shaftHistory = false;
    m_tileBlur = {};
    m_neighborBlur = {};
    if (width == 0 || height == 0) {
        return;
    }
    constexpr VkImageUsageFlags kReadWrite = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    m_particles = vulkan::Image(*m_context, width, height, kParticleFormat,
                                kReadWrite | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
    const std::uint32_t tilesX = (width + kMotionTile - 1) / kMotionTile;
    const std::uint32_t tilesY = (height + kMotionTile - 1) / kMotionTile;
    for (vulkan::Image* tiles : {&m_tileBlur, &m_neighborBlur}) {
        *tiles = vulkan::Image(*m_context, tilesX, tilesY, VK_FORMAT_R32G32B32A32_SFLOAT,
                               VK_IMAGE_USAGE_STORAGE_BIT);
    }
    for (std::uint32_t level = 0; level < kMaxBloomLevels; ++level) {
        const std::uint32_t levelWidth = width >> (level + 1);
        const std::uint32_t levelHeight = height >> (level + 1);
        if (std::min(levelWidth, levelHeight) < kMinBloomSize) {
            break;
        }
        m_bloom.emplace_back(*m_context, levelWidth, levelHeight, kBloomFormat, kReadWrite);
    }
    for (vulkan::Image& shafts : m_shafts) {
        shafts = vulkan::Image(*m_context, std::max(width / 4, 1U), std::max(height / 4, 1U),
                               kBloomFormat, kReadWrite);
    }
    // GENERAL from now on.
    m_context->submitAndWait([&](VkCommandBuffer commandBuffer) {
        auto toGeneral = [&](const vulkan::Image& image) {
            vulkan::transitionImage(commandBuffer, image.handle(), VK_IMAGE_LAYOUT_UNDEFINED,
                                    VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0,
                                    VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                    VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);
        };
        toGeneral(m_particles);
        toGeneral(m_tileBlur);
        toGeneral(m_neighborBlur);
        toGeneral(m_shafts[0]);
        toGeneral(m_shafts[1]);
        for (const vulkan::Image& level : m_bloom) {
            toGeneral(level);
        }
    });
}

void PostProcess::syncStars(const std::shared_ptr<const SkyMap>& map)
{
    if (map == m_starSource) {
        return;
    }
    // Frames in flight may still read the old buffer (a new sky is rare).
    m_context->waitIdle();
    m_stars = {};
    m_starSource = map;
    if (map && !map->stars.empty()) {
        m_stars = vulkan::createDeviceBuffer(
            *m_context, std::as_bytes(std::span<const SkyStar>(map->stars)),
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    }
}

void PostProcess::writeFrameUniforms(const PostFrame& frame)
{
    FrameUniforms uniforms{};
    cameraBasis(*frame.camera, uniforms.cameraPosition, uniforms.cameraForward,
                uniforms.cameraRight, uniforms.cameraUp);
    cameraBasis(*frame.previousCamera, uniforms.previousPosition, uniforms.previousForward,
                uniforms.previousRight, uniforms.previousUp);
    uniforms.cameraPosition.w = kNearPlane;
    uniforms.viewport = Vec4(static_cast<float>(m_particles.width()),
                             static_cast<float>(m_particles.height()), 0.5F, 0.5F);
    writeUniforms(*m_context, m_uniforms[frame.frameSlot], uniforms);
}

void PostProcess::recordExposure(VkCommandBuffer commandBuffer, const PostFrame& frame,
                                 const vulkan::Image& radiance)
{
    const bool automatic = frame.settings.autoExposure && !frame.raw;
    if (automatic) {
        vkCmdFillBuffer(commandBuffer, m_histogram.handle(), 0, VK_WHOLE_SIZE, 0);
        // The radiance written by the pipeline (or the upscaler), the histogram cleared.
        vulkan::memoryBarrier(commandBuffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                              VK_ACCESS_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                              VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
        vulkan::DescriptorWrites writes;
        writes.storageImage(0, radiance.view()).storageBuffer(1, m_histogram.handle());
        m_exposureHistogram.bind(commandBuffer);
        m_exposureHistogram.pushDescriptors(commandBuffer, writes.writes());
        vkCmdDispatch(commandBuffer, (radiance.width() + kHistogramGroup - 1) / kHistogramGroup,
                      (radiance.height() + kHistogramGroup - 1) / kHistogramGroup, 1);
        computeBarrier(commandBuffer);
    }
    vulkan::DescriptorWrites writes;
    writes.storageBuffer(0, m_histogram.handle()).storageBuffer(1, m_exposure.handle());
    const AdaptParams params{
        .seconds = frame.seconds,
        .flags = (automatic ? kAdaptAutomatic : 0U) | (frame.resetExposure ? kAdaptReset : 0U),
        .exposure = frame.settings.exposure};
    m_exposureAdapt.bind(commandBuffer);
    m_exposureAdapt.pushDescriptors(commandBuffer, writes.writes());
    m_exposureAdapt.pushConstants(commandBuffer, &params, sizeof(params));
    vkCmdDispatch(commandBuffer, 1, 1, 1);
    vulkan::memoryBarrier(commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                          VK_ACCESS_SHADER_WRITE_BIT,
                          VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                          VK_ACCESS_SHADER_READ_BIT);
}

auto PostProcess::drawParticles(VkCommandBuffer commandBuffer, const PostFrame& frame) -> bool
{
    if (frame.depth == nullptr || frame.raw) {
        return false;
    }
    syncStars(frame.sky != nullptr ? frame.sky->map : nullptr);
    const bool stars = m_stars.isValid();
    if (frame.particles.empty() && !stars) {
        return false;
    }
    const std::uint32_t width = m_particles.width();
    const std::uint32_t height = m_particles.height();

    // The slot's previous submission completed: its buffer may be rewritten.
    vulkan::Buffer& buffer = m_particleBuffers[frame.frameSlot];
    if (!frame.particles.empty()) {
        const auto bytes = std::as_bytes(frame.particles);
        if (!buffer.isValid() || buffer.size() < bytes.size()) {
            buffer = vulkan::Buffer(*m_context,
                                    std::max<VkDeviceSize>(bytes.size(), buffer.size() * 2),
                                    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                    vulkan::MemoryUsage::Upload);
        }
        buffer.write(bytes);
    }

    // The visibility pass wrote the depth (and copied it).
    vulkan::memoryBarrier(commandBuffer,
                          VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                          VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                          VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);

    VkRenderingAttachmentInfo color{};
    color.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    color.imageView = m_particles.view();
    color.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo rendering{};
    rendering.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    rendering.renderArea = {{0, 0}, {width, height}};
    rendering.layerCount = 1;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachments = &color;
    vkCmdBeginRendering(commandBuffer, &rendering);
    const VkViewport viewport{0.0F, 0.0F, static_cast<float>(width), static_cast<float>(height),
                              0.0F, 1.0F};
    const VkRect2D scissor{{0, 0}, {width, height}};
    vkCmdSetViewport(commandBuffer, 0, 1, &viewport);
    vkCmdSetScissor(commandBuffer, 0, 1, &scissor);

    const float shutter = frame.effects.motionBlur ? frame.effects.shutter : 0.0F;
    const Vec2 depthScale(static_cast<float>(frame.depth->width()) / static_cast<float>(width),
                          static_cast<float>(frame.depth->height()) / static_cast<float>(height));
    if (stars) {
        vulkan::DescriptorWrites writes;
        writes.uniformBuffer(1, m_uniforms[frame.frameSlot].handle())
            .storageBuffer(2, m_exposure.handle())
            .storageBuffer(3, m_stars.handle())
            .sampledImage(4, frame.depth->view(), m_nearest.handle());
        m_starPipeline.bind(commandBuffer);
        m_starPipeline.pushDescriptors(commandBuffer, writes.writes());
        StarParams params{
            .shutter = shutter, .brightness = frame.sky->brightness, .depthScale = depthScale};
        const Mat3 galacticToWorld = glm::transpose(frame.sky->worldToGalactic);
        for (int row = 0; row < 3; ++row) {
            params.galacticToWorld[row] = Vec4(galacticToWorld[0][row], galacticToWorld[1][row],
                                               galacticToWorld[2][row], 0.0F);
        }
        const std::size_t planets = std::min(frame.sky->planets.size(), kMaxPlanets);
        for (std::size_t i = 0; i < planets; ++i) {
            const RenderPlanet& planet = frame.sky->planets[i];
            const float distance = glm::length(planet.center);
            if (distance > planet.radius) {
                const float sine = planet.radius / distance;
                params.planets[i] =
                    Vec4(planet.center / distance, std::sqrt(1.0F - sine * sine));
            }
        }
        m_starPipeline.pushConstants(commandBuffer, &params, sizeof(params));
        vkCmdDraw(commandBuffer, static_cast<std::uint32_t>(6 * m_starSource->stars.size()), 1,
                  0, 0);
    }
    if (!frame.particles.empty()) {
        vulkan::DescriptorWrites writes;
        writes.uniformBuffer(1, m_uniforms[frame.frameSlot].handle())
            .storageBuffer(3, buffer.handle())
            .sampledImage(4, frame.depth->view(), m_nearest.handle());
        m_particlePipeline.bind(commandBuffer);
        m_particlePipeline.pushDescriptors(commandBuffer, writes.writes());
        const ParticleParams params{.shutter = shutter, .depthScale = depthScale};
        m_particlePipeline.pushConstants(commandBuffer, &params, sizeof(params));
        vkCmdDraw(commandBuffer, static_cast<std::uint32_t>(6 * frame.particles.size()), 1, 0,
                  0);
    }
    vkCmdEndRendering(commandBuffer);

    vulkan::memoryBarrier(commandBuffer, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                          VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                          VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
    return true;
}

void PostProcess::recordBloom(VkCommandBuffer commandBuffer, const vulkan::Image& radiance,
                              bool particles)
{
    {
        vulkan::DescriptorWrites writes;
        writes.storageImage(0, radiance.view())
            .storageImage(1, m_particles.view())
            .storageImage(2, m_bloom[0].view())
            .storageBuffer(3, m_exposure.handle());
        const PrefilterParams params{.particles = particles ? 1U : 0U};
        m_bloomPrefilter.bind(commandBuffer);
        m_bloomPrefilter.pushDescriptors(commandBuffer, writes.writes());
        m_bloomPrefilter.pushConstants(commandBuffer, &params, sizeof(params));
        vkCmdDispatch(commandBuffer, groups(m_bloom[0].width()), groups(m_bloom[0].height()), 1);
        computeBarrier(commandBuffer);
    }
    auto resample = [&](const vulkan::ComputePipeline& pipeline, const vulkan::Image& source,
                        const vulkan::Image& target) {
        vulkan::DescriptorWrites writes;
        writes.sampledImage(0, source.view(), m_sampler.handle()).storageImage(1, target.view());
        pipeline.bind(commandBuffer);
        pipeline.pushDescriptors(commandBuffer, writes.writes());
        vkCmdDispatch(commandBuffer, groups(target.width()), groups(target.height()), 1);
        computeBarrier(commandBuffer);
    };
    for (std::size_t level = 1; level < m_bloom.size(); ++level) {
        resample(m_bloomDown, m_bloom[level - 1], m_bloom[level]);
    }
    for (std::size_t level = m_bloom.size() - 1; level-- > 0;) {
        resample(m_bloomUp, m_bloom[level + 1], m_bloom[level]);
    }
}

void PostProcess::recordMotionTiles(VkCommandBuffer commandBuffer, const PostFrame& frame)
{
    {
        vulkan::DescriptorWrites writes;
        writes.storageImage(0, frame.motion->view()).storageImage(1, m_tileBlur.view());
        const TileParams params{.shutter = frame.effects.shutter,
                                .maxRadius = blurRadius(frame.effects),
                                .motionScale = frame.renderScale};
        m_motionTiles.bind(commandBuffer);
        m_motionTiles.pushDescriptors(commandBuffer, writes.writes());
        m_motionTiles.pushConstants(commandBuffer, &params, sizeof(params));
        vkCmdDispatch(commandBuffer, groups(m_tileBlur.width()), groups(m_tileBlur.height()), 1);
        computeBarrier(commandBuffer);
    }
    vulkan::DescriptorWrites writes;
    writes.storageImage(0, m_tileBlur.view()).storageImage(1, m_neighborBlur.view());
    m_motionNeighbors.bind(commandBuffer);
    m_motionNeighbors.pushDescriptors(commandBuffer, writes.writes());
    vkCmdDispatch(commandBuffer, groups(m_neighborBlur.width()), groups(m_neighborBlur.height()),
                  1);
    computeBarrier(commandBuffer);
}

void PostProcess::recordLensVisibility(VkCommandBuffer commandBuffer, const PostFrame& frame,
                                       const vulkan::Image& radiance, const LensSun& sun)
{
    vulkan::DescriptorWrites writes;
    writes.storageImage(0, radiance.view()).storageBuffer(1, m_lensState.handle());
    const LensVisibilityParams params{.sun = sun.uv,
                                      .radius = sun.radius,
                                      .threshold = 0.25F * sun.luminance,
                                      .seconds = frame.seconds,
                                      .reset = frame.resetExposure ? 1U : 0U};
    m_lensVisibility.bind(commandBuffer);
    m_lensVisibility.pushDescriptors(commandBuffer, writes.writes());
    m_lensVisibility.pushConstants(commandBuffer, &params, sizeof(params));
    vkCmdDispatch(commandBuffer, 1, 1, 1);
    computeBarrier(commandBuffer);
}

void PostProcess::recordLightShafts(VkCommandBuffer commandBuffer, const PostFrame& frame,
                                    const LensSun& sun)
{
    if (!m_lightShafts) {
        m_lightShafts = std::make_unique<vulkan::ComputePipeline>(
            *m_context, light_shafts_comp_spv, kLightShaftBindings,
            static_cast<std::uint32_t>(sizeof(LightShaftParams)));
    }
    m_shaftTarget = 1U - m_shaftTarget;
    const vulkan::Image& target = m_shafts[m_shaftTarget];
    const vulkan::Image& history = m_shafts[1U - m_shaftTarget];
    vulkan::DescriptorWrites writes;
    writes.accelerationStructure(0, frame.tlas)
        .uniformBuffer(1, m_uniforms[frame.frameSlot].handle())
        .sampledImage(2, frame.depth->view(), m_nearest.handle())
        .sampledImage(3, history.view(), m_sampler.handle())
        .storageImage(4, target.view());
    const LightShaftParams params{
        .sunDirection = Vec4(sun.direction, 0.0F),
        .sunIrradiance = Vec4(sun.irradiance, kDustScattering * frame.effects.lightShafts),
        .frame = m_frames,
        .mask = frame.instanceMask & kAllInstances,
        .historyWeight = m_shaftHistory ? kShaftHistory : 0.0F};
    m_lightShafts->bind(commandBuffer);
    m_lightShafts->pushDescriptors(commandBuffer, writes.writes());
    m_lightShafts->pushConstants(commandBuffer, &params, sizeof(params));
    vkCmdDispatch(commandBuffer, groups(target.width()), groups(target.height()), 1);
    computeBarrier(commandBuffer);
    m_shaftHistory = true;
}

auto PostProcess::bloomScale() const -> float
{
    // The levels' weights sum (1, kBloomFalloff, kBloomFalloff^2, ...) to an average.
    float sum = 0.0F;
    float weight = 1.0F;
    for (std::size_t level = 0; level < m_bloom.size(); ++level) {
        sum += weight;
        weight *= kBloomFalloff;
    }
    return sum > 0.0F ? 1.0F / sum : 1.0F;
}

void PostProcess::record(VkCommandBuffer commandBuffer, const PostFrame& frame,
                         const vulkan::Image& radiance, const vulkan::Image& output)
{
    const EffectSettings& effects = frame.effects;
    recordExposure(commandBuffer, frame, radiance);
    if (frame.depth != nullptr && !frame.raw) {
        writeFrameUniforms(frame);
    }
    const bool particles = drawParticles(commandBuffer, frame);
    // The pipeline's radiance and motion vectors.
    computeBarrier(commandBuffer);
    const bool bloom = effects.bloom && !frame.raw && !m_bloom.empty();
    if (bloom) {
        recordBloom(commandBuffer, radiance, particles);
    }
    const bool motionBlur = effects.motionBlur && !frame.raw && frame.motion != nullptr;
    if (motionBlur) {
        recordMotionTiles(commandBuffer, frame);
    }

    const bool wantsLens = !frame.raw && (effects.lensGhosts > 0.0F || effects.lensHalo > 0.0F ||
                                          effects.starburst > 0.0F || effects.anamorphic > 0.0F ||
                                          effects.lensDirt > 0.0F);
    const bool wantsShafts = !frame.raw && effects.lightShafts > 0.0F && frame.depth != nullptr &&
                             frame.tlas != VK_NULL_HANDLE;
    const std::optional<LensSun> sun =
        wantsLens || wantsShafts ? findSun(frame.lights, *frame.camera) : std::nullopt;
    const bool lens = wantsLens && sun && sun->inFront;
    if (lens) {
        recordLensVisibility(commandBuffer, frame, radiance, *sun);
    }
    const bool shafts = wantsShafts && sun;
    if (shafts) {
        recordLightShafts(commandBuffer, frame, *sun);
    } else {
        m_shaftHistory = false;  // Whatever it holds, it is not the last frame's.
    }

    FinalParams params{
        .grain = frame.raw ? 0.0F : effects.filmGrain,
        .toneMapper = static_cast<std::uint32_t>(frame.settings.toneMapper),
        .bloomStrength = effects.bloomStrength,
        .bloomScale = bloomScale(),
        .shutter = effects.shutter,
        .maxRadius = blurRadius(effects),
        .vignette = frame.raw ? 0.0F : std::clamp(effects.vignette, 0.0F, 1.0F),
        .motionScale = frame.renderScale,
        .chromatic = frame.raw ? 0.0F : effects.chromaticAberration * kChromaticShift,
        .frame = m_frames++,
    };
    params.flags = (frame.raw ? kPostRaw : 0U) | (frame.settings.srgbOutput ? kPostSrgb : 0U) |
                   (particles ? kPostParticles : 0U) | (motionBlur ? kPostMotionBlur : 0U) |
                   (bloom ? kPostBloom : 0U) | (lens ? kPostLens : 0U) |
                   (shafts ? kPostShafts : 0U);
    if (lens) {
        const float aspect = static_cast<float>(output.width()) / static_cast<float>(output.height());
        params.sun = Vec4(sun->uv, 0.0F, aspect);
        // The exposure is the GPU's own: the sun's flares fade with the settings' only.
        const float exposed = sun->luminance * std::max(frame.settings.exposure, 1e-3F);
        params.sunLight = Vec4(sun->color, std::min(exposed / kFullFlare, 1.0F));
        params.lens = Vec4(effects.lensGhosts, effects.starburst, effects.anamorphic,
                           effects.lensDirt);
        params.halo = effects.lensHalo;
    }
    // Unused inputs still need valid descriptors of the right kind.
    vulkan::DescriptorWrites writes;
    writes.storageImage(0, radiance.view())
        .storageImage(1, m_particles.view())
        .storageImage(2, (motionBlur ? *frame.motion : radiance).view())
        .sampledImage(3, (m_bloom.empty() ? m_particles : m_bloom[0]).view(), m_sampler.handle())
        .storageImage(4, output.view())
        .storageImage(5, m_neighborBlur.view())
        .storageBuffer(6, m_exposure.handle())
        .storageBuffer(7, m_lensState.handle())
        .sampledImage(8, (shafts ? m_shafts[m_shaftTarget] : m_particles).view(),
                      m_sampler.handle());
    m_final.bind(commandBuffer);
    m_final.pushDescriptors(commandBuffer, writes.writes());
    m_final.pushConstants(commandBuffer, &params, sizeof(params));
    vkCmdDispatch(commandBuffer, groups(output.width()), groups(output.height()), 1);
}

}  // namespace starcore::gpu
