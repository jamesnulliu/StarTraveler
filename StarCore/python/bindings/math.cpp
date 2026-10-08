module;

#include <nanobind/nanobind.h>
#include <nanobind/ndarray.h>
#include "casters.hpp"

module starcore.python;

import std;
import starcore.math;

using namespace nb::literals;

namespace starcore::python
{

void bindMath(nb::module_& m)
{
    nb::class_<Ray>(m, "Ray", "Ray with an origin and a (not necessarily unit) direction.")
        .def(nb::init<>())
        .def(
            "__init__",
            [](Ray* self, Vec3 origin, Vec3 direction) { new (self) Ray{origin, direction}; },
            "origin"_a, "direction"_a)
        .def_rw("origin", &Ray::origin)
        .def_rw("direction", &Ray::direction)
        .def("at", &Ray::at, "t"_a, "Point origin + direction * t.");

    nb::enum_<ToneMapper>(m, "ToneMapper")
        .value("NONE", ToneMapper::None)
        .value("REINHARD", ToneMapper::Reinhard)
        .value("ACES", ToneMapper::Aces);

    m.def("pack_rgba8", &packRgba8, "color"_a,
          "Pack a [0, 1] RGBA color into a uint32 (R in the lowest byte).");
    m.def("unpack_rgba8", &unpackRgba8, "packed"_a);
    m.def("linear_to_srgb", nb::overload_cast<Vec3>(&linearToSrgb), "color"_a);
    m.def("srgb_to_linear", nb::overload_cast<Vec3>(&srgbToLinear), "color"_a);
    m.def("tone_map", &toneMap, "color"_a, "mapper"_a);

    m.def(
        "compose_transform",
        [](Vec3 translation, Quat rotation, Vec3 scale) {
            return toArray(composeTransform(translation, rotation, scale));
        },
        "translation"_a = Vec3(0.0F), "rotation"_a = Quat(1.0F, 0.0F, 0.0F, 0.0F),
        "scale"_a = Vec3(1.0F), "Translation * Rotation * Scale as a 4x4 array.");
    m.def("rotation_between", &rotationBetween, "from_"_a, "to"_a,
          "Shortest rotation (w, x, y, z) taking direction `from_` onto `to`.");
}

}  // namespace starcore::python
