#include <algorithm>
#include <cmath>
#include <functional>
#include <span>
#include <utility>

#include <glm/common.hpp>
#include <glm/geometric.hpp>

#include "shadow_maps.hpp"
#include "starcore/render/sampling.hpp"
#include "starcore/vulkan/context.hpp"

namespace starcore::gpu
{

namespace
{

#include "shaders/shadow.vert.spv.h"
#include "shaders/shadow_sphere.frag.spv.h"

constexpr VkFormat kDepthFormat = VK_FORMAT_D32_SFLOAT;
constexpr std::uint32_t kAtlasTiles = 4;  // Per side.
static_assert(kAtlasTiles * kAtlasTiles == kMaxShadowViews);
constexpr std::uint32_t kMinResolution = 128;
constexpr std::uint32_t kMaxResolution = 2048;  // An atlas of 256 MB.

// Bindings of the shadow pass: 2 the transforms, 4 the instances (hybrid.glsl,
// scene.glsl), 13 the records (shadows.glsl); the others unused.
constexpr VkDescriptorType kBuffer = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
constexpr VkDescriptorType kUniform = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
constexpr VkDescriptorType kShadowBindings[] = {kBuffer, kUniform, kBuffer, kBuffer, kBuffer,
                                                kBuffer, kBuffer,  kBuffer, kBuffer, kBuffer,
                                                kBuffer, kBuffer,  kBuffer, kUniform};

// Lights nearer the camera than this many shadow distances get a cube.
constexpr float kCubeReach = 1.25F;
constexpr std::uint32_t kCubeViews = 6;
constexpr std::uint32_t kCascades = 3;
constexpr float kCascadeRatio = 4.0F;  // Radius of a cascade over the next finer one's.
// A far light's reference direction follows the camera once the cascades'
// centers are this far off it (tangent): rarely, as it moves their texels.
constexpr float kMaxReferenceOffset = 0.25F;
// Near planes at this fraction of the light's radius: all of a cube face is
// beyond the light's surface (its corners are at 1/sqrt(3) of the distance).
constexpr float kNearFraction = 0.5F;
// Surfaces letting at least this much light through cast no shadow
// (isSeeThrough in path_tracer.cpp and scene.glsl).
constexpr float kSeeThrough = 0.5F;

struct DrawConstants
{
    std::uint32_t instanceIndex;
    std::uint32_t viewIndex;
};

auto pipelineConfig(std::span<const std::uint32_t> fragmentSpirv)
    -> vulkan::GraphicsPipelineConfig
{
    return {.vertexSpirv = shadow_vert_spv,
            .fragmentSpirv = fragmentSpirv,
            .bindings = kShadowBindings,
            .pushConstantSize = sizeof(DrawConstants),
            .depthFormat = kDepthFormat};
}

auto castsShadow(const RenderMaterial& m) -> bool
{
    return (m.flags & kMaterialThinWalled) == 0U ||
           m.transmission * (1.0F - m.metallic) < kSeeThrough;
}

/** @brief A unit vector perpendicular to a unit vector. */
auto perpendicular(Vec3 v) -> Vec3
{
    const Vec3 axis = std::abs(v.y) < 0.9F ? Vec3(0.0F, 1.0F, 0.0F) : Vec3(1.0F, 0.0F, 0.0F);
    return glm::normalize(glm::cross(axis, v));
}

/** @brief Orthonormal axes of a transform's rotation (world axes if degenerate). */
auto rotationAxes(const Mat4& m) -> std::array<Vec3, 3>
{
    const Vec3 a(m[0]);
    const Vec3 b(m[1]);
    if (glm::length(glm::cross(a, b)) < 1e-12F) {
        return {Vec3(1.0F, 0.0F, 0.0F), Vec3(0.0F, 1.0F, 0.0F), Vec3(0.0F, 0.0F, 1.0F)};
    }
    const Vec3 x = glm::normalize(a);
    const Vec3 y = glm::normalize(b - glm::dot(b, x) * x);
    return {x, y, glm::cross(x, y)};
}

/** @brief Rounded up to an eighth of an octave: a view's size changes in steps. */
auto quantizeUp(float value) -> float
{
    return std::exp2(std::ceil(std::log2(value) * 8.0F) / 8.0F);
}

/** @brief Whether a view may see any of a sphere (center, radius). */
auto viewSees(const ShadowViewRecord& view, Vec3 center, float radius) -> bool
{
    const Vec3 d = center - Vec3(view.position);
    const Vec3 forward(view.forward);
    if (glm::dot(d, forward) + radius < view.position.w) {
        return false;
    }
    // The side planes through the light (outward normals).
    const float half = 1.0F / view.forward.w;
    for (const auto& [axis, middle] :
         {std::pair{Vec3(view.right), view.right.w}, std::pair{Vec3(view.up), view.up.w}}) {
        for (const float side : {1.0F, -1.0F}) {
            const Vec3 normal = side * (axis - (middle + side * half) * forward);
            if (glm::dot(d, normal) > radius * glm::length(normal)) {
                return false;
            }
        }
    }
    return true;
}

}  // namespace

ShadowMaps::ShadowMaps(vulkan::Context& context)
    : m_context(&context), m_triangles(context, pipelineConfig({})),
      m_spheres(context, pipelineConfig(shadow_sphere_frag_spv)),
      m_sampler(context, VK_FILTER_NEAREST)
{
    resizeAtlas(0);  // A placeholder until shadows are drawn.
}

void ShadowMaps::resizeAtlas(std::uint32_t resolution)
{
    if (m_atlas.isValid()) {
        m_context->waitIdle();  // Earlier frames may sample it.
    }
    const std::uint32_t size = std::max(1U, kAtlasTiles * resolution);
    m_atlas = vulkan::Image(*m_context, size, size, kDepthFormat,
                            VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                                VK_IMAGE_USAGE_SAMPLED_BIT);
    m_context->submitAndWait([&](VkCommandBuffer commandBuffer) {
        vulkan::transitionImage(commandBuffer, m_atlas.handle(), VK_IMAGE_LAYOUT_UNDEFINED,
                                VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0,
                                VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
                                m_atlas.aspect());
    });
    m_resolution = resolution;
}

void ShadowMaps::record(VkCommandBuffer commandBuffer, std::uint32_t frameSlot,
                        const RenderScene& scene, const GpuScene& gpuScene, const Camera& camera,
                        std::uint32_t instanceMask, const ShadowSettings& settings)
{
    vulkan::Buffer& buffer = m_records[frameSlot];
    if (!buffer.isValid()) {
        buffer = vulkan::Buffer(*m_context, sizeof(ShadowRecords),
                                VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, vulkan::MemoryUsage::Upload);
    }
    ShadowRecords records;
    const std::uint32_t resolution =
        std::clamp(settings.resolution, kMinResolution, kMaxResolution);
    if (settings.enabled && settings.distance > 0.0F) {
        fitViews(scene, camera, settings.distance, resolution, records);
    }
    buffer.write(std::as_bytes(std::span(&records, 1)));
    if (records.lightCount == 0) {
        return;
    }
    if (m_resolution != resolution) {
        resizeAtlas(resolution);
    }
    draw(commandBuffer, buffer.handle(), scene, gpuScene, instanceMask, records);
}

void ShadowMaps::fitViews(const RenderScene& scene, const Camera& camera, float distance,
                          std::uint32_t resolution, ShadowRecords& records)
{
    struct Candidate
    {
        std::uint32_t light;
        float score;  // How brightly it may light the camera's surroundings.
        bool cube;
    };
    const Vec3 eye = camera.position();
    std::vector<Candidate> candidates;
    for (std::uint32_t i = 0; i < scene.lights.size(); ++i) {
        const RenderLight& light = scene.lights[i];
        const float brightness = sampling::luminance(light.emission);
        // Meshes are no lights to the Raster pipeline.
        if (light.triangleCount > 0 || light.radius <= 0.0F || brightness <= 0.0F) {
            continue;
        }
        const Vec3 toLight = light.center - eye;
        const float distance2 = glm::dot(toLight, toLight);
        const float radius2 = light.radius * light.radius;
        const float reach = kCubeReach * distance;
        candidates.push_back({i, brightness * radius2 / std::max(distance2, radius2),
                              distance2 < reach * reach});
    }
    std::ranges::stable_sort(candidates, std::greater{}, &Candidate::score);

    const auto texels = static_cast<float>(resolution);
    const float inverseAtlas = 1.0F / (static_cast<float>(kAtlasTiles) * texels);
    std::uint32_t viewCount = 0;
    m_viewLights.clear();
    auto addView = [&](const RenderLight& light, Vec3 forward, float scale, Vec3 right, Vec3 up,
                       Vec2 middle) {
        const std::uint32_t tile = viewCount++;
        records.views[tile] = ShadowViewRecord{
            .position = Vec4(light.center, kNearFraction * light.radius),
            .forward = Vec4(forward, scale),
            .right = Vec4(right, middle.x),
            .up = Vec4(up, middle.y),
            .tile = Vec4(static_cast<float>(tile % kAtlasTiles) * texels,
                         static_cast<float>(tile / kAtlasTiles) * texels, texels, inverseAtlas),
        };
        m_viewLights.push_back(light.instanceIndex);
    };

    std::unordered_map<std::uint32_t, Reference> references;
    for (const Candidate& candidate : candidates) {
        const std::uint32_t needed = candidate.cube ? kCubeViews : kCascades;
        if (records.lightCount == kMaxShadowLights) {
            break;
        }
        if (viewCount + needed > kMaxShadowViews) {
            continue;
        }
        const RenderLight& light = scene.lights[candidate.light];
        ShadowLightRecord& record = records.lights[records.lightCount++];
        record.lightIndex = candidate.light;
        record.firstView = viewCount;
        record.viewCount = needed;
        record.cube = candidate.cube ? 1U : 0U;

        if (candidate.cube) {
            // Along the light's own axes: a light moving with what it lights
            // keeps its texels where they were on it. Each face sees two
            // texels beyond its edges (lookups near them gather 2x2 texels).
            const std::array<Vec3, 3> axes =
                rotationAxes(scene.instances[light.instanceIndex].objectToWorld);
            record.axisX = Vec4(axes[0], 0.0F);
            record.axisY = Vec4(axes[1], 0.0F);
            record.axisZ = Vec4(axes[2], 0.0F);
            const float scale = (texels - 4.0F) / texels;
            for (std::uint32_t axis = 0; axis < 3; ++axis) {
                for (const float sign : {1.0F, -1.0F}) {
                    addView(light, sign * axes[axis], scale, axes[(axis + 1) % 3],
                            axes[(axis + 2) % 3], Vec2(0.0F));
                }
            }
            continue;
        }

        // Cascades: spheres around the camera of growing radius, as seen from
        // the light, on the plane of a reference direction, their centers
        // moved by whole texels (and their sizes in steps) as the camera moves.
        const Vec3 toEye = eye - light.center;
        const float eyeDistance = glm::length(toEye);
        const Vec3 direction = toEye / eyeDistance;
        Reference reference = m_references.contains(light.instanceIndex)
                                  ? m_references[light.instanceIndex]
                                  : Reference{};
        auto offset = [&] {
            return Vec2(glm::dot(direction, reference.right), glm::dot(direction, reference.up)) /
                   glm::dot(direction, reference.forward);
        };
        if (reference.center != light.center || glm::dot(direction, reference.forward) <= 0.5F ||
            glm::max(glm::abs(offset().x), glm::abs(offset().y)) > kMaxReferenceOffset) {
            const Vec3 right = perpendicular(direction);
            reference = {light.center, direction, right, glm::cross(direction, right)};
        }
        references[light.instanceIndex] = reference;
        const Vec2 center = offset();
        // A tangent across a sphere off the reference spans more: 1 / cos^2.
        const float stretch = 1.0F + glm::dot(center, center);
        for (std::uint32_t level = 0; level < kCascades; ++level) {
            const float radius =
                distance / std::pow(kCascadeRatio, static_cast<float>(kCascades - 1 - level));
            const float tangent = quantizeUp(
                1.02F * stretch * radius /
                std::sqrt(eyeDistance * eyeDistance - radius * radius));
            const float texel = 2.0F * tangent / texels;
            addView(light, reference.forward, 1.0F / tangent, reference.right, reference.up,
                    glm::round(center / texel) * texel);
        }
    }
    m_references = std::move(references);  // Forget lights without cascades now.
}

void ShadowMaps::draw(VkCommandBuffer commandBuffer, VkBuffer records, const RenderScene& scene,
                      const GpuScene& gpuScene, std::uint32_t instanceMask,
                      const ShadowRecords& shadowRecords)
{
    const auto viewCount = static_cast<std::uint32_t>(m_viewLights.size());
    const std::uint32_t rows = (viewCount + kAtlasTiles - 1) / kAtlasTiles;
    VkRenderingAttachmentInfo depth{};
    depth.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    depth.imageView = m_atlas.view();
    depth.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    depth.clearValue.depthStencil = {0.0F, 0};  // Reversed depth: 0 is nothing.
    VkRenderingInfo rendering{};
    rendering.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    rendering.renderArea = {{0, 0}, {m_atlas.width(), rows * m_resolution}};
    rendering.layerCount = 1;
    rendering.pDepthAttachment = &depth;
    vkCmdBeginRendering(commandBuffer, &rendering);

    // What casts shadows, and the bounding spheres to cull it per view.
    struct Caster
    {
        std::uint32_t instance;
        std::uint32_t vertexCount;
        bool sphere;
        Vec3 center;
        float radius;
    };
    std::vector<Caster> casters;
    for (std::uint32_t i = 0; i < scene.instances.size(); ++i) {
        const RenderInstance& instance = scene.instances[i];
        const RenderGeometry& geometry = scene.geometries[instance.geometryIndex];
        const bool sphere = geometry.kind == GeometryKind::Sphere;
        const auto vertexCount =
            sphere ? 36U
                   : static_cast<std::uint32_t>(geometry.mesh ? 3 * geometry.mesh->triangleCount()
                                                              : 0);
        if ((instance.mask & instanceMask) == 0 || vertexCount == 0 ||
            !castsShadow(scene.materials[instance.materialIndex]) ||
            !instance.worldBounds.isValid()) {
            continue;
        }
        const Aabb& bounds = instance.worldBounds;
        casters.push_back({i, vertexCount, sphere, bounds.center(),
                           0.5F * glm::length(bounds.max - bounds.min)});
    }

    vulkan::DescriptorWrites writes;
    writes.storageBuffer(2, gpuScene.transformBuffer())
        .storageBuffer(4, gpuScene.instanceBuffer())
        .uniformBuffer(13, records);
    for (const vulkan::GraphicsPipeline* pipeline : {&m_triangles, &m_spheres}) {
        const bool spheres = pipeline == &m_spheres;
        bool bound = false;
        for (std::uint32_t v = 0; v < viewCount; ++v) {
            const ShadowViewRecord& view = shadowRecords.views[v];
            bool tileSet = false;
            for (const Caster& caster : casters) {
                if (caster.sphere != spheres || caster.instance == m_viewLights[v] ||
                    !viewSees(view, caster.center, caster.radius)) {
                    continue;
                }
                if (!bound) {
                    pipeline->bind(commandBuffer);
                    pipeline->pushDescriptors(commandBuffer, writes.writes());
                    bound = true;
                }
                if (!tileSet) {
                    const VkViewport viewport{view.tile.x, view.tile.y, view.tile.z, view.tile.z,
                                              0.0F, 1.0F};
                    const VkRect2D scissor{
                        {static_cast<std::int32_t>(view.tile.x),
                         static_cast<std::int32_t>(view.tile.y)},
                        {m_resolution, m_resolution}};
                    vkCmdSetViewport(commandBuffer, 0, 1, &viewport);
                    vkCmdSetScissor(commandBuffer, 0, 1, &scissor);
                    tileSet = true;
                }
                const DrawConstants constants{caster.instance, v};
                pipeline->pushConstants(commandBuffer, &constants, sizeof(constants));
                vkCmdDraw(commandBuffer, caster.vertexCount, 1, 0, 0);
            }
        }
    }
    vkCmdEndRendering(commandBuffer);
    vulkan::memoryBarrier(commandBuffer,
                          VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                              VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                          VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                          VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
}

}  // namespace starcore::gpu
