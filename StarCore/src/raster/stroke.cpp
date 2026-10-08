// The overlay's pixel writer (module starcore.raster).

module starcore.raster;

import std;
import starcore.common;
import starcore.math;

namespace starcore::raster
{

StrokeWriter::StrokeWriter(ColorImage& color, IdImage* ids,
                           std::uint32_t packedColor, std::uint32_t entityId,
                           std::uint32_t thickness, LinePattern pattern)
    : m_image(&color), m_ids(ids), m_color(packedColor), m_entityId(entityId),
      m_thickness(static_cast<int>(std::max(1U, thickness))),
      m_pattern(pattern)
{
}

auto StrokeWriter::patternVisible() noexcept -> bool
{
    const std::uint32_t step = m_step++;
    switch (m_pattern) {
    case LinePattern::Solid:
        return true;
    case LinePattern::Dash:  // 20 on, 10 off (legacy values)
        return step % 30U < 20U;
    case LinePattern::Dot:  // 5 on, 5 off
        return step % 10U < 5U;
    }
    return true;
}

void StrokeWriter::putPixel(int x, int y)
{
    if (!m_image->contains(x, y)) {
        return;
    }
    const auto ux = static_cast<std::uint32_t>(x);
    const auto uy = static_cast<std::uint32_t>(y);
    m_image->at(ux, uy) = m_color;
    if (m_ids != nullptr) {
        m_ids->at(ux, uy) = m_entityId;
    }
}

void StrokeWriter::operator()(IVec2 position)
{
    if (!patternVisible()) {
        return;
    }
    if (m_thickness == 1) {
        putPixel(position.x, position.y);
        return;
    }
    // Square stamp of side `thickness`, roughly centered on the point.
    const int x0 = position.x - (m_thickness - 1) / 2;
    const int y0 = position.y - (m_thickness - 1) / 2;
    const int xBegin = std::max(0, x0);
    const int yBegin = std::max(0, y0);
    const int xEnd =
        std::min(static_cast<int>(m_image->width()), x0 + m_thickness);
    const int yEnd =
        std::min(static_cast<int>(m_image->height()), y0 + m_thickness);
    for (int y = yBegin; y < yEnd; ++y) {
        for (int x = xBegin; x < xEnd; ++x) {
            putPixel(x, y);
        }
    }
}

void StrokeWriter::operator()(int y, int xBegin, int xEnd)
{
    if (y < 0 || y >= static_cast<int>(m_image->height())) {
        return;
    }
    xBegin = std::max(0, xBegin);
    xEnd = std::min(static_cast<int>(m_image->width()), xEnd);
    for (int x = xBegin; x < xEnd; ++x) {
        putPixel(x, y);
    }
}

}  // namespace starcore::raster
