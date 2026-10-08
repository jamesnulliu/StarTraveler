export module starcore.scene:serializer;

import std;

export namespace starcore
{

class AssetLibrary;
class Scene;

/** @brief Current version written to the "StarCoreScene" key. */
inline constexpr int kSceneFormatVersion = 1;

/**
 * @brief Serialize every entity and the environment to YAML.
 * @param baseDirectory Model files referenced by meshes are written relative
 *        to it (absolute when empty or on another drive).
 */
[[nodiscard]] auto serializeScene(const Scene& scene,
                                  const std::filesystem::path& baseDirectory = {})
    -> std::string;

/**
 * @brief Replace the contents of `scene` with the scene described by `yaml`.
 *        On failure the scene is left empty and the error is returned.
 * @param baseDirectory Relative model paths are resolved against it (the
 *        current directory when empty).
 * @param assets Cache used to load referenced models (a temporary one when null).
 */
[[nodiscard]] auto deserializeScene(std::string_view yaml, Scene& scene,
                                    const std::filesystem::path& baseDirectory = {},
                                    AssetLibrary* assets = nullptr)
    -> std::expected<void, std::string>;

/** @brief Write a scene file (model paths relative to the file's directory). */
[[nodiscard]] auto saveScene(const Scene& scene, const std::filesystem::path& path)
    -> std::expected<void, std::string>;

[[nodiscard]] auto loadScene(const std::filesystem::path& path, Scene& scene,
                             AssetLibrary* assets = nullptr)
    -> std::expected<void, std::string>;

}  // namespace starcore
