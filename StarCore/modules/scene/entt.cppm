// EnTT, a header-only library, as far as the scene's users need it: its entity type and registry
// are the scene's vocabulary.
//
// EnTT's operators are exported too: the registry's templates (storages, iterators), instantiated
// in the units that use them, find them by argument-dependent lookup, which sees only exported or
// included declarations. Code that uses more of EnTT (views, ...) includes
// <entt/entity/registry.hpp> -- all of it: a partial include (e.g. entity.hpp alone) forward
// declares EnTT's templates without their definitions, which hides the imported ones from MSVC.

module;

#include <entt/entity/registry.hpp>

export module starcore.scene:entt;

import std;

export namespace entt
{
using entt::entity;
using entt::null;
using entt::registry;
using entt::to_integral;

using entt::operator==;
using entt::operator!=;
}  // namespace entt

export namespace entt::internal
{
using internal::operator==;
using internal::operator!=;
using internal::operator<;
using internal::operator<=;
using internal::operator>;
using internal::operator>=;
using internal::operator-;
}  // namespace entt::internal
