module;

#include <fastgltf/core.hpp>
#include <fastgltf/glm_element_traits.hpp>
#include <fastgltf/tools.hpp>
#include <fastgltf/types.hpp>

module starcore.scene;

import std;
import starcore.asset;
import starcore.math;

namespace starcore
{

// Model ------------------------------------------------------------------------------------------

auto Model::addMaterial(std::string name, const MaterialComponent& material) -> std::uint32_t
{
    materials.push_back({std::move(name), material});
    return static_cast<std::uint32_t>(materials.size() - 1);
}

auto Model::addMesh(std::string name, MeshHandle mesh, std::optional<std::uint32_t> material)
    -> std::uint32_t
{
    meshes.push_back({std::move(name), std::move(mesh), material});
    return static_cast<std::uint32_t>(meshes.size() - 1);
}

auto Model::addNode(std::string name, const TransformComponent& transform,
                    std::optional<std::uint32_t> mesh, std::optional<std::uint32_t> parent)
    -> std::uint32_t
{
    const auto index = static_cast<std::uint32_t>(nodes.size());
    nodes.push_back({std::move(name), transform, mesh, {}});
    if (parent && *parent < index) {
        nodes[*parent].children.push_back(index);
    } else {
        roots.push_back(index);
    }
    return index;
}

auto Model::validate() const -> std::expected<void, std::string>
{
    for (std::size_t i = 0; i < meshes.size(); ++i) {
        if (!meshes[i].mesh) {
            return std::unexpected(std::format("mesh {} has no data", i));
        }
        if (meshes[i].material && *meshes[i].material >= materials.size()) {
            return std::unexpected(std::format("mesh {} uses unknown material {}", i,
                                               *meshes[i].material));
        }
    }
    // Every node must be reached exactly once from the roots (a forest).
    std::vector<int> visits(nodes.size(), 0);
    std::vector<std::uint32_t> pending(roots.begin(), roots.end());
    while (!pending.empty()) {
        const std::uint32_t node = pending.back();
        pending.pop_back();
        if (node >= nodes.size()) {
            return std::unexpected(std::format("unknown node {}", node));
        }
        if (++visits[node] > 1) {
            return std::unexpected(std::format("node {} has several parents (or a cycle)", node));
        }
        if (const auto mesh = nodes[node].mesh; mesh && *mesh >= meshes.size()) {
            return std::unexpected(std::format("node {} uses unknown mesh {}", node, *mesh));
        }
        pending.insert(pending.end(), nodes[node].children.begin(), nodes[node].children.end());
    }
    return {};
}

// Loading (fastgltf) -------------------------------------------------------------------------------

namespace
{

auto toMaterial(const fastgltf::Material& source) -> MaterialComponent
{
    MaterialComponent material;
    const auto& base = source.pbrData.baseColorFactor;
    material.albedo = {base[0], base[1], base[2]};
    material.metallic = source.pbrData.metallicFactor;
    material.roughness = source.pbrData.roughnessFactor;
    material.emissionColor = {source.emissiveFactor[0], source.emissiveFactor[1],
                              source.emissiveFactor[2]};
    material.emissionPower =
        material.emissionColor == Vec3(0.0F) ? 0.0F : source.emissiveStrength;
    material.ior = source.ior;
    if (source.transmission) {
        material.transmission = source.transmission->transmissionFactor;
        // Without KHR_materials_volume a transmissive surface is thin-walled.
        material.thinWalled = !source.volume || source.volume->thicknessFactor <= 0.0F;
    }
    return material;
}

auto toTransform(const fastgltf::Node& node) -> TransformComponent
{
    if (const auto* trs = std::get_if<fastgltf::TRS>(&node.transform)) {
        return TransformComponent{
            .translation = {trs->translation[0], trs->translation[1], trs->translation[2]},
            .rotation = Quat(trs->rotation[3], trs->rotation[0], trs->rotation[1],
                             trs->rotation[2]),
            .scale = {trs->scale[0], trs->scale[1], trs->scale[2]},
        };
    }
    const auto& source = std::get<fastgltf::math::fmat4x4>(node.transform);
    Mat4 matrix{1.0F};
    for (int column = 0; column < 4; ++column) {
        for (int row = 0; row < 4; ++row) {
            matrix[column][row] = source[static_cast<std::size_t>(column)][static_cast<std::size_t>(row)];
        }
    }
    const auto decomposed = decomposeTransform(matrix);
    return {decomposed.translation, decomposed.rotation, decomposed.scale};
}

auto readPrimitive(const fastgltf::Asset& asset, const fastgltf::Primitive& primitive)
    -> std::expected<MeshHandle, std::string>
{
    const auto* position = primitive.findAttribute("POSITION");
    if (position == primitive.attributes.end()) {
        return std::unexpected("primitive without POSITION");
    }
    const fastgltf::Accessor& positionAccessor = asset.accessors[position->accessorIndex];
    std::vector<Vec3> positions(positionAccessor.count);
    fastgltf::copyFromAccessor<Vec3>(asset, positionAccessor, positions.data());

    std::vector<Vec3> normals;
    if (const auto* normal = primitive.findAttribute("NORMAL");
        normal != primitive.attributes.end()) {
        const fastgltf::Accessor& accessor = asset.accessors[normal->accessorIndex];
        if (accessor.count == positions.size()) {
            normals.resize(accessor.count);
            fastgltf::copyFromAccessor<Vec3>(asset, accessor, normals.data());
        }
    }

    std::vector<std::uint32_t> indices;
    if (primitive.indicesAccessor) {
        const fastgltf::Accessor& accessor = asset.accessors[*primitive.indicesAccessor];
        indices.resize(accessor.count);
        fastgltf::copyFromAccessor<std::uint32_t>(asset, accessor, indices.data());
    } else {
        indices.resize(positions.size());
        std::ranges::generate(indices, [i = 0U]() mutable { return i++; });
    }
    return makeMesh(std::move(positions), std::move(indices), std::move(normals));
}

}  // namespace

auto loadModel(const std::filesystem::path& path) -> std::expected<Model, std::string>
{
    auto data = fastgltf::GltfDataBuffer::FromPath(path);
    if (data.error() != fastgltf::Error::None) {
        return std::unexpected(std::format("cannot read \"{}\": {}", path.string(),
                                           fastgltf::getErrorMessage(data.error())));
    }
    fastgltf::Parser parser(fastgltf::Extensions::KHR_materials_emissive_strength |
                            fastgltf::Extensions::KHR_materials_transmission |
                            fastgltf::Extensions::KHR_materials_ior |
                            fastgltf::Extensions::KHR_materials_volume |
                            fastgltf::Extensions::KHR_mesh_quantization |
                            fastgltf::Extensions::KHR_texture_transform);
    auto asset = parser.loadGltf(data.get(), path.parent_path(),
                                 fastgltf::Options::LoadExternalBuffers |
                                     fastgltf::Options::DecomposeNodeMatrices);
    if (asset.error() != fastgltf::Error::None) {
        return std::unexpected(std::format("invalid glTF \"{}\": {}", path.string(),
                                           fastgltf::getErrorMessage(asset.error())));
    }
    const fastgltf::Asset& gltf = asset.get();

    Model model;
    for (const fastgltf::Material& material : gltf.materials) {
        model.addMaterial(std::string(material.name), toMaterial(material));
    }

    // Every triangle primitive becomes a ModelMesh; remember them per glTF mesh.
    std::vector<std::vector<std::uint32_t>> primitivesOfMesh(gltf.meshes.size());
    for (std::size_t m = 0; m < gltf.meshes.size(); ++m) {
        const fastgltf::Mesh& mesh = gltf.meshes[m];
        for (std::size_t p = 0; p < mesh.primitives.size(); ++p) {
            const fastgltf::Primitive& primitive = mesh.primitives[p];
            if (primitive.type != fastgltf::PrimitiveType::Triangles) {
                continue;  // Points/lines are not rendered.
            }
            auto meshData = readPrimitive(gltf, primitive);
            if (!meshData) {
                return std::unexpected(std::format("mesh \"{}\" in \"{}\": {}",
                                                   std::string(mesh.name), path.string(),
                                                   meshData.error()));
            }
            std::string name(mesh.name);
            if (mesh.primitives.size() > 1) {
                name += std::format("/{}", p);
            }
            std::optional<std::uint32_t> material;
            if (primitive.materialIndex && *primitive.materialIndex < model.materials.size()) {
                material = static_cast<std::uint32_t>(*primitive.materialIndex);
            }
            primitivesOfMesh[m].push_back(
                model.addMesh(std::move(name), *std::move(meshData), material));
        }
    }

    for (const fastgltf::Node& node : gltf.nodes) {
        ModelNode out{.name = std::string(node.name), .transform = toTransform(node)};
        for (const std::size_t child : node.children) {
            out.children.push_back(static_cast<std::uint32_t>(child));
        }
        model.nodes.push_back(std::move(out));
    }
    // Meshes go to their node, or (several primitives) to one new child node each;
    // the new nodes come after the glTF ones so glTF node indices stay valid.
    for (std::size_t i = 0; i < gltf.nodes.size(); ++i) {
        const auto& meshIndex = gltf.nodes[i].meshIndex;
        if (!meshIndex || *meshIndex >= primitivesOfMesh.size()) {
            continue;
        }
        const auto& primitives = primitivesOfMesh[*meshIndex];
        if (primitives.size() == 1) {
            model.nodes[i].mesh = primitives.front();
            continue;
        }
        for (const std::uint32_t mesh : primitives) {
            const auto child = static_cast<std::uint32_t>(model.nodes.size());
            model.nodes.push_back(ModelNode{.name = model.meshes[mesh].name, .mesh = mesh});
            model.nodes[i].children.push_back(child);
        }
    }

    if (!gltf.scenes.empty()) {
        const std::size_t scene = gltf.defaultScene.value_or(0);
        for (const std::size_t node : gltf.scenes[std::min(scene, gltf.scenes.size() - 1)]
                                          .nodeIndices) {
            model.roots.push_back(static_cast<std::uint32_t>(node));
        }
    } else {
        // No scene: every glTF node without a parent is a root.
        std::vector<bool> isChild(gltf.nodes.size(), false);
        for (const ModelNode& node : model.nodes) {
            for (const std::uint32_t child : node.children) {
                if (child < isChild.size()) {
                    isChild[child] = true;
                }
            }
        }
        for (std::uint32_t i = 0; i < gltf.nodes.size(); ++i) {
            if (!isChild[i]) {
                model.roots.push_back(i);
            }
        }
    }

    if (auto valid = model.validate(); !valid) {
        return std::unexpected(std::format("invalid model \"{}\": {}", path.string(),
                                           valid.error()));
    }
    return model;
}

// Saving (binary glTF) -------------------------------------------------------------------------------

namespace
{

/** @brief Minimal streaming JSON writer (enough for glTF). */
class JsonWriter
{
public:
    void beginObject()
    {
        prefix();
        m_text += '{';
        m_first.push_back(true);
    }
    void endObject()
    {
        m_text += '}';
        m_first.pop_back();
    }
    void beginArray()
    {
        prefix();
        m_text += '[';
        m_first.push_back(true);
    }
    void endArray()
    {
        m_text += ']';
        m_first.pop_back();
    }
    void key(std::string_view name)
    {
        separator();
        writeString(name);
        m_text += ':';
        m_afterKey = true;
    }
    void string(std::string_view value)
    {
        prefix();
        writeString(value);
    }
    void number(double value)
    {
        prefix();
        m_text += std::isfinite(value) ? std::format("{}", value) : "0";
    }
    void number(std::size_t value)
    {
        prefix();
        m_text += std::format("{}", value);
    }
    void boolean(bool value)
    {
        prefix();
        m_text += value ? "true" : "false";
    }
    template <typename Vector>
    void numbers(const Vector& values, std::size_t count)
    {
        beginArray();
        for (std::size_t i = 0; i < count; ++i) {
            number(static_cast<double>(values[static_cast<typename Vector::length_type>(i)]));
        }
        endArray();
    }

