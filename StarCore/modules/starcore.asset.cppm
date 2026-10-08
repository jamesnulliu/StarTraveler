// starcore.asset -- shared, immutable assets:
//
//   :mesh  triangle meshes with their BVH (MeshData, MeshHandle), MeshBuilder
//          and procedural primitives
//
// Models (glTF files: a tree of meshes and materials) become entities, so they
// belong to starcore.scene (:model).

export module starcore.asset;

export import :mesh;
