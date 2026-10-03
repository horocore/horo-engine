#include "Horo/Release/UpdateRetention.h"
#include "Horo/Release/UpdateRetentionErrors.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <limits>

using namespace Horo::Release;

TEST_CASE("Retention removes the oldest obsolete version without touching protected versions", "[release][update][retention]") {
    const std::array versions{
        UpdateRetentionVersion{{"active"}, 40U, 10U, UpdateRetentionRole::Active},
        UpdateRetentionVersion{{"last-good"}, 30U, 9U, UpdateRetentionRole::LastKnownGood},
        UpdateRetentionVersion{{"obsolete-new"}, 20U, 8U, UpdateRetentionRole::Obsolete},
        UpdateRetentionVersion{{"obsolete-old"}, 15U, 1U, UpdateRetentionRole::Obsolete},
    };
    auto plan = PlanUpdateRetention(versions, 90U);
    REQUIRE(plan.HasValue());
    REQUIRE(plan.Value().remove.size() == 1U);
    CHECK(plan.Value().remove.front().value == "obsolete-old");
    CHECK(plan.Value().remainingBytes == 90U);
}

TEST_CASE("Retention uses a stable package-ID tie breaker and permits no-op plans", "[release][update][retention]") {
    const std::array versions{
        UpdateRetentionVersion{{"active"}, 40U, 10U, UpdateRetentionRole::Active},
        UpdateRetentionVersion{{"last-good"}, 30U, 9U, UpdateRetentionRole::LastKnownGood},
        UpdateRetentionVersion{{"obsolete-z"}, 20U, 1U, UpdateRetentionRole::Obsolete},
        UpdateRetentionVersion{{"obsolete-a"}, 20U, 1U, UpdateRetentionRole::Obsolete},
    };
    auto noOp = PlanUpdateRetention(versions, 110U);
    REQUIRE(noOp.HasValue());
    CHECK(noOp.Value().remove.empty());
    auto trim = PlanUpdateRetention(versions, 90U);
    REQUIRE(trim.HasValue());
    REQUIRE(trim.Value().remove.size() == 1U);
    CHECK(trim.Value().remove.front().value == "obsolete-a");
    CHECK(trim.Value().remainingBytes == 90U);
}

TEST_CASE("Retention fails when protected versions alone exceed budget", "[release][update][retention]") {
    const std::array versions{
        UpdateRetentionVersion{{"active"}, 40U, 10U, UpdateRetentionRole::Active},
        UpdateRetentionVersion{{"last-good"}, 30U, 9U, UpdateRetentionRole::LastKnownGood},
        UpdateRetentionVersion{{"obsolete"}, 20U, 1U, UpdateRetentionRole::Obsolete},
    };
    auto plan = PlanUpdateRetention(versions, 69U);
    REQUIRE(plan.HasError());
    CHECK(plan.ErrorValue().code.Value() == UpdateRetentionErrors::ProtectedBudgetExceeded.code.Value());
}

TEST_CASE("Retention rejects ambiguous identities and overflowing measurements", "[release][update][retention]") {
    const std::array duplicate{
        UpdateRetentionVersion{{"active"}, 40U, 10U, UpdateRetentionRole::Active},
        UpdateRetentionVersion{{"active"}, 30U, 9U, UpdateRetentionRole::LastKnownGood},
    };
    CHECK(PlanUpdateRetention(duplicate, 100U).HasError());
    const std::array overflow{
        UpdateRetentionVersion{{"active"}, std::numeric_limits<std::uint64_t>::max(), 10U, UpdateRetentionRole::Active},
        UpdateRetentionVersion{{"last-good"}, 1U, 9U, UpdateRetentionRole::LastKnownGood},
    };
    CHECK(PlanUpdateRetention(overflow, std::numeric_limits<std::uint64_t>::max()).HasError());
    const std::array missingPin{UpdateRetentionVersion{{"active"}, 10U, 1U, UpdateRetentionRole::Active}};
    CHECK(PlanUpdateRetention(missingPin, 20U).HasError());
}
