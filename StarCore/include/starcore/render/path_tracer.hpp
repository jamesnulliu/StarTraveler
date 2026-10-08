#pragma once

#include <array>
#include <cstdint>

#include "starcore/common/enum_names.hpp"
#include "starcore/common/image.hpp"
#include "starcore/common/parallel.hpp"
#include "starcore/math/color.hpp"
#include "starcore/math/random.hpp"
#include "starcore/render/camera.hpp"
#include "starcore/render/render_scene.hpp"

namespace starcore
{

enum class RenderMode : std::uint8_t
{
    // Unidirectional path tracing (GGX / Lambert / dielectric BSDF, next event
    // estimation of sphere lights with multiple importance sampling).
    PathTraced,
    // Primary rays with direct light and ambient only: noise-free and fast,
    // for moving around.
    Preview,
};

template <>
struct EnumNames<RenderMode>
{
    static constexpr std::array kNames = {"Path traced", "Preview"};
};

struct PathTracerSettings
{
    RenderMode mode = RenderMode::PathTraced;
    // Path segments traced per sample (the legacy tracer used 3).
    std::uint32_t maxBounces = 5;
    std::uint32_t samplesPerPixel = 1;  // Per frame.
    bool accumulate = true;
    // Sample sphere lights directly (off: BSDF sampling only, for testing).
    bool nextEventEstimation = true;
    std::uint32_t seed = 0;
    float exposure = 1.0F;
    ToneMapper toneMapper = ToneMapper::None;
    // Encode the output as sRGB (the legacy tracer wrote linear values).
    bool srgbOutput = true;
    // Only instances whose mask intersects this are visible (layers).
    std::uint32_t instanceMask = kAllInstances;
    // Largest radiance one sample may add after the first bounce (0 = off).
    // Clamping removes caustic fireflies at the cost of some bias.
    float fireflyClamp = 0.0F;

    auto operator==(const PathTracerSettings&) const -> bool = default;
};

/**
 * @brief Radiance along one camera path.
 *
 * Shader-style free function so it maps directly onto the planned compute
 * shader. Fixes of the legacy PerPixelColor:
 *   - estimator: radiance += throughput * emission; throughput *= BSDF weight
 *     (legacy accumulated `light += emission; illum += light * contrib`,
 *     counting emission multiple times with the wrong weight),
 *   - importance sampling (legacy normalized a random point in a cube, which
 *     is not even uniform), plus direct light sampling.
 *
 * Thin-walled transmissive surfaces (window panes) are see-through for the
 * primary entity id, so looking out of a cockpit picks what is outside.
 *
 * @param primaryEntityId If not null, receives the entity id of the first
 *        opaque hit (kNoEntityId on a miss).
 */
[[nodiscard]] auto tracePath(const RenderScene& scene, Ray ray,
                             const PathTracerSettings& settings, Rng& rng,
                             std::uint32_t* primaryEntityId = nullptr) -> Vec3;

/** @brief RenderMode::Preview shading of one camera ray (deterministic). */
[[nodiscard]] auto shadePreview(const RenderScene& scene, Ray ray,
                                const PathTracerSettings& settings,
                                std::uint32_t* primaryEntityId = nullptr) -> Vec3;

/**
 * @brief Progressive multi-threaded CPU path tracer.
 *
 * Each render() call adds one frame of samples; accumulation restarts
 * automatically when the scene version, camera, settings or size change.
 */
class CpuPathTracer
{
public:
    /** @param threadCount 0 = all hardware threads. */
    explicit CpuPathTracer(unsigned threadCount = 0);

    void resize(std::uint32_t width, std::uint32_t height);
    void resetAccumulation() noexcept
    {
        m_frameIndex = 0;
    }

    void render(const RenderScene& scene, const Camera& camera,
                const PathTracerSettings& settings);

    /** @brief Tone-mapped RGBA8 result of all accumulated frames. */
    [[nodiscard]] auto image() const -> const ColorImage&
    {
        return m_image;
    }

    /** @brief Entity id of the primary hit per pixel (kNoEntityId = sky). */
    [[nodiscard]] auto entityIds() const -> const IdImage&
    {
        return m_entityIds;
    }

    [[nodiscard]] auto accumulatedFrames() const noexcept -> std::uint32_t
    {
        return m_frameIndex;
    }

    [[nodiscard]] auto threadCount() const noexcept -> unsigned
    {
        return m_executor.threadCount();
    }

private:
    [[nodiscard]] auto needsReset(const RenderScene& scene,
                                  const Camera& camera,
                                  const PathTracerSettings& settings) const
        -> bool;

    ParallelExecutor m_executor;
    Image2D<Vec4> m_accumulation;  // rgb = sum of samples, a = frame count.
    ColorImage m_image;
    IdImage m_entityIds;
    std::uint32_t m_frameIndex = 0;

    // State of the previous frame, for automatic accumulation resets.
    std::uint64_t m_lastSceneVersion = ~0ULL;
    Mat4 m_lastView{0.0F};
    Mat4 m_lastProjection{0.0F};
    PathTracerSettings m_lastSettings{};
};

}  // namespace starcore
