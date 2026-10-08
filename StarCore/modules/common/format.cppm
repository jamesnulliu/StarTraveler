// std::formatter specializations for glm types, so vectors/quaternions can be
// passed straight to std::format and the SC_* logging macros.

export module starcore.common:format;

import std;
import glm;

/** @brief Formats glm vectors as "(x, y, z)"; the format spec applies to every
 *         component, e.g. "{:.2f}". */
// `auto L`: glm's module does not export glm::length_t.
template <auto L, typename T, glm::qualifier Q>
struct std::formatter<glm::vec<L, T, Q>, char> : std::formatter<T, char>
{
    template <typename FormatContext>
    auto format(const glm::vec<L, T, Q>& v, FormatContext& ctx) const
    {
        auto out = ctx.out();
        *out++ = '(';
        for (decltype(L) i = 0; i < L; ++i) {
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
