#pragma once

#include <format>
#include <source_location>
#include <string>
#include <string_view>
#include <utility>

namespace starcore::detail
{

[[noreturn]] void assertFailed(std::string_view expression,
                               std::string_view message,
                               std::source_location location);

inline auto assertMessage() -> std::string
{
    return {};
}

template <typename... Args>
auto assertMessage(std::format_string<Args...> fmt, Args&&... args)
    -> std::string
{
    return std::format(fmt, std::forward<Args>(args)...);
}

}  // namespace starcore::detail

/**
 * @brief Always-on check: logs the failed expression (and an optional
 *        std::format message), breaks into the debugger and aborts.
 * @example SC_VERIFY(index < size, "index {} out of range", index);
 */
#define SC_VERIFY(condition, ...)                                              \
    do {                                                                       \
        if (!(condition)) [[unlikely]] {                                       \
            ::starcore::detail::assertFailed(                                  \
                #condition,                                                    \
                ::starcore::detail::assertMessage(__VA_ARGS__),                \
                std::source_location::current());                              \
        }                                                                      \
    } while (false)

/**
 * @brief Debug-only check (enabled when NDEBUG is not defined or
 *        STARCORE_ENABLE_ASSERTS is defined). The condition is not evaluated
 *        in release builds.
 */
#if !defined(NDEBUG) || defined(STARCORE_ENABLE_ASSERTS)
    #define SC_ASSERT(condition, ...) SC_VERIFY(condition, __VA_ARGS__)
#else
    #define SC_ASSERT(condition, ...)                                          \
        do {                                                                   \
            (void) sizeof(condition);                                          \
        } while (false)
#endif
