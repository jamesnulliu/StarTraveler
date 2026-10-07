#pragma once

#include <cstddef>
#include <string_view>

namespace starcore
{

/**
 * @brief Specialize with `static constexpr std::array kNames = {...};`
 *        listing the names of a contiguous enum starting at 0.
 */
template <typename E>
struct EnumNames;

template <typename E>
[[nodiscard]] constexpr auto enumToString(E value) -> std::string_view
{
    const auto index = static_cast<std::size_t>(value);
    return index < EnumNames<E>::kNames.size() ? EnumNames<E>::kNames[index]
                                                : "Unknown";
}

template <typename E>
[[nodiscard]] constexpr auto enumFromString(std::string_view name, E& out)
    -> bool
{
    for (std::size_t i = 0; i < EnumNames<E>::kNames.size(); ++i) {
        if (EnumNames<E>::kNames[i] == name) {
            out = static_cast<E>(i);
            return true;
        }
    }
    return false;
}

}  // namespace starcore
