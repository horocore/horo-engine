#include "Horo/WorldStreaming/StreamingOwnerFrameBudget.h"
#include "WorldStreamingTestUtils.h"

#include <catch2/catch_test_macros.hpp>
#include <limits>
#include <utility>

namespace Horo::WorldStreaming {
    namespace {
        using TestSupport::IdentityFrom;
        using TestSupport::RequireError;

        StreamingOwnerFrameLimits Limits() {
            return {IdentityFrom<StreamingSchedulerLedgerId>(1), IdentityFrom<StreamingOwnerWorkRevision>(1),
                    IdentityFrom<StreamingOwnerFrameId>(1), 30, 2};
        }
    }  // namespace

    TEST_CASE("One shared frame bounds cumulative work and starts no unit after elapsed time expires",
              "[unit][world_streaming][frame_budget]") {
        const auto limits = Limits();
        auto budget = StreamingOwnerFrameBudget::Create(limits).Value();
        REQUIRE(budget.TryConsume(limits.owner, 10, 0).Value());
        REQUIRE(budget.TryConsume(limits.owner, 20, 10).Value());
        REQUIRE(budget.ChargedNanoseconds() == 30);
        REQUIRE(budget.ConsumedUnits() == 2);
        REQUIRE_FALSE(budget.TryConsume(limits.owner, 1, 30).Value());
        REQUIRE(budget.ChargedNanoseconds() == 30);
        auto timed = StreamingOwnerFrameBudget::Create(limits).Value();
        REQUIRE_FALSE(timed.TryConsume(limits.owner, 10, 21).Value());
        REQUIRE(timed.ConsumedUnits() == 0);
        REQUIRE_FALSE(timed.TryConsume(limits.owner, 1, std::numeric_limits<std::uint64_t>::max()).Value());
        RequireError(timed.TryConsume(limits.owner, 1, 21), WorldStreamingErrors::OwnerFrameStale);
    }

    TEST_CASE("Frame units independently bound many cheap callbacks and overflow is never admitted",
              "[unit][world_streaming][frame_budget]") {
        auto limits = Limits();
        limits.maximumNanoseconds = std::numeric_limits<std::uint64_t>::max();
        auto budget = StreamingOwnerFrameBudget::Create(limits).Value();
        REQUIRE(budget.TryConsume(limits.owner, limits.maximumNanoseconds - 1, 0).Value());
        REQUIRE_FALSE(budget.TryConsume(limits.owner, 2, 0).Value());
        REQUIRE(budget.TryConsume(limits.owner, 1, 0).Value());
        REQUIRE(budget.ChargedNanoseconds() == limits.maximumNanoseconds);
        auto cheap = StreamingOwnerFrameBudget::Create(limits).Value();
        REQUIRE(cheap.TryConsume(limits.owner, 1, 0).Value());
        REQUIRE(cheap.TryConsume(limits.owner, 1, 0).Value());
        REQUIRE_FALSE(cheap.TryConsume(limits.owner, 1, 0).Value());
        REQUIRE(cheap.ChargedNanoseconds() == 2);
    }

    TEST_CASE("Invalid frame facts and foreign owner fail without consuming work", "[unit][world_streaming][frame_budget][failure]") {
        const auto limits = Limits();
        for (int field = 0; field < 5; ++field) {
            auto invalid = limits;
            switch (field) {
                case 0:
                    invalid.owner = {};
                    break;
                case 1:
                    invalid.policyRevision = {};
                    break;
                case 2:
                    invalid.frame = {};
                    break;
                case 3:
                    invalid.maximumNanoseconds = 0;
                    break;
                case 4:
                    invalid.maximumUnits = 0;
                    break;
            }
            RequireError(StreamingOwnerFrameBudget::Create(invalid), WorldStreamingErrors::OwnerFrameInvalid);
        }
        auto budget = StreamingOwnerFrameBudget::Create(limits).Value();
        RequireError(budget.TryConsume({}, 10, 0), WorldStreamingErrors::OwnerFrameInvalid);
        RequireError(budget.TryConsume(limits.owner, 0, 0), WorldStreamingErrors::OwnerFrameInvalid);
        RequireError(budget.TryConsume(IdentityFrom<StreamingSchedulerLedgerId>(2), 10, 0), WorldStreamingErrors::OwnerFrameStale);
        RequireError(budget.TryConsume(limits.owner, 31, 0), WorldStreamingErrors::OwnerFrameCapacityExceeded);
        REQUIRE(budget.ConsumedUnits() == 0);
        REQUIRE(budget.ChargedNanoseconds() == 0);
        REQUIRE(budget.TryConsume(limits.owner, 10, 10).Value());
        RequireError(budget.TryConsume(limits.owner, 1, 9), WorldStreamingErrors::OwnerFrameStale);
        REQUIRE(budget.ConsumedUnits() == 1);
        REQUIRE(budget.TryConsume(limits.owner, 10, 10).Value());
    }

    TEST_CASE("Moving frame ownership preserves accounting and closes its source", "[unit][world_streaming][frame_budget][ownership]") {
        const auto limits = Limits();
        auto source = StreamingOwnerFrameBudget::Create(limits).Value();
        REQUIRE(source.TryConsume(limits.owner, 10, 5).Value());
        auto budget = std::move(source);
        RequireError(source.TryConsume(limits.owner, 1, 5), WorldStreamingErrors::OwnerFrameStale);
        REQUIRE(budget.Limits().owner == limits.owner);
        REQUIRE(budget.Limits().frame == limits.frame);
        REQUIRE(budget.Limits().policyRevision == limits.policyRevision);
        REQUIRE(budget.ChargedNanoseconds() == 10);
        REQUIRE(budget.ConsumedUnits() == 1);
        REQUIRE(budget.TryConsume(limits.owner, 20, 10).Value());
    }
}  // namespace Horo::WorldStreaming
