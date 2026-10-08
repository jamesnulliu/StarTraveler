#pragma once
// Layers: every entity is on one of 8 layers (LayerComponent, default 0).
// The scene keeps per-layer settings -- name, visibility, pickability -- and
// a symmetric collision matrix. Renderers and picking filter with layer
// masks (bit i = layer i), which map 1:1 onto the 8-bit instance masks of
// Vulkan ray tracing; physics uses the collision matrix.

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace starcore
{

inline constexpr std::size_t kLayerCount = 8;
inline constexpr std::uint32_t kAllLayers = 0xFFU;

[[nodiscard]] constexpr auto layerBit(std::uint32_t layer) -> std::uint32_t
{
    return 1U << (layer % kLayerCount);
}

struct LayerSettings
{
    std::string name;
    bool visible = true;   // Rendered (ray tracing and 2D overlay).
    bool pickable = true;  // Selectable in the viewport (false = locked).

    auto operator==(const LayerSettings&) const -> bool = default;
};

struct SceneLayers
{
    std::array<LayerSettings, kLayerCount> layers;
    // collisionMasks[i] bit j: layer i collides with layer j (kept symmetric).
    std::array<std::uint8_t, kLayerCount> collisionMasks;

    /** @brief "Default", "Layer 1" ... "Layer 7"; all visible, pickable, colliding. */
    SceneLayers();

    [[nodiscard]] auto visibleMask() const -> std::uint32_t;
    [[nodiscard]] auto pickableMask() const -> std::uint32_t;
    /** @brief Layers that colliders on `layer` collide with. */
    [[nodiscard]] auto collisionMask(std::uint32_t layer) const -> std::uint32_t
    {
        return collisionMasks[layer % kLayerCount];
    }
    [[nodiscard]] auto collides(std::uint32_t a, std::uint32_t b) const -> bool
    {
        return (collisionMask(a) & layerBit(b)) != 0U;
    }
    /** @brief Set whether layers a and b collide (both directions). */
    void setCollides(std::uint32_t a, std::uint32_t b, bool collide);

    auto operator==(const SceneLayers&) const -> bool = default;
};

}  // namespace starcore
