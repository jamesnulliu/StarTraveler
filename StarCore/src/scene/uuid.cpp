#include <random>

#include "starcore/scene/uuid.hpp"

namespace starcore
{

namespace
{

auto randomUuidValue() -> std::uint64_t
{
    thread_local std::mt19937_64 engine{std::random_device{}()};
    std::uint64_t value = 0;
    while (value == 0) {
        value = engine();
    }
    return value;
}

}  // namespace

Uuid::Uuid() : m_value(randomUuidValue())
{
}

}  // namespace starcore
