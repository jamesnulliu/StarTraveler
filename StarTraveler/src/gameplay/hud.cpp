#include <array>

#include <starcore/math/color.hpp>
#include <starcore/raster/polygon.hpp>
#include <starcore/raster/stroke.hpp>

#include "gameplay/hud.hpp"

namespace startraveler
{

using namespace starcore;

void drawCockpitHud(ColorImage& frame)
{
    const auto w = static_cast<float>(frame.width());
    const auto h = static_cast<float>(frame.height());
    if (w <= 0.0F || h <= 0.0F) {
        return;
    }
    auto fill = [&](std::array<Vec2, 4> polygon, float gray) {
        raster::StrokeWriter writer(frame, nullptr, packRgba8({gray, gray, gray, 1.0F}),
                                    kNoEntityId);
        raster::scanlineFill(polygon, static_cast<int>(w), static_cast<int>(h), writer);
    };
    // Legacy (y up): (0,0) (w/4,h/5) (3w/4,h/5) (w,0); y' = h - y.
    fill({Vec2{0.0F, h}, Vec2{w / 4.0F, h - h / 5.0F},
          Vec2{3.0F * w / 4.0F, h - h / 5.0F}, Vec2{w, h}},
         121.0F / 255.0F);
    fill({Vec2{80.0F, h}, Vec2{w / 4.0F, h - (h / 5.0F - 50.0F)},
          Vec2{3.0F * w / 4.0F, h - (h / 5.0F - 50.0F)}, Vec2{w - 80.0F, h}},
         200.0F / 255.0F);
}

}  // namespace startraveler
