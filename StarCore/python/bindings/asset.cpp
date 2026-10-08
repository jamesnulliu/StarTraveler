module;

#include <nanobind/nanobind.h>
#include <nanobind/ndarray.h>
#include <nanobind/stl/filesystem.h>
#include <nanobind/stl/optional.h>
#include <nanobind/stl/pair.h>
#include <nanobind/stl/shared_ptr.h>
#include <nanobind/stl/string.h>
#include <nanobind/stl/vector.h>

#include "casters.hpp"

module starcore.python;

import std;
import starcore.asset;
import starcore.math;
import starcore.scene;

using namespace nb::literals;

namespace starcore::python
{

namespace
{

using Vec3Array = nb::ndarray<nb::numpy, float, nb::shape<-1, 3>>;
using Vec3Input = nb::ndarray<const float, nb::shape<-1, 3>, nb::c_contig, nb::device::cpu>;
using IndexInput = nb::ndarray<const std::uint32_t, nb::c_contig, nb::device::cpu>;

auto vec3Array(std::span<const Vec3> values) -> Vec3Array
{
    std::vector<float> flat(values.size() * 3);
    if (!values.empty()) {
        std::memcpy(flat.data(), values.data(), values.size_bytes());
    }
    return makeArray<float, nb::shape<-1, 3>>(std::move(flat), {values.size(), 3});
}

auto toIndexArray(std::span<const std::uint32_t> indices)
    -> nb::ndarray<nb::numpy, std::uint32_t, nb::shape<-1, 3>>
{
    return makeArray<std::uint32_t, nb::shape<-1, 3>>(
        std::vector<std::uint32_t>(indices.begin(), indices.end()), {indices.size() / 3, 3});
}

auto vec3Vector(const Vec3Input& array) -> std::vector<Vec3>
{
    std::vector<Vec3> values(array.shape(0));
    if (!values.empty()) {
        std::memcpy(values.data(), array.data(), values.size() * sizeof(Vec3));
    }
    return values;
}

template <typename T>
auto unwrap(std::expected<T, std::string> result) -> T
{
    if (!result) {
        throw std::runtime_error(result.error());
    }
    return *std::move(result);
}

void bindMeshes(nb::module_& m)
{
    nb::class_<MeshData>(m, "MeshData",
                         "Immutable triangle mesh (with its BVH), shared by every entity that "
                         "shows it. Create one with make_mesh() or MeshBuilder.build().")
        .def_prop_ro(
            "positions", [](const MeshData& mesh) { return vec3Array(mesh.positions); },
            nb::rv_policy::move, "float32 array (vertex_count, 3).")
        .def_prop_ro(
            "normals", [](const MeshData& mesh) { return vec3Array(mesh.normals); },
            nb::rv_policy::move, "float32 array (vertex_count, 3), or (0, 3) if flat shaded.")
        .def_prop_ro(
            "indices", [](const MeshData& mesh) { return toIndexArray(mesh.indices); },
            nb::rv_policy::move, "uint32 array (triangle_count, 3).")
        .def_prop_ro("vertex_count", [](const MeshData& mesh) { return mesh.positions.size(); })
        .def_prop_ro("triangle_count", &MeshData::triangleCount)
        .def_prop_ro("bounds",
                     [](const MeshData& mesh) { return std::pair(mesh.bounds.min, mesh.bounds.max); },
                     "(min, max) corners of the bounding box.")
        .def("__repr__", [](const MeshData& mesh) {
            return std::format("<MeshData {} vertices, {} triangles{}>", mesh.positions.size(),
                               mesh.triangleCount(), mesh.normals.empty() ? ", flat" : "");
        });

    m.def(
        "_make_mesh",
        [](const Vec3Input& positions, const IndexInput& indices,
           std::optional<Vec3Input> normals) {
            std::vector<std::uint32_t> flatIndices(indices.data(), indices.data() + indices.size());
            return unwrap(makeMesh(vec3Vector(positions), std::move(flatIndices),
                                   normals ? vec3Vector(*normals) : std::vector<Vec3>{}));
        },
        "positions"_a, "indices"_a, "normals"_a = nb::none());

    nb::class_<MeshBuilder>(m, "MeshBuilder",
                            "Incremental mesh construction; combine transformed parts and "
                            "build() an immutable MeshData.")
        .def(nb::init<>())
        .def("add_vertex", &MeshBuilder::addVertex, "position"_a, "normal"_a,
             "Add a vertex and return its index.")
        .def("add_triangle", &MeshBuilder::addTriangle, "a"_a, "b"_a, "c"_a)
        .def("add_quad", &MeshBuilder::addQuad, "a"_a, "b"_a, "c"_a, "d"_a,
             "Two triangles (a, b, c) and (a, c, d).")
        .def(
            "append",
            [](MeshBuilder& self, const MeshBuilder& other, std::optional<Matrix4Input> transform) {
                self.append(other, transform ? fromArray(*transform) : Mat4(1.0F));
                return &self;
            },
            "other"_a, "transform"_a = nb::none(), nb::rv_policy::reference,
            "Append another builder's geometry, transformed by a 4x4 matrix.")
        .def(
            "transform",
            [](MeshBuilder& self, const Matrix4Input& matrix) {
                self.transform(fromArray(matrix));
                return &self;
            },
            "matrix"_a, nb::rv_policy::reference)
        .def(
            "flip",
            [](MeshBuilder& self) {
                self.flip();
                return &self;
            },
            nb::rv_policy::reference, "Reverse the winding and the normals.")
        .def_prop_ro("vertex_count",
                     [](const MeshBuilder& b) { return b.positions().size(); })
        .def_prop_ro("triangle_count",
                     [](const MeshBuilder& b) { return b.indices().size() / 3; })
        .def("build", &MeshBuilder::build);

    nb::module_ p = m.def_submodule(
        "primitives", "Procedural meshes (counter-clockwise faces, outward normals) as "
                      "MeshBuilders; call build() for a MeshData.");
    p.def("box", &primitives::box, "half_extents"_a, "Axis-aligned box centered at the origin.");
    p.def("plane", &primitives::plane, "size"_a, "Rectangle in the XZ plane facing +Y.");
    p.def("quad", &primitives::quad, "size"_a, "Rectangle in the XY plane facing +Z.");
    p.def("sphere", &primitives::sphere, "radius"_a, "segments"_a = 32U, "rings"_a = 16U,
          "polar"_a = Vec2(0.0F, kPi), "azimuth"_a = Vec2(0.0F, kTwoPi),
          "Part of a sphere: polar angle from +Y, azimuth from +X towards -Z (radians).");
    p.def("cylinder", &primitives::cylinder, "radius"_a, "height"_a, "segments"_a = 32U,
          "caps"_a = true, "azimuth"_a = Vec2(0.0F, kTwoPi),
          "Cylinder along Y; a partial azimuth range gives a curved panel.");
    p.def("torus", &primitives::torus, "major_radius"_a, "minor_radius"_a, "segments"_a = 32U,
          "sides"_a = 16U);
    p.def(
        "extrude",
        [](const std::vector<Vec2>& profile, float depth) {
            return primitives::extrude(profile, depth);
        },
        "profile"_a, "depth"_a,
        "Prism from a simple polygon in the XY plane, from z = -depth/2 to +depth/2.");
}

void bindModels(nb::module_& m)
{
    nb::class_<ModelMaterial>(m, "ModelMaterial")
        .def_rw("name", &ModelMaterial::name)
        .def_rw("material", &ModelMaterial::material);
    nb::class_<ModelMesh>(m, "ModelMesh")
        .def_rw("name", &ModelMesh::name)
        .def_rw("mesh", &ModelMesh::mesh)
        .def_rw("material", &ModelMesh::material, "Index into Model.materials, or None.");
    nb::class_<ModelNode>(m, "ModelNode")
        .def_rw("name", &ModelNode::name)
        .def_rw("transform", &ModelNode::transform)
        .def_rw("mesh", &ModelNode::mesh, "Index into Model.meshes, or None.")
        .def_rw("children", &ModelNode::children);

    nb::class_<Model>(m, "Model",
                      "A tree of nodes referencing meshes and materials (glTF 2.0). The list "
                      "attributes return copies: build models with the add_* methods.")
        .def(nb::init<>())
        .def_rw("materials", &Model::materials)
        .def_rw("meshes", &Model::meshes)
        .def_rw("nodes", &Model::nodes)
        .def_rw("roots", &Model::roots)
        .def("add_material", &Model::addMaterial, "name"_a, "material"_a)
        .def("add_mesh", &Model::addMesh, "name"_a, "mesh"_a, "material"_a = nb::none())
        .def("add_node", &Model::addNode, "name"_a, "transform"_a = TransformComponent{},
             "mesh"_a = nb::none(), "parent"_a = nb::none(),
             "Add a node under `parent` (or as a root) and return its index.")
        .def("validate",
             [](const Model& model) {
                 if (auto valid = model.validate(); !valid) {
                     throw std::runtime_error(valid.error());
                 }
             })
        .def(
            "save",
            [](const Model& model, const std::filesystem::path& path) {
                if (auto saved = saveModel(model, path); !saved) {
                    throw std::runtime_error(saved.error());
                }
            },
            "path"_a, "Write a binary glTF (.glb) file.")
        .def("__repr__", [](const Model& model) {
            return std::format("<Model {} nodes, {} meshes, {} materials>", model.nodes.size(),
                               model.meshes.size(), model.materials.size());
        });

    m.def(
        "load_model", [](const std::filesystem::path& path) { return unwrap(loadModel(path)); },
        "path"_a, "Load a .gltf/.glb file (meshes, materials, node tree).");

    nb::class_<AssetLibrary>(m, "AssetLibrary", "Cache of loaded models, keyed by path.")
        .def(nb::init<>())
        .def(
            "load_model",
            [](AssetLibrary& library, const std::filesystem::path& path) {
                return Model(*unwrap(library.loadModel(path)));
            },
            "path"_a, "The cached model (loaded on first use); a copy sharing its meshes.")
        .def(
            "load_mesh",
            [](AssetLibrary& library, const std::filesystem::path& path, std::uint32_t index) {
                return unwrap(library.loadMesh(MeshSource{normalizeAssetPath(path), index}));
            },
            "path"_a, "index"_a)
        .def("clear", &AssetLibrary::clear)
        .def_prop_ro("model_count", &AssetLibrary::modelCount);
}

}  // namespace

void bindAsset(nb::module_& m)
{
    bindMeshes(m);
}

void bindModel(nb::module_& m)
{
    bindModels(m);
}

}  // namespace starcore::python
