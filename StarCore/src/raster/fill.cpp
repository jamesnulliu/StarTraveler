#include <vector>

#include "starcore/raster/fill.hpp"

namespace starcore::raster
{

namespace
{

/**
 * @brief Span flood fill: fills whole horizontal runs and pushes one seed per
 *        run in the rows above/below. `shouldFill(color)` decides which pixels
 *        belong to the region; filled pixels must stop matching it.
 */
template <typename ShouldFill>
auto spanFill(ColorImage& image, IVec2 seed, std::uint32_t fillColor,
              ShouldFill&& shouldFill) -> std::size_t
{
    if (!image.contains(seed.x, seed.y)) {
        return 0;
    }
    const int width = static_cast<int>(image.width());
    const int height = static_cast<int>(image.height());
    auto pixel = [&](int x, int y) -> std::uint32_t& {
        return image.at(static_cast<std::uint32_t>(x),
                        static_cast<std::uint32_t>(y));
    };

    std::size_t filled = 0;
    std::vector<IVec2> stack{seed};
    while (!stack.empty()) {
        const IVec2 p = stack.back();
        stack.pop_back();
        if (!shouldFill(pixel(p.x, p.y))) {
            continue;
        }
        int left = p.x;
        while (left > 0 && shouldFill(pixel(left - 1, p.y))) {
            --left;
        }
        int right = p.x;
        while (right + 1 < width && shouldFill(pixel(right + 1, p.y))) {
            ++right;
        }
        for (int x = left; x <= right; ++x) {
            pixel(x, p.y) = fillColor;
        }
        filled += static_cast<std::size_t>(right - left + 1);

        for (const int y : {p.y - 1, p.y + 1}) {
            if (y < 0 || y >= height) {
                continue;
            }
            bool inRun = false;
            for (int x = left; x <= right; ++x) {
                const bool match = shouldFill(pixel(x, y));
                if (match && !inRun) {
                    stack.push_back({x, y});
                }
                inRun = match;
            }
        }
    }
    return filled;
}

}  // namespace

auto floodFill(ColorImage& image, IVec2 seed, std::uint32_t fillColor)
    -> std::size_t
{
    if (!image.contains(seed.x, seed.y)) {
        return 0;
    }
    const std::uint32_t target = image.at(static_cast<std::uint32_t>(seed.x),
                                          static_cast<std::uint32_t>(seed.y));
    if (target == fillColor) {
        return 0;
    }
    return spanFill(image, seed, fillColor,
                    [target](std::uint32_t color) { return color == target; });
}

auto boundaryFill(ColorImage& image, IVec2 seed, std::uint32_t fillColor,
                  std::uint32_t boundaryColor) -> std::size_t
{
    return spanFill(image, seed, fillColor,
                    [=](std::uint32_t color) {
                        return color != boundaryColor && color != fillColor;
                    });
}

}  // namespace starcore::raster
