#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

#include <glm/common.hpp>
#include <glm/exponential.hpp>
#include <glm/geometric.hpp>

#include "starcore/common/enum_names.hpp"
#include "starcore/math/types.hpp"

namespace starcore
{

/**
 * @brief Pack a [0,1] RGBA color into 8-bit channels, R in the lowest byte
 *        (= VK_FORMAT_R8G8B8A8_UNORM memory order on little-endian).
 *
 * Values are clamped and rounded. The legacy RGBA_to_Uint32 did neither (an
 * albedo of 2.0 overflowed into the neighbouring channel) and forced alpha=1.
 */
[[nodiscard]] constexpr auto packRgba8(Vec4 color) -> std::uint32_t
{
    const auto channel = [](float value) -> std::uint32_t {
        const float clamped = value < 0.0F ? 0.0F : (value > 1.0F ? 1.0F : value);
        return static_cast<std::uint32_t>(clamped * 255.0F + 0.5F);
    };
    return channel(color.r) | (channel(color.g) << 8U) |
           (channel(color.b) << 16U) | (channel(color.a) << 24U);
}

[[nodiscard]] constexpr auto unpackRgba8(std::uint32_t packed) -> Vec4
{
    constexpr float kScale = 1.0F / 255.0F;
    return {static_cast<float>(packed & 0xFFU) * kScale,
            static_cast<float>((packed >> 8U) & 0xFFU) * kScale,
            static_cast<float>((packed >> 16U) & 0xFFU) * kScale,
            static_cast<float>((packed >> 24U) & 0xFFU) * kScale};
}

[[nodiscard]] inline auto linearToSrgb(float linear) -> float
{
    if (linear <= 0.0031308F) {
        return 12.92F * linear;
    }
    return 1.055F * std::pow(linear, 1.0F / 2.4F) - 0.055F;
}

[[nodiscard]] inline auto linearToSrgb(Vec3 linear) -> Vec3
{
    return {linearToSrgb(linear.r), linearToSrgb(linear.g),
            linearToSrgb(linear.b)};
}

[[nodiscard]] inline auto srgbToLinear(float srgb) -> float
{
    if (srgb <= 0.04045F) {
        return srgb / 12.92F;
    }
    return std::pow((srgb + 0.055F) / 1.055F, 2.4F);
}

[[nodiscard]] inline auto srgbToLinear(Vec3 srgb) -> Vec3
{
    return {srgbToLinear(srgb.r), srgbToLinear(srgb.g), srgbToLinear(srgb.b)};
}

enum class ToneMapper : std::uint8_t
{
    None,      // Clamp only.
    Reinhard,  // c / (1 + c)
    Aces,      // Narkowicz 2015 ACES filmic fit.
    // Troy Sobotka's AgX (Blender's view transform), Benjamin Wrensch's fit of its base
    // contrast: some 16.5 stops from 1.8e-4 to 16, bright saturated light rolling off to
    // white as on film rather than clipping to a pure hue.
    AgX,
    // AgX with Blender's Punchy look (power 1.35, saturation 1.4): deeper blacks, faint
    // light fainter, colors richer.
    AgXPunchy,
};

template <>
struct EnumNames<ToneMapper>
{
    static constexpr std::array kNames = {"None", "Reinhard", "ACES", "AgX", "AgX Punchy"};
};

namespace detail
{

/** @brief AgX of linear Rec. 709, to linear display values (shaders/common.glsl's agx). */
[[nodiscard]] inline auto agx(Vec3 color, bool punchy) -> Vec3
{
    // Into AgX's working space (columns), its log2 encoding, its sigmoid, back out.
    const Mat3 inset(0.842479062253094F, 0.0423282422610123F, 0.0423756549057051F,
                     0.0784335999999992F, 0.878468636469772F, 0.0784336F, 0.0792237451477643F,
                     0.0791661274605434F, 0.879142973793104F);
    const Mat3 outset(1.19687900512017F, -0.0528968517574562F, -0.0529716355144438F,
                      -0.0980208811401368F, 1.15190312990417F, -0.0980434501171241F,
                      -0.0990297440797205F, -0.0989611768448433F, 1.15107367264116F);
    constexpr float kMinEv = -12.47393F;
    constexpr float kMaxEv = 4.026069F;
    Vec3 x = inset * glm::max(color, Vec3(0.0F));
    for (int c = 0; c < 3; ++c) {
        x[c] = (std::clamp(std::log2(std::max(x[c], 1e-10F)), kMinEv, kMaxEv) - kMinEv) /
               (kMaxEv - kMinEv);
    }
    const Vec3 x2 = x * x;
    const Vec3 x4 = x2 * x2;
    x = 15.5F * x4 * x2 - 40.14F * x4 * x + 31.96F * x4 - 6.868F * x2 * x + 0.4298F * x2 +
        0.1191F * x - 0.00232F;
    if (punchy) {
        // The look, on the encoded values (ASC CDL power, then saturation about the luma).
        const float luma = glm::dot(x, Vec3(0.2126F, 0.7152F, 0.0722F));
        x = glm::pow(glm::max(x, Vec3(0.0F)), Vec3(1.35F));
        x = Vec3(luma) + 1.4F * (x - Vec3(luma));
    }
    // The sigmoid's output is display-encoded (a 2.2 power): linear again for sRGB output.
    x = outset * x;
    return glm::pow(glm::clamp(x, 0.0F, 1.0F), Vec3(2.2F));
}

}  // namespace detail

[[nodiscard]] inline auto toneMap(Vec3 color, ToneMapper mapper) -> Vec3
{
    switch (mapper) {
    case ToneMapper::None:
        return glm::clamp(color, 0.0F, 1.0F);
    case ToneMapper::Reinhard:
        return color / (Vec3(1.0F) + color);
    case ToneMapper::Aces: {
        constexpr float a = 2.51F, b = 0.03F, c = 2.43F, d = 0.59F, e = 0.14F;
        return glm::clamp((color * (a * color + b)) /
                              (color * (c * color + d) + e),
                          0.0F, 1.0F);
    }
    case ToneMapper::AgX:
        return detail::agx(color, false);
    case ToneMapper::AgXPunchy:
        return detail::agx(color, true);
    }
    return color;
}

}  // namespace starcore
