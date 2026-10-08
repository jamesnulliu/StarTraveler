// Shared helpers of the `starcore._core` bindings: numpy conversions and the
// functions that bind each part of the engine. Its implementation units are
// the binding sources; the type casters are in casters.hpp.

module;

#include <nanobind/nanobind.h>
#include <nanobind/ndarray.h>

export module starcore.python;

import std;
import starcore.common;
import starcore.math;

export namespace nb = nanobind;

// numpy helpers ---------------------------------------------------------------------------------

export namespace starcore::python
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
            matrix[static_cast<Mat4::length_type>(column)][static_cast<Mat4::length_type>(row)] =
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
void bindPhysics(nb::module_& m);

}  // namespace starcore::python
