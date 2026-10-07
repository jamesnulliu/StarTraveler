#pragma once

#include <starcore/common/image.hpp>

namespace startraveler
{

/**
 * @brief Cockpit dashboard at the bottom of the view: the two gray
 *        trapezoids the legacy renderer always drew with ScanLineFill
 *        (converted from its bottom-left to our top-left pixel origin).
 */
void drawCockpitHud(starcore::ColorImage& frame);

}  // namespace startraveler
