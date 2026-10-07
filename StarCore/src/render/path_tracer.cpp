#include <algorithm>

#include <glm/geometric.hpp>

#include "starcore/render/path_tracer.hpp"

namespace starcore
{

namespace
{

constexpr float kRayOffset = 1e-4F;
constexpr std::uint32_t kTileSize = 16;
constexpr std::uint32_t kRussianRouletteStart = 3;

}  // namespace

auto tracePath(const RenderScene& scene, Ray ray,
               const PathTracerSettings& settings, Rng& rng,
               std::uint32_t* primaryEntityId) -> Vec3
{
    Vec3 radiance{0.0F};
    Vec3 throughput{1.0F};
    if (primaryEntityId != nullptr) {
        *primaryEntityId = kNoEntityId;
    }

    for (std::uint32_t bounce = 0; bounce < settings.maxBounces; ++bounce) {
        const auto hit = traceClosestHit(scene, ray, 0.0F, kInfinity);
        if (!hit.has_value()) {
            radiance += throughput * scene.skyColor;
            break;
        }

        const RenderInstance& instance = scene.instances[hit->instanceIndex];
        const RenderMaterial& material = scene.materials[instance.materialIndex];
        if (bounce == 0 && primaryEntityId != nullptr) {
            *primaryEntityId = instance.entityId;
        }

        radiance += throughput * Vec3(material.emission);

        Vec3 direction{};
        if (rng.nextFloat() < material.metallic) {
            const Vec3 reflected =
                glm::reflect(glm::normalize(ray.direction), hit->normal);
            direction = glm::normalize(reflected +
                                       material.roughness * rng.inUnitSphere());
            if (glm::dot(direction, hit->normal) <= 0.0F) {
                break;  // Scattered below the surface: absorbed.
            }
        } else {
            direction = rng.cosineHemisphere(hit->normal);
        }
        throughput *= Vec3(material.albedo);

        if (bounce >= kRussianRouletteStart) {
            const float survival = std::min(
                1.0F, std::max({throughput.r, throughput.g, throughput.b}));
            if (rng.nextFloat() >= survival) {
                break;
            }
            throughput /= survival;
        }

        // Offset along the geometric normal (which faces the incoming side,
        // where the scattered ray leaves) to avoid self-intersection.
        ray = Ray{hit->position + hit->geometricNormal * kRayOffset, direction};
    }
    return radiance;
}

CpuPathTracer::CpuPathTracer(unsigned threadCount) : m_executor(threadCount)
{
}

void CpuPathTracer::resize(std::uint32_t width, std::uint32_t height)
{
    if (width == m_image.width() && height == m_image.height()) {
        return;
    }
    m_accumulation.resize(width, height, Vec4(0.0F));
    m_image.resize(width, height, 0U);
    m_entityIds.resize(width, height, kNoEntityId);
    resetAccumulation();
}

auto CpuPathTracer::needsReset(const RenderScene& scene, const Camera& camera,
                               const PathTracerSettings& settings) const
    -> bool
{
    return m_frameIndex == 0 || !settings.accumulate ||
           scene.sceneVersion != m_lastSceneVersion ||
           camera.view() != m_lastView ||
           camera.projection() != m_lastProjection ||
           settings != m_lastSettings;
}

void CpuPathTracer::render(const RenderScene& scene, const Camera& camera,
                           const PathTracerSettings& settings)
{
    const std::uint32_t width = m_image.width();
    const std::uint32_t height = m_image.height();
    if (width == 0 || height == 0) {
        return;
    }
    if (needsReset(scene, camera, settings)) {
        m_frameIndex = 0;
        m_accumulation.fill(Vec4(0.0F));
    }
    m_lastSceneVersion = scene.sceneVersion;
    m_lastView = camera.view();
    m_lastProjection = camera.projection();
    m_lastSettings = settings;

    const std::uint32_t frame = m_frameIndex++;
    const bool writeIds = frame == 0;
    const std::uint32_t samples = std::max(1U, settings.samplesPerPixel);
    const std::uint32_t tilesX = (width + kTileSize - 1) / kTileSize;
    const std::uint32_t tilesY = (height + kTileSize - 1) / kTileSize;

    m_executor.parallelFor(tilesX * tilesY, [&](std::size_t tile) {
        const std::uint32_t x0 = static_cast<std::uint32_t>(tile % tilesX) * kTileSize;
        const std::uint32_t y0 = static_cast<std::uint32_t>(tile / tilesX) * kTileSize;
        const std::uint32_t x1 = std::min(width, x0 + kTileSize);
        const std::uint32_t y1 = std::min(height, y0 + kTileSize);

        for (std::uint32_t y = y0; y < y1; ++y) {
            for (std::uint32_t x = x0; x < x1; ++x) {
                Rng rng = Rng::forPixel(x, y, frame, settings.seed);
                Vec3 color{0.0F};
                std::uint32_t entityId = kNoEntityId;
                for (std::uint32_t s = 0; s < samples; ++s) {
                    // Jittered sub-pixel position: anti-aliasing for free.
                    const Vec2 pixel{static_cast<float>(x) + rng.nextFloat(),
                                     static_cast<float>(y) + rng.nextFloat()};
                    color += tracePath(scene, camera.generateRay(pixel),
                                       settings, rng,
                                       s == 0 && writeIds ? &entityId : nullptr);
                }
                color /= static_cast<float>(samples);

                Vec4& accumulated = m_accumulation.at(x, y);
                accumulated += Vec4(color, 1.0F);
                Vec3 display = Vec3(accumulated) / accumulated.a;
                display = toneMap(display * settings.exposure, settings.toneMapper);
                if (settings.srgbOutput) {
                    display = linearToSrgb(display);
                }
                m_image.at(x, y) = packRgba8(Vec4(display, 1.0F));
                if (writeIds) {
                    m_entityIds.at(x, y) = entityId;
                }
            }
        }
    });
}

}  // namespace starcore
