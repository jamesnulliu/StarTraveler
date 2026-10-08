export module starcore.scene:uuid;

import std;

export namespace starcore
{

/** @brief Stable 64-bit entity identifier used for serialization/references. */
class Uuid
{
public:
    /** @brief A new random (non-zero) id. */
    Uuid();

    constexpr explicit Uuid(std::uint64_t value) : m_value(value)
    {
    }

    [[nodiscard]] constexpr auto value() const noexcept -> std::uint64_t
    {
        return m_value;
    }

    constexpr auto operator<=>(const Uuid&) const = default;

private:
    std::uint64_t m_value;
};

}  // namespace starcore

template <>
struct std::hash<starcore::Uuid>
{
    auto operator()(const starcore::Uuid& id) const noexcept -> std::size_t
    {
        return std::hash<std::uint64_t>{}(id.value());
    }
};
