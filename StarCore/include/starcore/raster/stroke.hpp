#pragma once

#include <cstdint>

#include "starcore/common/image.hpp"
#include "starcore/raster/types.hpp"

namespace starcore::raster
{

/**
 * @brief Pixel writer used by the overlay renderer: applies a dash pattern,
 *        stamps a thickness x thickness square per point (clipped to the
 *        image), and records the entity id for mouse picking.
 *
 * Satisfies PixelPlotter (operator()(IVec2)) and SpanFiller (fillSpan).
 * The legacy Artist::PutPoint aborted the whole stamp at the first
 * out-of-bounds pixel and wrapped negative coordinates to huge unsigned ones.
 */
class StrokeWriter
{
public:
    StrokeWriter(ColorImage& color, IdImage* ids, std::uint32_t packedColor,
                 std::uint32_t entityId, std::uint32_t thickness = 1,
                 LinePattern pattern = LinePattern::Solid);

    /** @brief Plot a pattern-filtered, thick point. */
    void operator()(IVec2 position);

    /** @brief Write one pixel (bounds-checked, no pattern/thickness). */
    void putPixel(int x, int y);

    /** @brief Fill pixels [xBegin, xEnd) of row y (bounds-checked). */
    void operator()(int y, int xBegin, int xEnd);

    /** @brief Restart the dash pattern (call at the start of each shape). */
    void resetPattern() noexcept
    {
        m_step = 0;
    }

    void setColor(std::uint32_t packedColor) noexcept
    {
        m_color = packedColor;
    }

private:
    [[nodiscard]] auto patternVisible() noexcept -> bool;

    ColorImage* m_image;
    IdImage* m_ids;
    std::uint32_t m_color;
    std::uint32_t m_entityId;
    int m_thickness;
    LinePattern m_pattern;
    std::uint32_t m_step = 0;
};

}  // namespace starcore::raster
