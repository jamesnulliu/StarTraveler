export module starcore.math:color;

import std;
import glm;
import starcore.common;
import :types;

export namespace starcore
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
};

template <>
struct EnumNames<ToneMapper>
{
    static constexpr std::array kNames = {"None", "Reinhard", "ACES"};
};

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
    }
    return color;
}

}  // namespace starcore
