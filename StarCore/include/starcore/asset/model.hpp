#pragma once
// Models: a tree of nodes that reference meshes and materials, loaded from and
// saved to glTF 2.0. A model is an asset; instantiateModel() turns it into
// entities (Transform + Mesh + Material) in a scene.

#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "starcore/asset/mesh.hpp"
#include "starcore/scene/components.hpp"

namespace starcore
{

class Entity;
class Scene;

struct ModelMaterial
{
    std::string name;
    MaterialComponent material;
};

/** @brief One mesh with one material (a glTF primitive). */
struct ModelMesh
{
    std::string name;
    MeshHandle mesh;
    std::optional<std::uint32_t> material;  // Index into Model::materials.
};

/**
 * @brief A node of the model tree with at most one mesh (glTF meshes with
 *        several primitives are loaded as one child node per primitive).
 */
struct ModelNode
{
    std::string name;
    TransformComponent transform;          // Relative to the parent node.
    std::optional<std::uint32_t> mesh;     // Index into Model::meshes.
    std::vector<std::uint32_t> children;   // Indices into Model::nodes.
};

struct Model
{
    std::vector<ModelMaterial> materials;
    std::vector<ModelMesh> meshes;
    std::vector<ModelNode> nodes;
    std::vector<std::uint32_t> roots;  // Top-level nodes.

    auto addMaterial(std::string name, const MaterialComponent& material) -> std::uint32_t;
    auto addMesh(std::string name, MeshHandle mesh,
                 std::optional<std::uint32_t> material = std::nullopt) -> std::uint32_t;
    /** @brief Add a node under `parent` (or as a root). */
    auto addNode(std::string name, const TransformComponent& transform = {},
                 std::optional<std::uint32_t> mesh = std::nullopt,
                 std::optional<std::uint32_t> parent = std::nullopt) -> std::uint32_t;

    /** @brief Indices in range, meshes present, and the nodes form a forest. */
    [[nodiscard]] auto validate() const -> std::expected<void, std::string>;
};

/**
 * @brief Load a .gltf/.glb file: triangle primitives (positions, normals,
 *        indices), metallic-roughness materials (with emissive strength,
 *        transmission, IOR and volume) and the node tree of the default scene.
 *        Textures, skins and animations are ignored.
 */
[[nodiscard]] auto loadModel(const std::filesystem::path& path)
    -> std::expected<Model, std::string>;

/** @brief Write a model as binary glTF (.glb). */
[[nodiscard]] auto saveModel(const Model& model, const std::filesystem::path& path)
    -> std::expected<void, std::string>;

/**
 * @brief Create entities for the model's node tree under `parent` (or as
 *        root entities). With a `sourcePath`, mesh components remember it so
 *        scene files reference the model instead of copying its geometry.
 * @return The entities created for the model's root nodes.
 */
auto instantiateModel(Scene& scene, const Model& model, std::string_view sourcePath,
                      Entity parent) -> std::expected<std::vector<Entity>, std::string>;

/** @brief Absolute, lexically normalized path with '/' separators (asset keys). */
[[nodiscard]] auto normalizeAssetPath(const std::filesystem::path& path) -> std::string;

/** @brief Cache of loaded models, keyed by normalized path. */
class AssetLibrary
{
public:
    [[nodiscard]] auto loadModel(const std::filesystem::path& path)
        -> std::expected<std::shared_ptr<const Model>, std::string>;

    /** @brief A mesh of a model file (loading the model if needed). */
    [[nodiscard]] auto loadMesh(const MeshSource& source) -> std::expected<MeshHandle, std::string>;

    void clear()
    {
        m_models.clear();
    }

    [[nodiscard]] auto modelCount() const noexcept -> std::size_t
    {
        return m_models.size();
    }

private:
    std::unordered_map<std::string, std::shared_ptr<const Model>> m_models;
};

}  // namespace starcore