    [[nodiscard]] auto text() const -> const std::string&
    {
        return m_text;
    }

private:
    void separator()
    {
        if (!m_first.empty()) {
            if (!m_first.back()) {
                m_text += ',';
            }
            m_first.back() = false;
        }
    }
    void prefix()
    {
        if (m_afterKey) {
            m_afterKey = false;
        } else {
            separator();
        }
    }
    void writeString(std::string_view value)
    {
        m_text += '"';
        for (const char c : value) {
            switch (c) {
            case '"':
                m_text += "\\\"";
                break;
            case '\\':
                m_text += "\\\\";
                break;
            case '\n':
                m_text += "\\n";
                break;
            case '\t':
                m_text += "\\t";
                break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    m_text += std::format("\\u{:04x}", static_cast<unsigned>(c));
                } else {
                    m_text += c;
                }
            }
        }
        m_text += '"';
    }

    std::string m_text;
    std::vector<bool> m_first;
    bool m_afterKey = false;
};

struct BufferView
{
    std::size_t offset = 0;
    std::size_t length = 0;
    std::uint32_t target = 0;  // 34962 = ARRAY_BUFFER, 34963 = ELEMENT_ARRAY_BUFFER.
};

struct Accessor
{
    std::uint32_t view = 0;
    std::uint32_t componentType = 0;  // 5126 = FLOAT, 5125 = UNSIGNED_INT.
    std::size_t count = 0;
    const char* type = "SCALAR";
    std::optional<Aabb> bounds;
};

struct MeshAccessors
{
    std::uint32_t position = 0;
    std::optional<std::uint32_t> normal;
    std::uint32_t indices = 0;
};

constexpr std::uint32_t kGlbMagic = 0x46546C67U;  // "glTF"
constexpr std::uint32_t kChunkJson = 0x4E4F534AU;  // "JSON"
constexpr std::uint32_t kChunkBin = 0x004E4942U;   // "BIN\0"

void writeMaterial(JsonWriter& json, const ModelMaterial& source, std::set<std::string>& used)
{
    const MaterialComponent& m = source.material;
    json.beginObject();
    if (!source.name.empty()) {
        json.key("name");
        json.string(source.name);
    }
    json.key("pbrMetallicRoughness");
    json.beginObject();
    json.key("baseColorFactor");
    json.numbers(Vec4(glm::clamp(m.albedo, 0.0F, 1.0F), 1.0F), 4);
    json.key("metallicFactor");
    json.number(std::clamp(m.metallic, 0.0F, 1.0F));
    json.key("roughnessFactor");
    json.number(std::clamp(m.roughness, 0.0F, 1.0F));
    json.endObject();
    json.key("doubleSided");
    json.boolean(true);

    // emissiveFactor must be in [0, 1]: move the excess into the strength.
    Vec3 emissive = glm::max(m.emissionColor, Vec3(0.0F));
    float strength = std::max(m.emissionPower, 0.0F);
    if (const float peak = std::max({emissive.r, emissive.g, emissive.b}); peak > 1.0F) {
        emissive /= peak;
        strength *= peak;
    }
    if (strength == 0.0F) {
        emissive = Vec3(0.0F);
    }
    if (emissive != Vec3(0.0F)) {
        json.key("emissiveFactor");
        json.numbers(emissive, 3);
    }

    const bool hasStrength = emissive != Vec3(0.0F) && strength != 1.0F;
    const bool hasTransmission = m.transmission > 0.0F;
    const bool hasIor = m.ior != 1.5F;
    if (hasStrength || hasTransmission || hasIor) {
        json.key("extensions");
        json.beginObject();
        if (hasStrength) {
            used.insert("KHR_materials_emissive_strength");
            json.key("KHR_materials_emissive_strength");
            json.beginObject();
            json.key("emissiveStrength");
            json.number(strength);
            json.endObject();
        }
        if (hasTransmission) {
            used.insert("KHR_materials_transmission");
            json.key("KHR_materials_transmission");
            json.beginObject();
            json.key("transmissionFactor");
            json.number(std::clamp(m.transmission, 0.0F, 1.0F));
            json.endObject();
            if (!m.thinWalled) {
                used.insert("KHR_materials_volume");
                json.key("KHR_materials_volume");
                json.beginObject();
                json.key("thicknessFactor");
                json.number(1.0);
                json.endObject();
            }
        }
        if (hasIor) {
            used.insert("KHR_materials_ior");
            json.key("KHR_materials_ior");
            json.beginObject();
            json.key("ior");
            json.number(m.ior);
            json.endObject();
        }
        json.endObject();
    }
    json.endObject();
}

template <typename T>
void appendBytes(std::vector<std::byte>& buffer, std::span<const T> values)
{
    const auto bytes = std::as_bytes(values);
    buffer.insert(buffer.end(), bytes.begin(), bytes.end());
}

void appendUint32(std::string& out, std::uint32_t value)
{
    static_assert(std::endian::native == std::endian::little, "GLB is little-endian");
    char bytes[4];
    std::memcpy(bytes, &value, 4);
    out.append(bytes, 4);
}

}  // namespace

