#include <format>

#include <catch2/catch_test_macros.hpp>

#include <starcore/common/log.hpp>
#include <starcore/common/version.hpp>

TEST_CASE("Version string matches the version numbers", "[common]")
{
    const auto expected =
        std::format("{}.{}.{}", starcore::kVersionMajor,
                    starcore::kVersionMinor, starcore::kVersionPatch);
    REQUIRE(starcore::kVersionString == expected);
}

TEST_CASE("Loggers are usable without explicit init", "[common][log]")
{
    REQUIRE(starcore::log::core().name() == "Core");
    REQUIRE(starcore::log::client().name() == "Client");
    SC_CORE_DEBUG("debug message from {}", "test");
}
