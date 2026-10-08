module starcore.scene;

import std;

namespace starcore
{

SceneLayers::SceneLayers()
{
    for (std::size_t i = 0; i < kLayerCount; ++i) {
        layers[i].name = i == 0 ? "Default" : std::format("Layer {}", i);
    }
    collisionMasks.fill(static_cast<std::uint8_t>(kAllLayers));
}

auto SceneLayers::visibleMask() const -> std::uint32_t
{
    std::uint32_t mask = 0;
    for (std::size_t i = 0; i < kLayerCount; ++i) {
        mask |= layers[i].visible ? layerBit(static_cast<std::uint32_t>(i)) : 0U;
    }
    return mask;
}

auto SceneLayers::pickableMask() const -> std::uint32_t
{
    std::uint32_t mask = 0;
    for (std::size_t i = 0; i < kLayerCount; ++i) {
        mask |= layers[i].pickable ? layerBit(static_cast<std::uint32_t>(i)) : 0U;
    }
    return mask;
}

void SceneLayers::setCollides(std::uint32_t a, std::uint32_t b, bool collide)
{
    auto update = [&](std::uint32_t row, std::uint32_t column) {
        auto& mask = collisionMasks[row % kLayerCount];
        mask = static_cast<std::uint8_t>(collide ? (mask | layerBit(column))
                                                 : (mask & ~layerBit(column)));
    };
    update(a, b);
    update(b, a);
}

}  // namespace starcore