auto saveModel(const Model& model, const std::filesystem::path& path)
    -> std::expected<void, std::string>
{
    if (auto valid = model.validate(); !valid) {
        return std::unexpected(std::format("invalid model: {}", valid.error()));
    }
    if (path.extension() != ".glb") {
        return std::unexpected(std::format("\"{}\": only .glb files can be written",
                                           path.string()));
    }

    // Binary buffer: positions, normals, indices of every mesh.
    std::vector<std::byte> binary;
    std::vector<BufferView> views;
    std::vector<Accessor> accessors;
    std::vector<MeshAccessors> meshAccessors;
    auto addView = [&](auto values, std::uint32_t target) {
        views.push_back({binary.size(), values.size_bytes(), target});
        appendBytes(binary, values);
        return static_cast<std::uint32_t>(views.size() - 1);
    };
    for (const ModelMesh& mesh : model.meshes) {
        const MeshData& data = *mesh.mesh;
        MeshAccessors entry;
        entry.position = static_cast<std::uint32_t>(accessors.size());
        accessors.push_back({addView(std::span<const Vec3>(data.positions), 34962), 5126,
                             data.positions.size(), "VEC3", data.bounds});
        if (!data.normals.empty()) {
            entry.normal = static_cast<std::uint32_t>(accessors.size());
            accessors.push_back({addView(std::span<const Vec3>(data.normals), 34962), 5126,
                                 data.normals.size(), "VEC3", std::nullopt});
        }
        entry.indices = static_cast<std::uint32_t>(accessors.size());
        accessors.push_back({addView(std::span<const std::uint32_t>(data.indices), 34963), 5125,
                             data.indices.size(), "SCALAR", std::nullopt});
        meshAccessors.push_back(entry);
    }

    JsonWriter json;
    std::set<std::string> extensionsUsed;
    json.beginObject();
    json.key("asset");
    json.beginObject();
    json.key("version");
    json.string("2.0");
    json.key("generator");
    json.string("StarCore");
    json.endObject();

    json.key("scene");
    json.number(std::size_t{0});
    json.key("scenes");
    json.beginArray();
    json.beginObject();
    json.key("nodes");
    json.beginArray();
    for (const std::uint32_t root : model.roots) {
        json.number(std::size_t{root});
    }
    json.endArray();
    json.endObject();
    json.endArray();

    if (!model.nodes.empty()) {
        json.key("nodes");
        json.beginArray();
        for (const ModelNode& node : model.nodes) {
            json.beginObject();
            if (!node.name.empty()) {
                json.key("name");
                json.string(node.name);
            }
            const TransformComponent& t = node.transform;
            json.key("translation");
            json.numbers(t.translation, 3);
            json.key("rotation");
            json.numbers(Vec4(t.rotation.x, t.rotation.y, t.rotation.z, t.rotation.w), 4);
            json.key("scale");
            json.numbers(t.scale, 3);
            if (node.mesh) {
                json.key("mesh");  // ModelMesh i = glTF mesh i (one primitive).
                json.number(std::size_t{*node.mesh});
            }
            if (!node.children.empty()) {
                json.key("children");
                json.beginArray();
                for (const std::uint32_t child : node.children) {
                    json.number(std::size_t{child});
                }
                json.endArray();
            }
            json.endObject();
        }
        json.endArray();
    }

    if (!model.meshes.empty()) {
        json.key("meshes");
        json.beginArray();
        for (std::uint32_t mesh = 0; mesh < model.meshes.size(); ++mesh) {
            json.beginObject();
            if (!model.meshes[mesh].name.empty()) {
                json.key("name");
                json.string(model.meshes[mesh].name);
            }
            json.key("primitives");
            json.beginArray();
            {
                json.beginObject();
                json.key("attributes");
                json.beginObject();
                json.key("POSITION");
                json.number(std::size_t{meshAccessors[mesh].position});
                if (meshAccessors[mesh].normal) {
                    json.key("NORMAL");
                    json.number(std::size_t{*meshAccessors[mesh].normal});
                }
                json.endObject();
                json.key("indices");
                json.number(std::size_t{meshAccessors[mesh].indices});
                if (model.meshes[mesh].material) {
                    json.key("material");
                    json.number(std::size_t{*model.meshes[mesh].material});
                }
                json.endObject();
            }
            json.endArray();
            json.endObject();
        }
        json.endArray();
    }

    if (!model.materials.empty()) {
        json.key("materials");
        json.beginArray();
        for (const ModelMaterial& material : model.materials) {
            writeMaterial(json, material, extensionsUsed);
        }
        json.endArray();
    }

    if (!accessors.empty()) {
        json.key("accessors");
        json.beginArray();
        for (const Accessor& accessor : accessors) {
            json.beginObject();
            json.key("bufferView");
            json.number(std::size_t{accessor.view});
            json.key("componentType");
            json.number(std::size_t{accessor.componentType});
            json.key("count");
            json.number(accessor.count);
            json.key("type");
            json.string(accessor.type);
            if (accessor.bounds && accessor.bounds->isValid()) {
                json.key("min");
                json.numbers(accessor.bounds->min, 3);
                json.key("max");
                json.numbers(accessor.bounds->max, 3);
            }
            json.endObject();
        }
        json.endArray();

        json.key("bufferViews");
        json.beginArray();
        for (const BufferView& view : views) {
            json.beginObject();
            json.key("buffer");
            json.number(std::size_t{0});
            json.key("byteOffset");
            json.number(view.offset);
            json.key("byteLength");
            json.number(view.length);
            json.key("target");
            json.number(std::size_t{view.target});
            json.endObject();
        }
        json.endArray();

        json.key("buffers");
        json.beginArray();
        json.beginObject();
        json.key("byteLength");
        json.number(binary.size());
        json.endObject();
        json.endArray();
    }

    if (!extensionsUsed.empty()) {
        json.key("extensionsUsed");
        json.beginArray();
        for (const std::string& extension : extensionsUsed) {
            json.string(extension);
        }
        json.endArray();
    }
    json.endObject();

    // GLB container: header, JSON chunk (space padded), BIN chunk (zero padded).
    std::string jsonChunk = json.text();
    jsonChunk.resize((jsonChunk.size() + 3) / 4 * 4, ' ');
    const std::size_t binaryPadded = (binary.size() + 3) / 4 * 4;
    const std::size_t total =
        12 + 8 + jsonChunk.size() + (binary.empty() ? 0 : 8 + binaryPadded);

    std::string file;
    file.reserve(total);
    appendUint32(file, kGlbMagic);
    appendUint32(file, 2);
    appendUint32(file, static_cast<std::uint32_t>(total));
    appendUint32(file, static_cast<std::uint32_t>(jsonChunk.size()));
    appendUint32(file, kChunkJson);
    file += jsonChunk;
    if (!binary.empty()) {
        appendUint32(file, static_cast<std::uint32_t>(binaryPadded));
        appendUint32(file, kChunkBin);
        file.append(reinterpret_cast<const char*>(binary.data()), binary.size());
        file.append(binaryPadded - binary.size(), '\0');
    }

    if (path.has_parent_path()) {
        std::error_code error;
        std::filesystem::create_directories(path.parent_path(), error);
    }
    std::ofstream stream(path, std::ios::binary);
    if (!stream || !stream.write(file.data(), static_cast<std::streamsize>(file.size()))) {
        return std::unexpected(std::format("cannot write \"{}\"", path.string()));
    }
    return {};
}

