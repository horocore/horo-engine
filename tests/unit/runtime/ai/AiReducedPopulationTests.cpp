#include "Horo/AI/AiReducedPopulation.h"

#include <catch2/catch_test_macros.hpp>

#include <limits>
#include <thread>

namespace Horo::AI {
    namespace {
        TEST_CASE("Reduced population proposes a bounded batch without constructing full agents", "[ai][reduced-population]") {
            constexpr std::size_t count = 65'536;
            std::vector<AiReducedAgent> rows;
            rows.reserve(count);
            for (std::size_t id = 1; id <= count; ++id)
                rows.push_back({AgentId::Create(id).Value(), {25, 0, -25}});
            rows.front().positionMillimeters[0] = std::numeric_limits<std::int64_t>::max();

            auto created = AiReducedPopulation::Create({count, 2, 25}, rows);
            REQUIRE(created.HasValue());
            auto &population = created.Value();
            std::array<AiReducedAgent, 2> output{};
            CHECK(population.EvaluatePromotions({0, 0, 0}, {}).HasError());
            const auto evaluated = population.EvaluatePromotions({0, 0, 0}, output);
            REQUIRE(evaluated.HasValue());
            CHECK(evaluated.Value() == output.size());
            CHECK(output.front().agent == rows[1].agent);
            CHECK(output.back().agent == rows[2].agent);
            const auto overflow = population.EvaluatePromotions({std::numeric_limits<std::int64_t>::min(), 0, 0}, output);
            REQUIRE(overflow.HasValue());
            CHECK(overflow.Value() == 0);
        }

        TEST_CASE("Reduced population transfers identity only when its transaction publishes", "[ai][reduced-population]") {
            const auto first = AgentId::Create(1).Value();
            const auto second = AgentId::Create(2).Value();
            const auto missing = AgentId::Create(3).Value();
            const std::array rows{AiReducedAgent{first, {1, 0, 0}}, AiReducedAgent{second, {2, 0, 0}}};
            auto created = AiReducedPopulation::Create({rows.size(), 2, 10}, rows);
            REQUIRE(created.HasValue());
            auto &population = created.Value();
            std::array<AiReducedAgent, 2> output{};
            const std::array invalid{first, missing};
            CHECK(population.PrepareTransition(invalid, {}).HasError());
            CHECK(population.PrepareTransition(std::span{&first, 1}, rows).HasError());
            auto stale = population.PrepareTransition(std::span{&second, 1}, {});
            REQUIRE(stale.HasValue());
            {
                auto candidate = population.PrepareTransition(std::span{&first, 1}, {});
                REQUIRE(candidate.HasValue());
                REQUIRE(candidate.Value().ValidatePublication().HasValue());
            }
            CHECK(population.EvaluatePromotions({0, 0, 0}, output).Value() == 2);

            auto promote = population.PrepareTransition(std::span{&first, 1}, {});
            REQUIRE(promote.HasValue());
            REQUIRE(promote.Value().Publish().HasValue());
            CHECK(stale.Value().Publish().HasError());
            CHECK(promote.Value().Publish().HasError());
            CHECK(population.EvaluatePromotions({0, 0, 0}, output).Value() == 1);
            CHECK(output.front().agent == second);

            const AiReducedAgent demoted{first, {10, 4, -2}};
            auto swap = population.PrepareTransition(std::span{&second, 1}, std::span{&demoted, 1});
            REQUIRE(swap.HasValue());
            bool rejectedOnForeignLane = false;
            std::thread foreign([&] { rejectedOnForeignLane = swap.Value().Publish().HasError(); });
            foreign.join();
            CHECK(rejectedOnForeignLane);
            REQUIRE(swap.Value().Publish().HasValue());
            CHECK(population.EvaluatePromotions({0, 0, 0}, output).Value() == 1);
            CHECK(output.front().agent == first);
            CHECK(output.front().positionMillimeters == demoted.positionMillimeters);
        }
    }  // namespace
}  // namespace Horo::AI
