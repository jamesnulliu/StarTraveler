#pragma once

#include <expected>
#include <filesystem>
#include <string>
#include <string_view>

namespace starcore
{

class Scene;

/** @brief Current version written to the "StarCoreScene" key. */
inline constexpr int kSceneFormatVersion = 1;

/** @brief Serialize every entity and the environment to YAML. */
[[nodiscard]] auto serializeScene(const Scene& scene) -> std::string;

/**
 * @brief Replace the contents of `scene` with the scene described by `yaml`.
 *        On failure the scene is left empty and the error is returned.
 */
[[nodiscard]] auto deserializeScene(std::string_view yaml, Scene& scene)
    -> std::expected<void, std::string>;

[[nodiscard]] auto saveScene(const Scene& scene,
                             const std::filesystem::path& path)
    -> std::expected<void, std::string>;

[[nodiscard]] auto loadScene(const std::filesystem::path& path, Scene& scene)
    -> std::expected<void, std::string>;

}  // namespace starcore
