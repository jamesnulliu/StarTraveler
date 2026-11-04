#pragma once
// Automatic exposure (PathTracerSettings::autoExposure), as a camera meters: a histogram of
// the frame's log2 luminance; the exposure that brings the mean of its brighter part (the
// darkest half and the brightest few percent left out: black space, a sun in view) to middle
// gray, within the camera's range; the exposure in use moving towards it over time, faster
// towards darker than towards brighter, as eyes adapt. The CPU path tracer and the GPU
// renderer (shaders/exposure.glsl) meter alike.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <span>

namespace starcore
{

inline constexpr std::uint32_t kExposureBins = 64;
// Bin 0: darker than 2^kExposureMinLog2; bins 1 to 63 split the range up to 2^kExposureMaxLog2.
inline constexpr float kExposureMinLog2 = -14.0F;
inline constexpr float kExposureMaxLog2 = 6.0F;
// The part of the pixels metered (fractions of them, darkest first).
inline constexpr float kExposureLowPercentile = 0.5F;
inline constexpr float kExposureHighPercentile = 0.96F;
// Middle gray: what the metered mean is exposed to.
inline constexpr float kExposureKey = 0.18F;
// The camera's range: from a sunlit surface filling the view (some 1/0.05 of its radiance up
// to white) to empty space, whose brightest light, the Milky Way, it lifts to a dim gray.
inline constexpr float kMinExposure = 0.05F;
inline constexpr float kMaxExposure = 12.0F;
// Adaptation: the fraction of the way (in stops) left after t seconds is exp(-rate t).
inline constexpr float kDarkeningRate = 3.0F;    // Towards a lower exposure (brighter view).
inline constexpr float kBrighteningRate = 1.2F;  // Towards a higher one.

/** @brief The histogram bin of a luminance. */
[[nodiscard]] inline auto exposureBin(float luminance) -> std::uint32_t
{
    if (!(luminance > std::exp2(kExposureMinLog2))) {
        return 0;  // Also NaN.
    }
    const float t = (std::log2(luminance) - kExposureMinLog2) / (kExposureMaxLog2 - kExposureMinLog2);
    return static_cast<std::uint32_t>(
        std::clamp(t * static_cast<float>(kExposureBins - 1) + 1.0F, 1.0F,
                   static_cast<float>(kExposureBins - 1)));
}

/**
 * @brief The exposure a histogram meters (within kMinExposure to kMaxExposure; 1 for an
 *        empty one).
 */
[[nodiscard]] inline auto meteredExposure(std::span<const std::uint32_t, kExposureBins> histogram)
    -> float
{
    double total = 0.0;
    for (const std::uint32_t count : histogram) {
        total += count;
    }
    if (total <= 0.0) {
        return 1.0F;
    }
    const double low = kExposureLowPercentile * total;
    const double high = kExposureHighPercentile * total;
    double below = 0.0;
    double sum = 0.0;
    double weight = 0.0;
    for (std::uint32_t bin = 0; bin < kExposureBins; ++bin) {
        const double count = histogram[bin];
        const double overlap = std::max(0.0, std::min(below + count, high) - std::max(below, low));
        const double log2Luminance =
            bin == 0 ? kExposureMinLog2
                     : kExposureMinLog2 + (static_cast<double>(bin) - 0.5) /
                                              static_cast<double>(kExposureBins - 1) *
                                              (kExposureMaxLog2 - kExposureMinLog2);
        sum += overlap * log2Luminance;
        weight += overlap;
        below += count;
    }
    const float mean = weight > 0.0 ? static_cast<float>(sum / weight) : std::log2(kExposureKey);
    return std::clamp(kExposureKey / std::exp2(mean), kMinExposure, kMaxExposure);
}

/** @brief The exposure in use, `seconds` later, moving from `current` towards `target`. */
[[nodiscard]] inline auto adaptExposure(float current, float target, float seconds) -> float
{
    const float rate = target < current ? kDarkeningRate : kBrighteningRate;
    const float left = std::exp(-rate * std::max(seconds, 0.0F));
    return std::exp2(std::log2(target) + (std::log2(current) - std::log2(target)) * left);
}

}  // namespace starcore
