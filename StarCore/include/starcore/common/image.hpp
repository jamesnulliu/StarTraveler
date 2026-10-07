#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <mdspan>
#include <span>
#include <string>
#include <vector>

#include "starcore/common/assert.hpp"
#include "starcore/math/types.hpp"

namespace starcore
{

/**
 * @brief Row-major 2D pixel buffer. Row 0 is the TOP row and pixel (x, y)
 *        covers [x, x+1) x [y, y+1) in screen space (y grows downwards),
 *        matching ImGui and Vulkan images -- no UV flipping when displayed.
 */
template <typename T>
class Image2D
{
public:
    using value_type = T;

    Image2D() = default;
    Image2D(std::uint32_t width, std::uint32_t height, T fillValue = T{})
    {
        resize(width, height, fillValue);
    }

    void resize(std::uint32_t width, std::uint32_t height, T fillValue = T{})
    {
        m_width = width;
        m_height = height;
        m_pixels.assign(static_cast<std::size_t>(width) * height, fillValue);
    }

    void fill(T value)
    {
        std::ranges::fill(m_pixels, value);
    }

    [[nodiscard]] auto width() const noexcept -> std::uint32_t
    {
        return m_width;
    }

    [[nodiscard]] auto height() const noexcept -> std::uint32_t
    {
        return m_height;
    }

    [[nodiscard]] auto size() const noexcept -> UVec2
    {
        return {m_width, m_height};
    }

    [[nodiscard]] auto empty() const noexcept -> bool
    {
        return m_pixels.empty();
    }

    [[nodiscard]] auto contains(int x, int y) const noexcept -> bool
    {
        return x >= 0 && y >= 0 && static_cast<std::uint32_t>(x) < m_width &&
               static_cast<std::uint32_t>(y) < m_height;
    }

    template <typename Self>
    [[nodiscard]] auto at(this Self& self, std::uint32_t x, std::uint32_t y)
        -> auto&
    {
        SC_ASSERT(x < self.m_width && y < self.m_height,
                  "pixel ({}, {}) outside {}x{} image", x, y, self.m_width,
                  self.m_height);
        return self.m_pixels[static_cast<std::size_t>(y) * self.m_width + x];
    }

    template <typename Self>
    [[nodiscard]] auto pixels(this Self& self)
    {
        return std::span{self.m_pixels};
    }

    template <typename Self>
    [[nodiscard]] auto data(this Self& self)
    {
        return self.m_pixels.data();
    }

    /** @brief 2D view indexed as view[y, x]. */
    template <typename Self>
    [[nodiscard]] auto view(this Self& self)
    {
        return std::mdspan(self.m_pixels.data(),
                           static_cast<std::size_t>(self.m_height),
                           static_cast<std::size_t>(self.m_width));
    }

private:
    std::uint32_t m_width = 0;
    std::uint32_t m_height = 0;
    std::vector<T> m_pixels;
};

/** @brief Packed RGBA8 colors (see packRgba8). */
using ColorImage = Image2D<std::uint32_t>;

/** @brief Entity id per pixel (`entt::to_integral`), for mouse picking. */
using IdImage = Image2D<std::uint32_t>;

/** @brief Value of IdImage pixels not covered by any entity. */
inline constexpr std::uint32_t kNoEntityId = 0xFFFFFFFFU;

/** @brief Write an RGBA8 image as PNG. */
auto writePng(const ColorImage& image, const std::filesystem::path& path)
    -> std::expected<void, std::string>;

}  // namespace starcore
