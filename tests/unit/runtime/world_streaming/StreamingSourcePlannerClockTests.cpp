#include "StreamingPlannerFixture.h"
#include "WorldStreamingTestUtils.h"

#include <catch2/catch_test_macros.hpp>

namespace Horo::WorldStreaming {
    namespace {
        using namespace PlannerFixture;
        using TestSupport::RequireError;
        constexpr std::uint32_t DenseCells = 65'536;
        constexpr std::int32_t DenseSide = 256;

        /** @brief Every invocation advances the unscaled virtual clock at a 60Hz host cadence, without sleeping. */
        std::uint64_t FrameTime(const std::uint32_t frame) {
            return 1'001 + static_cast<std::uint64_t>(frame) * 1'000 / 60;
        }

        StreamingSourcePlannerConfig DenseProfile(const std::uint64_t maximumAge, const std::uint32_t budget) {
            auto config = Config(DenseCells);
            config.limits.maximumPlanAgeMilliseconds = maximumAge;
            config.limits.nodesPerFrame = budget;
            config.limits.candidatesPerFrame = budget;
            config.limits.stabilityRowsPerFrame = budget;
            return config;
        }

        StreamingSourceRequest DenseSource() {
            auto request = Source(1, StreamingSourceIntent::Camera);
            request.shape = StreamingSourceBox{Point(0), Point(DenseSide * 100 - 1, DenseSide * 100 - 1, 99)};
            return request;
        }

        /** @brief Motion never hard-replaces the admitted source or resets its in-flight cursor. */
        void MoveForFrame(StreamingSourceRegistration &source, const std::uint32_t frame) {
            REQUIRE(source.SampleMotion({Point(frame % 2), {}, FrameTime(frame)}).HasValue());
        }

        /** @brief Checks actual consumed work, not just output row capacity. */
        void RequireBudget(const StreamingSourcePlannerProgress &progress, const std::uint32_t budget) {
            REQUIRE(progress.sourceVisits <= 2);
            REQUIRE(progress.nodesVisited <= budget);
            REQUIRE(progress.candidatesExamined <= budget);
            REQUIRE(progress.stabilityRows <= budget);
        }

        void RequireDenseProgress(const std::uint64_t maximumAge, const std::uint32_t budget, const std::uint32_t frameLimit) {
            auto planner = StreamingSourcePlanner::Create(DenseProfile(maximumAge, budget), Partition(DenseSide), Environment()).Value();
            auto source = planner->Register(DenseSource()).Value();
            std::uint32_t publications = 0;
            for (std::uint32_t frame = 0; frame < frameLimit && publications < 2; ++frame) {
                MoveForFrame(source, frame);
                const auto progress = planner->Service(FrameTime(frame));
                REQUIRE(progress.HasValue());
                RequireBudget(progress.Value(), budget);
                if (progress.Value().published) {
                    ++publications;
                    const auto plan = planner->Plan().Value();
                    REQUIRE(plan.cells.size() == DenseCells);
                    REQUIRE(plan.contributions.size() == DenseCells);
                    REQUIRE(FrameTime(frame) - plan.sampledAtServiceMilliseconds <= maximumAge);
                }
            }
            REQUIRE(publications >= 2);
        }

        TEST_CASE("Dense advancing-clock motion publishes repeatedly with an explicitly compatible long-age profile",
                  "[world_streaming][source_planner][clock][qualification]") {
            RequireDenseProgress(60'000, 128, 6'000);
        }

        TEST_CASE("Dense advancing-clock motion publishes with a separately selected short-age higher-budget profile",
                  "[world_streaming][source_planner][clock][qualification]") {
            RequireDenseProgress(2'000, 4'096, 256);
        }

        TEST_CASE("Dense incompatible cadence expires typed and never exposes an aged previous plan",
                  "[world_streaming][source_planner][clock][qualification]") {
            auto planner = StreamingSourcePlanner::Create(DenseProfile(2'000, 128), Partition(DenseSide), Environment()).Value();
            auto source = planner->Register(DenseSource()).Value();
            // A valid initial CPU/work-only cycle lets the test check revocation of a real earlier publication.
            REQUIRE(Finish(*planner).cells.size() == DenseCells);
            std::uint32_t expirations = 0;
            for (std::uint32_t frame = 0; frame < 6'000 && expirations < 2; ++frame) {
                MoveForFrame(source, frame);
                const auto progress = planner->Service(FrameTime(frame));
                if (progress.HasError()) {
                    RequireError(progress, WorldStreamingErrors::PrefetchStale);
                    ++expirations;
                } else {
                    RequireBudget(progress.Value(), 128);
                    REQUIRE_FALSE(progress.Value().published);
                }
                const auto plan = planner->Plan();
                if (FrameTime(frame) - 1'000 > 2'000 || expirations != 0)
                    RequireError(plan, WorldStreamingErrors::PartitionRegistryUnavailable);
                else {
                    REQUIRE(plan.HasValue());
                    REQUIRE(FrameTime(frame) - plan.Value().sampledAtServiceMilliseconds <= 2'000);
                }
            }
            REQUIRE(expirations >= 2);
        }
    }  // namespace
}  // namespace Horo::WorldStreaming
