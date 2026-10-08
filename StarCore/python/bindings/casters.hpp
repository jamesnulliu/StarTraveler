#pragma once
// nanobind type casters of the `starcore._core` bindings: glm vectors and
// quaternions as Python tuples.
//
// A header, not part of the module starcore.python: casters specialize
// nanobind's templates, which every binding unit instantiates from nanobind's
// headers, and MSVC (19.51) crashes compiling nanobind's call dispatch when
// the casters come from a module.

#include <type_traits>

#include <glm/gtc/quaternion.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>
#include <nanobind/nanobind.h>

// Type casters ----------------------------------------------------------------------------------

namespace nanobind::detail
{

template <glm::length_t L, typename T>
consteval auto glmVectorName()
{
    if constexpr (std::is_integral_v<T>) {
        if constexpr (L == 2) {
            return const_name("tuple[int, int]");
        } else if constexpr (L == 3) {
            return const_name("tuple[int, int, int]");
        } else {
            return const_name("tuple[int, int, int, int]");
        }
    } else {
        if constexpr (L == 2) {
            return const_name("tuple[float, float]");
        } else if constexpr (L == 3) {
            return const_name("tuple[float, float, float]");
        } else {
            return const_name("tuple[float, float, float, float]");
        }
    }
}

/** @brief Reads one number of a Python sequence; false (error cleared) on failure. */
template <typename T>
auto readNumber(PyObject* sequence, Py_ssize_t index, T& out) noexcept -> bool
{
    PyObject* item = PySequence_GetItem(sequence, index);
    if (item == nullptr) {
        PyErr_Clear();
        return false;
    }
    if constexpr (std::is_integral_v<T>) {
        const long long value = PyLong_AsLongLong(item);
        out = static_cast<T>(value);
        if constexpr (std::is_unsigned_v<T>) {
            if (value < 0 && !PyErr_Occurred()) {
                PyErr_SetString(PyExc_OverflowError, "negative value");
            }
        }
    } else {
        out = static_cast<T>(PyFloat_AsDouble(item));
    }
    Py_DECREF(item);
    if (PyErr_Occurred() != nullptr) {
        PyErr_Clear();
        return false;
    }
    return true;
}

template <typename T>
auto makeNumber(T value) noexcept -> PyObject*
{
    if constexpr (std::is_integral_v<T>) {
        return PyLong_FromLongLong(static_cast<long long>(value));
    } else {
        return PyFloat_FromDouble(static_cast<double>(value));
    }
}

/** @brief Sequences of numbers (tuple, list, 1-D array) <-> glm vectors (returned as tuples). */
template <glm::length_t L, typename T>
struct type_caster<glm::vec<L, T, glm::defaultp>>
{
    using VectorType = glm::vec<L, T, glm::defaultp>;
    NB_TYPE_CASTER(VectorType, (glmVectorName<L, T>()))

    auto from_python(handle src, uint32_t /*flags*/, cleanup_list* /*cleanup*/) noexcept
        -> bool
    {
        PyObject* object = src.ptr();
        if (PyUnicode_Check(object) || PyBytes_Check(object) ||
            PySequence_Check(object) == 0) {
            return false;
        }
        if (PySequence_Size(object) != L) {
            PyErr_Clear();
            return false;
        }
        for (glm::length_t i = 0; i < L; ++i) {
            if (!readNumber(object, i, value[i])) {
                return false;
            }
        }
        return true;
    }

    static auto from_cpp(const VectorType& vector, rv_policy /*policy*/,
                         cleanup_list* /*cleanup*/) noexcept -> handle
    {
        PyObject* tuple = PyTuple_New(L);
        if (tuple == nullptr) {
            return {};
        }
        for (glm::length_t i = 0; i < L; ++i) {
            PyObject* item = makeNumber(vector[i]);
            if (item == nullptr) {
                Py_DECREF(tuple);
                return {};
            }
            PyTuple_SetItem(tuple, i, item);  // Steals the reference.
        }
        return tuple;
    }
};

/** @brief Quaternions as (w, x, y, z) tuples (same order as the scene files). */
template <>
struct type_caster<glm::quat>
{
    NB_TYPE_CASTER(glm::quat, const_name("tuple[float, float, float, float]"))

    auto from_python(handle src, uint32_t flags, cleanup_list* cleanup) noexcept -> bool
    {
        type_caster<glm::vec4> wxyz;
        if (!wxyz.from_python(src, flags, cleanup)) {
            return false;
        }
        value = glm::quat(wxyz.value.x, wxyz.value.y, wxyz.value.z, wxyz.value.w);
        return true;
    }

    static auto from_cpp(const glm::quat& q, rv_policy policy, cleanup_list* cleanup) noexcept
        -> handle
    {
        return type_caster<glm::vec4>::from_cpp(glm::vec4(q.w, q.x, q.y, q.z), policy,
                                                cleanup);
    }
};

}  // namespace nanobind::detail
