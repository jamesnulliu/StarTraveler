// starcore.scene -- the ECS scene (on EnTT) and what is stored in it:
//
//   :entt        EnTT's entity, registry and operators (exported for the scene's users)
//   :uuid        stable entity ids
//   :layers      8 layers: visibility, pickability, the collision matrix
//   :components  the plain-data components (transform, material, mesh, ...)
//   :scene       Scene: the registry, hierarchy, layers and environment
//   :entity      Entity: a handle with typed component access
//   :model       models (glTF): load, save, instantiate as entities; AssetLibrary
//   :serializer  YAML scene files
//
// Code that uses more of EnTT than :entt exports (views, ...) includes all of
// <entt/entity/registry.hpp>.

export module starcore.scene;

export import :components;
export import :entity;
export import :entt;
export import :layers;
export import :model;
export import :scene;
export import :serializer;
export import :uuid;
