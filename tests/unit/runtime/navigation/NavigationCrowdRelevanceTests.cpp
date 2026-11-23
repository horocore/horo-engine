#include "Horo/Navigation/NavigationCrowdRelevance.h"

#include <catch2/catch_test_macros.hpp>

namespace Horo::Navigation {
    namespace {
        constexpr NavigationCrowdRelevanceProfile Profile{.fullDistanceMillimeters = 1000,
                                                          .reducedDistanceMillimeters = 2000,
                                                          .suspensionDistanceMillimeters = 4000,
                                                          .reducedIntervalTicks = 3,
                                                          .demotionDelayTicks = 2};

        /** @brief Advances the software-only admission owner with the authored test bands. */
        NavigationCrowdAdmission Advance(NavigationCrowdCadence &cadence, const NavigationCrowdRelevance &relevance,
                                         const std::uint64_t tick) {
            const auto result = cadence.Advance(Profile, relevance, tick);
            REQUIRE(result.HasValue());
            return result.Value();
        }
    }  // namespace

    TEST_CASE("Crowd relevance delays a demotion and restores avoidance immediately", "[unit][navigation][crowd][relevance]") {
        NavigationCrowdCadence cadence;
        const NavigationCrowdRelevance far{.anchorDistanceMillimeters = 100'000};
        CHECK(Advance(cadence, far, 1).tier == NavigationCrowdTier::Full);
        CHECK(Advance(cadence, far, 2).tier == NavigationCrowdTier::Full);
        const auto suspended = Advance(cadence, far, 3);
        CHECK(suspended.tierChanged);
        CHECK(suspended.tier == NavigationCrowdTier::Suspended);
        CHECK(suspended.work == NavigationCrowdWork::Suspended);

        auto resident = far;
        resident.activeSimulationRegion = true;
        const auto path = Advance(cadence, resident, 4);
        CHECK(path.tierChanged);
        CHECK(path.tier == NavigationCrowdTier::PathOnly);
        CHECK(path.work == NavigationCrowdWork::PathOnly);

        resident.gameplayPinned = true;
        const auto pinned = Advance(cadence, resident, 5);
        CHECK(pinned.tierChanged);
        CHECK(pinned.tier == NavigationCrowdTier::Full);
        CHECK(pinned.work == NavigationCrowdWork::Avoidance);
    }

    TEST_CASE("Reduced crowd cadence admits only due solves over continuous fixed ticks", "[unit][navigation][crowd][relevance]") {
        NavigationCrowdCadence cadence;
        const NavigationCrowdRelevance reduced{.anchorDistanceMillimeters = Profile.reducedDistanceMillimeters};
        CHECK(Advance(cadence, reduced, 1).tier == NavigationCrowdTier::Full);
        CHECK(Advance(cadence, reduced, 2).tier == NavigationCrowdTier::Full);
        const auto first = Advance(cadence, reduced, 3);
        CHECK(first.tierChanged);
        CHECK(first.tier == NavigationCrowdTier::Reduced);
        CHECK(first.work == NavigationCrowdWork::Avoidance);
        CHECK(Advance(cadence, reduced, 4).work == NavigationCrowdWork::PathOnly);
        CHECK(Advance(cadence, reduced, 5).work == NavigationCrowdWork::PathOnly);
        CHECK(Advance(cadence, reduced, 6).work == NavigationCrowdWork::Avoidance);
        const auto unknown = Advance(cadence, {}, 7);
        CHECK(unknown.tier == NavigationCrowdTier::Full);
        CHECK(unknown.work == NavigationCrowdWork::Avoidance);
    }

    TEST_CASE("Crowd cadence neither backdates relevance nor lets unseen ticks satisfy hysteresis",
              "[unit][navigation][crowd][relevance]") {
        NavigationCrowdCadence cadence;
        const NavigationCrowdRelevance path{.anchorDistanceMillimeters = Profile.suspensionDistanceMillimeters};
        const NavigationCrowdRelevance far{.anchorDistanceMillimeters = Profile.suspensionDistanceMillimeters + 1};
        auto invalid = Profile;
        invalid.reducedIntervalTicks = 1;
        CHECK(cadence.Advance(invalid, far, 1).HasError());
        CHECK(cadence.Advance(Profile, far, 0).HasError());
        CHECK(Advance(cadence, far, 1).tier == NavigationCrowdTier::Full);
        CHECK(cadence.Advance(Profile, {}, 1).HasError());
        CHECK(Advance(cadence, far, 7).tier == NavigationCrowdTier::Full);
        CHECK(Advance(cadence, path, 8).tier == NavigationCrowdTier::Full);
        CHECK(Advance(cadence, path, 9).tier == NavigationCrowdTier::Full);
        CHECK(Advance(cadence, path, 10).tier == NavigationCrowdTier::PathOnly);
        CHECK(cadence.Advance(Profile, {}, 9).HasError());
    }
}  // namespace Horo::Navigation
