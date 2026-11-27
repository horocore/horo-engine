#include "Horo/Navigation/NavigationCrowdOverload.h"

#include <array>
#include <catch2/catch_test_macros.hpp>

namespace Horo::Navigation {
    TEST_CASE("Crowd demand keeps an older group ahead of newly renewed work", "[unit][navigation][crowd][budget]") {
        std::array<std::uint64_t, 4> waiting{1, 1, 1, 1};
        std::array<std::size_t, 2> groupCounts{};
        for (std::size_t round = 0; round < waiting.size(); ++round) {
            std::array demands{NavigationCrowdDemand{3, 1, waiting[3]}, NavigationCrowdDemand{1, 0, waiting[1]},
                               NavigationCrowdDemand{2, 0, waiting[2]}, NavigationCrowdDemand{0, 0, waiting[0]}};
            const auto result = SelectNavigationCrowdDemand(demands, groupCounts, {1, 1});
            REQUIRE(result.HasValue());
            REQUIRE(result.Value() == 1);
            CHECK(demands.front().agentIndex == round);
            waiting[demands.front().agentIndex] = round + 2;
        }
    }

    TEST_CASE("Crowd demand respects group ceilings and rejects malformed spatial facts", "[unit][navigation][crowd][budget]") {
        std::array demands{NavigationCrowdDemand{1, 0, 2}, NavigationCrowdDemand{2, 1, 3}, NavigationCrowdDemand{0, 0, 1}};
        std::array<std::size_t, 2> groupCounts{123, 123};
        const auto result = SelectNavigationCrowdDemand(demands, groupCounts, {3, 1});
        REQUIRE(result.HasValue());
        REQUIRE(result.Value() == 2);
        CHECK(demands[0].agentIndex == 0);
        CHECK(demands[1].agentIndex == 2);
        CHECK(groupCounts == std::array<std::size_t, 2>{1, 1});
        CHECK(SelectNavigationCrowdDemand(demands, groupCounts, {0, 0}).HasError());
        demands[0].groupIndex = groupCounts.size();
        CHECK(SelectNavigationCrowdDemand(demands, groupCounts, {3, 1}).HasError());
    }

    TEST_CASE("Crowd recovery contracts once per pressure observation and expands gradually", "[unit][navigation][crowd][budget]") {
        NavigationCrowdRecoveryBudget recovery;
        constexpr NavigationCrowdCapacity ceiling{8, 4};
        CHECK(recovery.Limit(ceiling).agents == 8);
        CHECK(recovery.Limit(ceiling).jobs == 4);
        recovery.Observe({1, 1}, false);
        CHECK(recovery.Limit(ceiling).agents == 8);
        recovery.Observe(ceiling, true);
        CHECK(recovery.Limit(ceiling).agents == 4);
        CHECK(recovery.Limit(ceiling).jobs == 2);
        recovery.Observe(recovery.Limit(ceiling), true);
        recovery.Observe(recovery.Limit(ceiling), true);
        recovery.Observe(recovery.Limit(ceiling), true);
        CHECK(recovery.Limit(ceiling).agents == 1);
        CHECK(recovery.Limit(ceiling).jobs == 1);
        recovery.Observe(recovery.Limit(ceiling), false);
        CHECK(recovery.Limit(ceiling).agents == 2);
        CHECK(recovery.Limit(ceiling).jobs == 2);
        CHECK(recovery.Limit({1, 1}).agents == 1);
        CHECK(recovery.Limit({1, 1}).jobs == 1);
    }
}  // namespace Horo::Navigation
