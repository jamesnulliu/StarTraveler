#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "starcore/common/log.hpp"

import std;
import starcore.common;

TEST_CASE("ParallelExecutor runs every index exactly once", "[common][parallel]")
{
    const unsigned threads = GENERATE(1U, 2U, 8U);
    starcore::ParallelExecutor executor(threads);
    REQUIRE(executor.threadCount() == threads);

    // Many back-to-back jobs (and destruction afterwards) must not deadlock.
    for (int job = 0; job < 200; ++job) {
        std::vector<std::atomic<int>> hits(97);
        executor.parallelFor(hits.size(), [&](std::size_t i) { ++hits[i]; });
        for (const auto& hit : hits) {
            REQUIRE(hit.load() == 1);
        }
    }
    executor.parallelFor(0, [](std::size_t) { FAIL("must not be called"); });
}

TEST_CASE("ParallelExecutor rethrows task exceptions", "[common][parallel]")
{
    starcore::ParallelExecutor executor(4);
    REQUIRE_THROWS_AS(executor.parallelFor(64,
                                           [](std::size_t i) {
                                               if (i == 13) {
                                                   throw std::runtime_error("boom");
                                               }
                                           }),
                      std::runtime_error);
    // Still usable afterwards.
    std::atomic<int> count{0};
    executor.parallelFor(10, [&](std::size_t) { ++count; });
    REQUIRE(count == 10);
}

TEST_CASE("Image2D indexing and mdspan view agree", "[common][image]")
{
    starcore::Image2D<int> image(4, 3, 0);
    image.at(3, 1) = 7;
    REQUIRE(image.view()[1, 3] == 7);
    REQUIRE(image.pixels()[1 * 4 + 3] == 7);
    REQUIRE(image.contains(3, 2));
    REQUIRE_FALSE(image.contains(4, 0));
    REQUIRE_FALSE(image.contains(-1, 0));
}

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
