export module starcore.raster:types;

import std;
import starcore.common;
import starcore.math;

export namespace starcore
{

enum class LinePattern : std::uint8_t
{
    Solid,
    Dash,
    Dot,
};

enum class LineAlgorithm : std::uint8_t
{
    Bresenham,
    MidPoint,
    Dda,
};

enum class EllipseAlgorithm : std::uint8_t
{
    // Sample the local-space ellipse and project it: correct under any
    // rotation/perspective.
    Parametric,
    // Screen-space midpoint algorithm on the projected (axis-aligned) radii.
    MidPoint,
};

enum class CurveType : std::uint8_t
{
    BSpline,
    Bezier,
};

template <>
struct EnumNames<LinePattern>
{
    static constexpr std::array kNames = {"Solid", "Dash", "Dot"};
};

template <>
struct EnumNames<LineAlgorithm>
{
    static constexpr std::array kNames = {"Bresenham", "MidPoint", "DDA"};
};

template <>
struct EnumNames<EllipseAlgorithm>
{
    static constexpr std::array kNames = {"Parametric", "MidPoint"};
};

template <>
struct EnumNames<CurveType>
{
    static constexpr std::array kNames = {"BSpline", "Bezier"};
};

}  // namespace starcore

export namespace starcore::raster
{

/** @brief Callable receiving integer pixel positions. */
template <typename F>
concept PixelPlotter = std::invocable<F&, IVec2>;

/** @brief Round half up; correct for negatives (unlike legacy `Int64(x + .5)`). */
[[nodiscard]] inline auto roundToInt(float value) -> int
{
    return static_cast<int>(std::floor(value + 0.5F));
}

/** @brief Pixel containing a continuous screen position. */
[[nodiscard]] inline auto toPixel(Vec2 position) -> IVec2
{
    return {static_cast<int>(std::floor(position.x)),
            static_cast<int>(std::floor(position.y))};
}

}  // namespace starcore::raster
