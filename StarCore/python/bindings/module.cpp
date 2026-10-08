// `starcore._core`: nanobind bindings of StarCore::Core (headless engine).
//
// Conventions of the Python API:
//   - glm vectors are tuples (any sequence of numbers is accepted as input),
//     quaternions are (w, x, y, z) tuples, matrices are 4x4 numpy arrays;
//   - images are numpy arrays: RGBA8 as uint8 (height, width, 4), ids as
//     uint32 (height, width);
//   - components are plain value types: Entity.get() returns a copy and
//     Entity.set() writes it back (which also bumps Scene.version).

module;

#include <nanobind/nanobind.h>
#include <nanobind/ndarray.h>
#include <nanobind/stl/string.h>

#include "starcore/common/log.hpp"

#include "casters.hpp"

module starcore.python;

import std;
import starcore.common;

using namespace nb::literals;

NB_MODULE(_core, m)
{
    m.doc() = "StarCore engine (headless core): scene/ECS, CPU path tracer, 2D raster";
    m.attr("__version__") = std::string(starcore::kVersionString);

    m.def(
        "set_log_level",
        [](const std::string& level) {
            const auto parsed = spdlog::level::from_str(level);
            starcore::log::core().set_level(parsed);
            starcore::log::client().set_level(parsed);
        },
        "level"_a,
        "Set the level of the StarCore loggers: trace, debug, info, warn, error, "
        "critical or off.");

    starcore::python::bindMath(m);
    starcore::python::bindAsset(m);
    starcore::python::bindScene(m);
    starcore::python::bindModel(m);
    starcore::python::bindRender(m);
    starcore::python::bindRaster(m);
    starcore::python::bindPhysics(m);
}
