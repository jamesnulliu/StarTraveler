#pragma once
// The temporal upscalers of GpuRenderer (private, GpuRenderOptions::upscaling): a
// pipeline renders at a lower resolution, each frame at another sample position in its
// pixels (the jitter), and an upscaler reconstructs the output's pixels from this frame
// and the earlier ones, reprojected with the motion vectors:
//   - TAAU (taau.comp): StarCore's own, any GPU;
//   - FSR (upscaler_fsr.cpp): AMD FidelityFX Super Resolution 3.1, its prebuilt DLL;
//   - DLSS (upscaler_dlss.cpp): NVIDIA DLSS Super Resolution, through NGX;
//   - DLSS-RR (upscaler_dlss.cpp): NVIDIA DLSS Ray Reconstruction, which also denoises
//     (denoises()): it takes a pipeline's noisy samples and the guides of its surfaces.

#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include <vulkan/vulkan.h>

#include "starcore/math/types.hpp"
#include "starcore/vulkan/resources.hpp"

namespace starcore::vulkan
{
class Context;
}

namespace starcore
{
enum class DlssPreset : std::uint8_t;
}

namespace starcore::gpu
{

/** @brief One frame to upscale; every image is in VK_IMAGE_LAYOUT_GENERAL. */
struct UpscaleFrame
{
    // The pipeline's size:
    const vulkan::Image* color = nullptr;   // RGBA32F, linear radiance.
    const vulkan::Image* depth = nullptr;   // D32, reversed with an infinite far plane.
    const vulkan::Image* motion = nullptr;  // RGBA32F, xy: previous minus current position (px).
    // The output's size: RGBA32F, linear radiance.
    const vulkan::Image* output = nullptr;
    Vec2 jitter{0.5F};          // Sample position in the pixels, [0, 1) (+y down).
    bool reset = false;         // Nothing earlier to reproject (new content, a cut).
    float frameMs = 1000.0F / 60.0F;  // Since the previous frame.
    float verticalFov = 0.0F;   // Radians.
    float nearPlane = 0.0F;
    float sharpness = 0.0F;     // 0 to 1, where supported.
    std::uint32_t frameSlot = 0;

    // Of an upscaler that denoises (`color`: one frame's noisy samples), at the
    // pipeline's size, of the surfaces the pixels see:
    const vulkan::Image* diffuseAlbedo = nullptr;    // RGBA16F, rgb.
    const vulkan::Image* specularAlbedo = nullptr;   // RGBA16F, rgb: the lobe's mean.
    const vulkan::Image* normalRoughness = nullptr;  // RGBA16F: world normal, roughness.
    // RGBA32F, xy: as `motion`, of what the surfaces reflect.
    const vulkan::Image* reflectionMotion = nullptr;
    // RGBA16F, premultiplied: added to the result, not denoised (null: none).
    const vulkan::Image* overlay = nullptr;
};

class TemporalUpscaler
{
public:
    virtual ~TemporalUpscaler() = default;
    TemporalUpscaler(const TemporalUpscaler&) = delete;
    auto operator=(const TemporalUpscaler&) -> TemporalUpscaler& = delete;

    /**
     * @brief (Re)create what depends on the sizes (the device is idle); the next frame resets.
     * @return Whether it can upscale between them (DLSS refuses tiny ones).
     */
    virtual auto resize(UVec2 renderSize, UVec2 outputSize) -> bool = 0;

    /** @brief Change the SR model hint; true requires an idle resize before recording. */
    virtual auto setDlssPreset(DlssPreset /*preset*/) -> bool
    {
        return false;
    }

    /** @brief Fixed K-model exposure (0: internal auto); true requires an idle resize. */
    virtual auto setDlssExposure(float /*exposure*/) -> bool { return false; }

    /**
     * @brief Record the upscaling of a frame: after the pipeline's writes of
     *        the inputs (compute shaders, rasterized depth), ending with the
     *        output written for later compute shaders.
     */
    virtual void record(VkCommandBuffer commandBuffer, const UpscaleFrame& frame) = 0;

    /** @brief Whether it denoises: the pipeline gives it noisy samples and the guides. */
    [[nodiscard]] virtual auto denoises() const -> bool
    {
        return false;
    }

protected:
    TemporalUpscaler() = default;
};

[[nodiscard]] auto createTaau(vulkan::Context& context) -> std::unique_ptr<TemporalUpscaler>;
/** @return Why not, without the SDK's DLL or a device it supports. */
[[nodiscard]] auto createFsr(vulkan::Context& context)
    -> std::expected<std::unique_ptr<TemporalUpscaler>, std::string>;
/** @return Why not, without an NVIDIA RTX GPU, its driver's NGX or the DLSS DLL. */
[[nodiscard]] auto createDlss(vulkan::Context& context)
    -> std::expected<std::unique_ptr<TemporalUpscaler>, std::string>;
/** @return Why not, without an NVIDIA RTX GPU, its driver's NGX or the DLSS-RR DLL. */
[[nodiscard]] auto createDlssRr(vulkan::Context& context)
    -> std::expected<std::unique_ptr<TemporalUpscaler>, std::string>;

/** @brief The barrier before an upscaler reads what the pipeline wrote. */
void inputBarrier(VkCommandBuffer commandBuffer);
/** @brief The barrier before compute shaders read what an upscaler wrote. */
void outputBarrier(VkCommandBuffer commandBuffer);

/**
 * @brief Where to look for a vendor's DLL: next to the module StarCore is in
 *        (the executable, the Python extension), then `buildDirectory` (where
 *        the build downloaded it).
 */
[[nodiscard]] auto libraryDirectories(const char* buildDirectory)
    -> std::vector<std::filesystem::path>;

}  // namespace starcore::gpu