// Instancing ---------------------------------------------------------------------------------------

auto instantiateModel(Scene& scene, const Model& model, std::string_view sourcePath,
                      Entity parent) -> std::expected<std::vector<Entity>, std::string>
{
    if (auto valid = model.validate(); !valid) {
        return std::unexpected(valid.error());
    }
    const std::string source = sourcePath.empty() ? std::string{} : normalizeAssetPath(sourcePath);

    auto attachMesh = [&](Entity target, std::uint32_t meshIndex) {
        const ModelMesh& mesh = model.meshes[meshIndex];
        target.add<MeshComponent>(MeshComponent{
            .mesh = mesh.mesh,
            .source = source.empty() ? MeshSource{} : MeshSource{source, meshIndex},
        });
        target.add<MaterialComponent>(mesh.material ? model.materials[*mesh.material].material
                                                    : MaterialComponent{});
    };
    auto createChild = [&](std::string name, Entity parentEntity,
                           const TransformComponent& transform) {
        Entity entity = scene.createEntity(std::move(name));
        if (parentEntity) {
            scene.setParent(entity, parentEntity);
        }
        entity.patch<TransformComponent>([&](auto& t) { t = transform; });
        return entity;
    };

    std::vector<Entity> roots;
    // Depth-first over the (validated) forest.
    std::vector<std::pair<std::uint32_t, Entity>> pending;
    for (auto it = model.roots.rbegin(); it != model.roots.rend(); ++it) {
        pending.emplace_back(*it, parent);
    }
    while (!pending.empty()) {
        const auto [nodeIndex, parentEntity] = pending.back();
        pending.pop_back();
        const ModelNode& node = model.nodes[nodeIndex];
        Entity entity =
            createChild(node.name.empty() ? std::format("Node {}", nodeIndex) : node.name,
                        parentEntity, node.transform);
        if (parentEntity == parent) {
            roots.push_back(entity);
        }
        if (node.mesh) {
            attachMesh(entity, *node.mesh);
        }
        for (auto it = node.children.rbegin(); it != node.children.rend(); ++it) {
            pending.emplace_back(*it, entity);
        }
    }
    return roots;
}

// AssetLibrary -------------------------------------------------------------------------------------

auto normalizeAssetPath(const std::filesystem::path& path) -> std::string
{
    return std::filesystem::absolute(path).lexically_normal().generic_string();
}

auto AssetLibrary::loadModel(const std::filesystem::path& path)
    -> std::expected<std::shared_ptr<const Model>, std::string>
{
    std::string key = normalizeAssetPath(path);
    if (const auto it = m_models.find(key); it != m_models.end()) {
        return it->second;
    }
    auto model = starcore::loadModel(key);
    if (!model) {
        return std::unexpected(model.error());
    }
    auto shared = std::make_shared<const Model>(*std::move(model));
    m_models.emplace(std::move(key), shared);
    return shared;
}

auto AssetLibrary::loadMesh(const MeshSource& source) -> std::expected<MeshHandle, std::string>
{
    auto model = loadModel(source.path);
    if (!model) {
        return std::unexpected(model.error());
    }
    if (source.meshIndex >= (*model)->meshes.size()) {
        return std::unexpected(std::format("\"{}\" has no mesh {}", source.path,
                                           source.meshIndex));
    }
    return (*model)->meshes[source.meshIndex].mesh;
}

}  // namespace starcore
