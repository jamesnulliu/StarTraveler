#pragma once

// The demo's seven rungs (TODO.md, feature/demo-clean-build): one key sets the
// Render panel's options, and the view names the one that is on.

#include <optional>
#include <string_view>

#include "game_layer.hpp"

namespace startraveler
{

inline constexpr int kDemoPresetCount = 7;

/** @brief The name drawn for preset `key` (1-7). Empty for any other key. */
[[nodiscard]] auto demoPresetName(int key) -> std::string_view;

/**
 * @brief Set `options` to preset `key` (1-7).
 * @param baseUpscaler Upscaler restored for every preset except 6 (DLSS-RR).
 *        Quality, effects, the denoiser's history and the probe spacing stay.
 * @return false when `key` is not a preset.
 */
auto applyDemoPreset(RenderOptions& options, int key, starcore::Upscaler baseUpscaler) -> bool;

/**
 * @brief The preset `options` are, or none when a panel edit has left the rungs
 *        (a debug view, the radiance cache, probes at full resolution, ...).
 *        Ray tracing off is preset 1; the path-tracing pipeline is preset 7.
 */
[[nodiscard]] auto matchDemoPreset(const RenderOptions& options) -> std::optional<int>;

}  // namespace startraveler
