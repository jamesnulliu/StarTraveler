export module starcore.math:types;

import std;

// glm's own module (glm/glm.cppm, built as part of StarCore::Core). Re-exported:
// the vector types below are glm's, and so are their operators and functions.
export import glm;

export namespace starcore
{

using Vec2 = glm::vec2;
using Vec3 = glm::vec3;
using Vec4 = glm::vec4;
using IVec2 = glm::ivec2;
using UVec2 = glm::uvec2;
using Mat3 = glm::mat3;
using Mat4 = glm::mat4;
using Quat = glm::quat;

inline constexpr float kPi = std::numbers::pi_v<float>;
inline constexpr float kTwoPi = 2.0F * kPi;
inline constexpr float kInfinity = std::numeric_limits<float>::infinity();

/** @brief Axis-aligned rectangle [min, max] in 2D (pixels or NDC). */
struct Rect
{
    Vec2 min{0.0F};
    Vec2 max{0.0F};

    [[nodiscard]] constexpr auto contains(Vec2 p) const -> bool
    {
        return p.x >= min.x && p.x <= max.x && p.y >= min.y && p.y <= max.y;
    }
};

}  // namespace starcore
