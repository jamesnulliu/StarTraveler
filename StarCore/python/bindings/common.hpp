#pragma once
// Shared helpers of the `starcore._core` bindings: type casters for glm
// vectors/quaternions (Python tuples) and numpy conversions.

#include <cstdint>
#include <cstring>
#include <type_traits>
#include <vector>

#include <nanobind/nanobind.h>
#include <nanobind/ndarray.h>

#include <starcore/common/image.hpp>
#include <starcore/math/types.hpp>

namespace nb = nanobind;
using namespace nb::literals;

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

// numpy helpers ---------------------------------------------------------------------------------

namespace starcore::python
{

/** @brief numpy array owning a heap copy of `values` with the given shape. */
template <typename T, typename Shape>
auto makeArray(std::vector<T> values, std::initializer_list<std::size_t> shape)
    -> nb::ndarray<nb::numpy, T, Shape>
{
    auto* owned = new std::vector<T>(std::move(values));
    nb::capsule owner(owned, [](void* pointer) noexcept {
        delete static_cast<std::vector<T>*>(pointer);
    });
    return nb::ndarray<nb::numpy, T, Shape>(owned->data(), shape, owner);
}

using Matrix4Array = nb::ndarray<nb::numpy, float, nb::shape<4, 4>>;
using RgbaArray = nb::ndarray<nb::numpy, std::uint8_t, nb::shape<-1, -1, 4>>;
using IdArray = nb::ndarray<nb::numpy, std::uint32_t, nb::shape<-1, -1>>;

using Matrix4Input = nb::ndarray<const float, nb::shape<4, 4>, nb::c_contig, nb::device::cpu>;
using RgbaInput = nb::ndarray<const std::uint8_t, nb::shape<-1, -1, 4>, nb::c_contig,
                              nb::device::cpu>;
using IdInput = nb::ndarray<const std::uint32_t, nb::shape<-1, -1>, nb::c_contig,
                            nb::device::cpu>;

/** @brief Row-major 4x4 array (glm matrices are column-major). */
inline auto toArray(const Mat4& matrix) -> Matrix4Array
{
    std::vector<float> values(16);
    for (int row = 0; row < 4; ++row) {
        for (int column = 0; column < 4; ++column) {
            values[static_cast<std::size_t>(row * 4 + column)] = matrix[column][row];
        }
    }
    return makeArray<float, nb::shape<4, 4>>(std::move(values), {4, 4});
}

/** @brief Row-major 4x4 array -> glm matrix. */
inline auto fromArray(const Matrix4Input& array) -> Mat4
{
    Mat4 matrix{1.0F};
    for (std::size_t row = 0; row < 4; ++row) {
        for (std::size_t column = 0; column < 4; ++column) {
            matrix[static_cast<glm::length_t>(column)][static_cast<glm::length_t>(row)] =
                array(row, column);
        }
    }
    return matrix;
}

/** @brief RGBA8 image -> uint8 array of shape (height, width, 4). */
inline auto toArray(const ColorImage& image) -> RgbaArray
{
    std::vector<std::uint8_t> bytes(image.pixels().size_bytes());
    if (!bytes.empty()) {
        std::memcpy(bytes.data(), image.data(), bytes.size());
    }
    return makeArray<std::uint8_t, nb::shape<-1, -1, 4>>(
        std::move(bytes), {image.height(), image.width(), 4});
}

/** @brief Id image -> uint32 array of shape (height, width). */
inline auto toIdArray(const IdImage& image) -> IdArray
{
    std::vector<std::uint32_t> ids(image.pixels().begin(), image.pixels().end());
    return makeArray<std::uint32_t, nb::shape<-1, -1>>(std::move(ids),
                                                       {image.height(), image.width()});
}

inline auto fromArray(const RgbaInput& array) -> ColorImage
{
    ColorImage image(static_cast<std::uint32_t>(array.shape(1)),
                     static_cast<std::uint32_t>(array.shape(0)));
    if (!image.empty()) {
        std::memcpy(image.data(), array.data(), image.pixels().size_bytes());
    }
    return image;
}

inline auto fromArray(const IdInput& array) -> IdImage
{
    IdImage image(static_cast<std::uint32_t>(array.shape(1)),
                  static_cast<std::uint32_t>(array.shape(0)));
    if (!image.empty()) {
        std::memcpy(image.data(), array.data(), image.pixels().size_bytes());
    }
    return image;
}

void bindMath(nb::module_& m);
void bindAsset(nb::module_& m);  // Meshes (before the components that use them).
void bindScene(nb::module_& m);
void bindModel(nb::module_& m);  // Models (after the components they contain).
void bindRender(nb::module_& m);
void bindRaster(nb::module_& m);

}  // namespace starcore::python
