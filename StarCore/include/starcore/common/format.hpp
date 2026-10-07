#pragma once
// std::formatter specializations for glm types, so vectors/quaternions can be
// passed straight to std::format and the SC_* logging macros.

#include <format>

#include <glm/gtc/quaternion.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

/** @brief Formats glm vectors as "(x, y, z)"; the format spec applies to every
 *         component, e.g. "{:.2f}". */
template <glm::length_t L, typename T, glm::qualifier Q>
struct std::formatter<glm::vec<L, T, Q>, char> : std::formatter<T, char>
{
    template <typename FormatContext>
    auto format(const glm::vec<L, T, Q>& v, FormatContext& ctx) const
    {
        auto out = ctx.out();
        *out++ = '(';
        for (glm::length_t i = 0; i < L; ++i) {
            if (i != 0) {
                *out++ = ',';
                *out++ = ' ';
            }
            ctx.advance_to(out);
            out = std::formatter<T, char>::format(v[i], ctx);
        }
        *out++ = ')';
        return out;
    }
};

/** @brief Formats glm quaternions as "quat(w, x, y, z)". */
template <typename T, glm::qualifier Q>
struct std::formatter<glm::qua<T, Q>, char> : std::formatter<T, char>
{
    template <typename FormatContext>
    auto format(const glm::qua<T, Q>& q, FormatContext& ctx) const
    {
        auto out = std::format_to(ctx.out(), "quat(");
        const T components[] = {q.w, q.x, q.y, q.z};
        for (int i = 0; i < 4; ++i) {
            if (i != 0) {
                *out++ = ',';
                *out++ = ' ';
            }
            ctx.advance_to(out);
            out = std::formatter<T, char>::format(components[i], ctx);
        }
        *out++ = ')';
        return out;
    }
};
