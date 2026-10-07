#pragma once

#include <filesystem>
#include <optional>

#include <spdlog/spdlog.h>

namespace starcore::log
{

struct LogConfig
{
    spdlog::level::level_enum level = spdlog::level::info;
    // Also write all messages to this file (appending) when set.
    std::optional<std::filesystem::path> filePath = std::nullopt;
};

/**
 * @brief (Re)create the "Core" and "Client" loggers.
 *
 * Calling it is optional: the loggers are created lazily with the default
 * config on first use, so headless users (tests, Python) need no setup.
 */
void init(const LogConfig& config = {});

/** @brief Flush and drop all loggers. */
void shutdown();

/** @brief Logger for StarCore itself. */
auto core() -> spdlog::logger&;

/** @brief Logger for applications built on StarCore (e.g. StarTraveler). */
auto client() -> spdlog::logger&;

}  // namespace starcore::log

// clang-format off
#define SC_CORE_TRACE(...)    ::starcore::log::core().trace(__VA_ARGS__)
#define SC_CORE_DEBUG(...)    ::starcore::log::core().debug(__VA_ARGS__)
#define SC_CORE_INFO(...)     ::starcore::log::core().info(__VA_ARGS__)
#define SC_CORE_WARN(...)     ::starcore::log::core().warn(__VA_ARGS__)
#define SC_CORE_ERROR(...)    ::starcore::log::core().error(__VA_ARGS__)
#define SC_CORE_CRITICAL(...) ::starcore::log::core().critical(__VA_ARGS__)

#define SC_TRACE(...)    ::starcore::log::client().trace(__VA_ARGS__)
#define SC_DEBUG(...)    ::starcore::log::client().debug(__VA_ARGS__)
#define SC_INFO(...)     ::starcore::log::client().info(__VA_ARGS__)
#define SC_WARN(...)     ::starcore::log::client().warn(__VA_ARGS__)
#define SC_ERROR(...)    ::starcore::log::client().error(__VA_ARGS__)
#define SC_CRITICAL(...) ::starcore::log::client().critical(__VA_ARGS__)
// clang-format on
