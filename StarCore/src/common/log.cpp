#include <memory>
#include <mutex>
#include <vector>

#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include "starcore/common/log.hpp"

namespace starcore::log
{

namespace
{

struct Loggers
{
    std::shared_ptr<spdlog::logger> core;
    std::shared_ptr<spdlog::logger> client;
};

std::mutex g_mutex;
// Guarded by g_mutex. References handed out by core()/client() must stay
// valid, so loggers replaced by init() are retired, not destroyed, until
// shutdown().
std::shared_ptr<Loggers> g_loggers;
std::vector<std::shared_ptr<Loggers>> g_retired;

auto makeLoggers(const LogConfig& config) -> std::shared_ptr<Loggers>
{
    // Format: [HH:MM:SS.mmm] [logger/level] message
    constexpr auto kPattern = "%^[%T.%e] [%n/%l]%$ %v";

    std::vector<spdlog::sink_ptr> sinks;
    sinks.push_back(std::make_shared<spdlog::sinks::stdout_color_sink_mt>());
    if (config.filePath.has_value()) {
        sinks.push_back(std::make_shared<spdlog::sinks::basic_file_sink_mt>(
            config.filePath->string(), /*truncate=*/false));
    }

    auto make = [&](const char* name) {
        auto logger =
            std::make_shared<spdlog::logger>(name, sinks.begin(), sinks.end());
        logger->set_pattern(kPattern);
        logger->set_level(config.level);
        logger->flush_on(spdlog::level::warn);
        return logger;
    };

    auto loggers = std::make_shared<Loggers>();
    loggers->core = make("Core");
    loggers->client = make("Client");
    return loggers;
}

auto loggers() -> Loggers&
{
    std::scoped_lock lock(g_mutex);
    if (!g_loggers) {
        g_loggers = makeLoggers({});
    }
    return *g_loggers;
}

}  // namespace

void init(const LogConfig& config)
{
    auto fresh = makeLoggers(config);
    std::scoped_lock lock(g_mutex);
    if (g_loggers) {
        g_retired.push_back(std::move(g_loggers));
    }
    g_loggers = std::move(fresh);
}

void shutdown()
{
    std::scoped_lock lock(g_mutex);
    if (g_loggers) {
        g_loggers->core->flush();
        g_loggers->client->flush();
    }
    g_loggers.reset();
    g_retired.clear();
}

auto core() -> spdlog::logger&
{
    return *loggers().core;
}

auto client() -> spdlog::logger&
{
    return *loggers().client;
}

}  // namespace starcore::log
