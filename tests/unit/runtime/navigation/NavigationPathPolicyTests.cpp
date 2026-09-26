#include "Horo/Navigation/NavigationErrors.h"
#include "Horo/Navigation/NavigationPathPolicy.h"

#include <array>
#include <catch2/catch_test_macros.hpp>

namespace Horo::Navigation {
    namespace {
        template <typename Id> Id Identity(const std::uint64_t value) {
            return Id::Create(value).Value();
        }

        NavigationOutcomeProvenance Source() {
            return {.snapshot = Identity<NavigationSnapshotToken>(1),
                    .world = Identity<NavigationWorldId>(2),
                    .topology = Identity<NavigationGeneration>(3),
                    .obstacleRevision = 4,
                    .filterRevision = 5,
                    .profileRevision = 6,
                    .originRevision = 7,
                    .completionTick = 8};
        }

        NavigationPathObservation Observation(const NavigationOutcomeProvenance &source,
                                              const std::span<const NavigationCoverageDependency> regions, const std::uint64_t tick = 10) {
            return {.provenance = source, .regions = regions, .linkRevision = 9, .goalRevision = 10, .tick = tick};
        }

        NavigationPath Path(const NavigationOutcomeProvenance &source) {
            NavigationPath path;
            path.status = NavigationPathStatus::Reachable;
            path.sourceGeneration = source.topology;
            path.corridor.push_back(
                {.provenance = {.world = source.world, .topology = source.topology, .surface = Identity<SurfaceId>(11), .polygonIndex = 1},
                 .area = Identity<NavigationAreaId>(1)});
            return path;
        }

        PathId Id(const NavigationWorldId world, const std::uint32_t generation = 1) {
            return {.world = world, .slot = {.index = 0, .generation = generation}};
        }
    }  // namespace

    TEST_CASE("Held navigation paths require exact current source evidence before use", "[unit][navigation][path-policy]") {
        const auto source = Source();
        const std::array regions{NavigationCoverageDependency{.region = 42, .generation = Identity<NavigationGeneration>(3)}};
        const auto coverage = NavigationCoverageEvidence::CompleteExact(regions).Value();
        auto now = Observation(source, regions);
        NavigationPathPolicy policy({.debounceTicks = 2, .cooldownTicks = 3});
        REQUIRE(policy.Install(Id(source.world), Path(source), source, coverage, 9, 10, now).HasValue());
        REQUIRE(policy.Id() == Id(source.world));
        REQUIRE(policy.CurrentPath(now) != nullptr);

        const auto previous = policy.CurrentPath(now);
        now.provenance.snapshot = Identity<NavigationSnapshotToken>(2);
        REQUIRE(policy.CurrentPath(now) == previous);  // A new token alone does not change a dependency.

        const std::array changed{NavigationCoverageDependency{.region = 42, .generation = Identity<NavigationGeneration>(4)}};
        now.regions = changed;
        now.tick = 11;
        REQUIRE(policy.CurrentPath(now) == nullptr);
        const NavigationPathInvalidation expected{NavigationPathInvalidationKind::Tile, 42, 3, 4};
        REQUIRE(policy.LastInvalidation() == expected);
        now.tick = 12;
        REQUIRE(policy.Observe(now).HasValue());
        REQUIRE(policy.Decide(12).action == NavigationRepathAction::Debouncing);
        now.tick = 13;
        REQUIRE(policy.Observe(now).HasValue());
        REQUIRE(policy.Decide(13).action == NavigationRepathAction::Submit);
        REQUIRE(policy.MarkSubmitted(13));
        REQUIRE(policy.CurrentPath(now) == nullptr);
        REQUIRE(policy.Decide(13).action == NavigationRepathAction::None);
        now.regions = regions;
        now.tick = 14;
        REQUIRE(policy.CurrentPath(now) == nullptr);  // A recycled observation cannot resurrect a stale corridor.
    }

    TEST_CASE("Path invalidation identifies world tile filter link obstacle and target changes", "[unit][navigation][path-policy]") {
        const auto source = Source();
        const std::array regions{NavigationCoverageDependency{.region = 42, .generation = Identity<NavigationGeneration>(3)}};
        const auto coverage = NavigationCoverageEvidence::CompleteExact(regions).Value();
        const auto original = Observation(source, regions);
        const auto check = [&](const NavigationPathObservation &changed, const NavigationPathInvalidationKind expected) {
            NavigationPathPolicy policy({});
            REQUIRE(policy.Install(Id(source.world), Path(source), source, coverage, 9, 10, original).HasValue());
            const auto result = policy.Observe(changed);
            REQUIRE(result.HasValue());
            REQUIRE(result.Value().kind == expected);
            REQUIRE(policy.CurrentPath(changed) == nullptr);
        };

        auto changed = original;
        changed.provenance.world = Identity<NavigationWorldId>(12);
        check(changed, NavigationPathInvalidationKind::World);
        changed = original;
        changed.provenance.filterRevision++;
        check(changed, NavigationPathInvalidationKind::Filter);
        changed = original;
        changed.provenance.profileRevision++;
        check(changed, NavigationPathInvalidationKind::Profile);
        changed = original;
        changed.provenance.originRevision++;
        check(changed, NavigationPathInvalidationKind::Origin);
        changed = original;
        changed.provenance.obstacleRevision++;
        check(changed, NavigationPathInvalidationKind::Obstacle);
        changed = original;
        changed.linkRevision++;
        check(changed, NavigationPathInvalidationKind::Link);
        changed = original;
        changed.goalRevision++;
        check(changed, NavigationPathInvalidationKind::Target);
        changed = original;
        changed.provenance.topology = Identity<NavigationGeneration>(4);
        check(changed, NavigationPathInvalidationKind::Topology);
        changed = original;
        changed.regions = {};
        check(changed, NavigationPathInvalidationKind::Tile);
    }

    TEST_CASE("Rapid target changes coalesce with debounce cooldown and forced bypass", "[unit][navigation][path-policy]") {
        const auto source = Source();
        const auto coverage = NavigationCoverageEvidence::CompleteTopology();
        auto now = Observation(source, {});
        NavigationPathPolicy policy({.debounceTicks = 2, .cooldownTicks = 5});
        REQUIRE(policy.Install(Id(source.world), Path(source), source, coverage, 9, 10, now).HasValue());
        now.goalRevision = 11;
        now.tick = 11;
        REQUIRE(policy.Observe(now).Value().kind == NavigationPathInvalidationKind::Target);
        now.goalRevision = 12;
        now.tick = 12;
        REQUIRE(policy.Observe(now).Value().observed == 12);
        now.goalRevision = 13;
        now.tick = 13;
        REQUIRE(policy.Observe(now).Value().observed == 13);
        now.tick = 14;
        REQUIRE(policy.Observe(now).HasValue());
        REQUIRE(policy.Decide(14).action == NavigationRepathAction::Debouncing);
        now.tick = 15;
        REQUIRE(policy.Observe(now).HasValue());
        const auto ready = policy.Decide(15);
        REQUIRE(ready.action == NavigationRepathAction::Submit);
        REQUIRE(ready.goalRevision == 13);
        REQUIRE_FALSE(policy.MarkSubmitted(14));
        REQUIRE(policy.MarkSubmitted(15));
        REQUIRE_FALSE(policy.MarkSubmitted(15));

        now.goalRevision = 14;
        now.tick = 16;
        REQUIRE(policy.Observe(now).HasValue());
        now.tick = 18;
        REQUIRE(policy.Observe(now).HasValue());
        REQUIRE(policy.Decide(18).action == NavigationRepathAction::CoolingDown);
        REQUIRE(policy.Decide(20).action == NavigationRepathAction::None);  // No admission against an unobserved later tick.
        REQUIRE(policy.ForceRepath(now).HasValue());
        const auto forced = policy.Decide(18);
        REQUIRE(forced.action == NavigationRepathAction::Submit);
        REQUIRE(forced.forced);
        REQUIRE(forced.goalRevision == 14);
        REQUIRE(policy.MarkSubmitted(18));
        now.goalRevision = 15;
        now.tick = 19;
        REQUIRE(policy.Observe(now).HasValue());
        now.tick = 23;
        REQUIRE(policy.Observe(now).HasValue());
        REQUIRE(policy.Decide(23).action == NavigationRepathAction::Submit);
    }

    TEST_CASE("Partial corridor tracks missing coverage and cannot be mislabeled complete", "[unit][navigation][path-policy]") {
        const auto source = Source();
        const std::array covered{NavigationCoverageDependency{.region = 41, .generation = Identity<NavigationGeneration>(3)}};
        const std::array missing{NavigationCoverageDependency{.region = 42, .generation = Identity<NavigationGeneration>(3)}};
        const std::array regions{covered.front(), missing.front()};
        const auto partialCoverage = NavigationCoverageEvidence::PartialExact(covered, missing).Value();
        const auto now = Observation(source, regions);
        auto partialPath = Path(source);
        partialPath.status = NavigationPathStatus::Partial;
        NavigationPathPolicy policy({});
        REQUIRE(policy.Install(Id(source.world), partialPath, source, partialCoverage, 9, 10, now).HasValue());
        REQUIRE(policy.CurrentPath(now)->status == NavigationPathStatus::Partial);

        const auto disappeared = Observation(source, covered, 11);
        const auto invalidation = policy.Observe(disappeared);
        REQUIRE(invalidation.HasValue());
        REQUIRE(invalidation.Value().kind == NavigationPathInvalidationKind::Tile);
        REQUIRE(invalidation.Value().dependency == 42);
        REQUIRE(invalidation.Value().observed == 0);
        REQUIRE(policy.CurrentPath(disappeared) == nullptr);

        NavigationPathPolicy invalid({});
        REQUIRE(invalid.Install(Id(source.world), Path(source), source, partialCoverage, 9, 10, now).HasError());
    }

    TEST_CASE("World replacement requires explicit lifecycle reset before another path", "[unit][navigation][path-policy]") {
        const auto source = Source();
        const auto coverage = NavigationCoverageEvidence::CompleteTopology();
        const auto original = Observation(source, {});
        NavigationPathPolicy policy({});
        REQUIRE(policy.Install(Id(source.world), Path(source), source, coverage, 9, 10, original).HasValue());

        auto oldWorldGoal = original;
        oldWorldGoal.goalRevision = 11;
        oldWorldGoal.tick = 10;
        REQUIRE(policy.Observe(oldWorldGoal).HasValue());
        REQUIRE(policy.Decide(10).action == NavigationRepathAction::Submit);

        auto replacementSource = source;
        replacementSource.world = Identity<NavigationWorldId>(12);
        auto replacement = Observation(replacementSource, {}, 11);
        replacement.goalRevision = 11;
        REQUIRE(policy.Observe(replacement).Value().kind == NavigationPathInvalidationKind::World);
        REQUIRE(policy.CurrentPath(replacement) == nullptr);
        REQUIRE(policy.Decide(11).action == NavigationRepathAction::None);
        const auto rejected = policy.ForceRepath(replacement);
        REQUIRE(rejected.HasError());
        REQUIRE(rejected.ErrorValue().code.Value() == NavigationErrors::InvalidWorld.code.Value());
        REQUIRE(policy.Install(Id(replacementSource.world), Path(replacementSource), replacementSource, coverage, 9, 10, replacement)
                    .HasError());
        policy.Clear();
        REQUIRE(policy.Install(Id(replacementSource.world), Path(replacementSource), replacementSource, coverage, 9, 11, replacement)
                    .HasValue());
        REQUIRE(policy.CurrentPath(replacement) != nullptr);
    }

    TEST_CASE("Forced repath cannot cross a world change without a held path", "[unit][navigation][path-policy]") {
        const auto source = Source();
        const auto original = Observation(source, {});
        NavigationPathPolicy policy({});
        REQUIRE(policy.ForceRepath(original).HasValue());
        REQUIRE(policy.Decide(10).action == NavigationRepathAction::Submit);

        auto newSource = source;
        newSource.world = Identity<NavigationWorldId>(12);
        const auto replacement = Observation(newSource, {}, 11);
        REQUIRE(policy.ForceRepath(replacement).HasError());
        REQUIRE(policy.LastInvalidation().kind == NavigationPathInvalidationKind::World);
        REQUIRE(policy.Decide(11).action == NavigationRepathAction::None);
        REQUIRE(policy.CurrentPath(replacement) == nullptr);
        REQUIRE(policy
                    .Install(Id(newSource.world), Path(newSource), newSource, NavigationCoverageEvidence::CompleteTopology(), 9, 10,
                             replacement)
                    .HasError());

        policy.Clear();
        REQUIRE(policy.ForceRepath(replacement).HasValue());
        REQUIRE(policy.Decide(11).action == NavigationRepathAction::Submit);
    }

    TEST_CASE("Same-world revision rollback cannot republish an older target", "[unit][navigation][path-policy]") {
        const auto source = Source();
        auto now = Observation(source, {});
        NavigationPathPolicy policy({});
        REQUIRE(
            policy.Install(Id(source.world), Path(source), source, NavigationCoverageEvidence::CompleteTopology(), 9, 10, now).HasValue());
        now.goalRevision = 12;
        now.tick = 11;
        REQUIRE(policy.Observe(now).HasValue());
        auto rolledBack = now;
        rolledBack.goalRevision = 11;
        rolledBack.tick = 12;
        REQUIRE(policy.Observe(rolledBack).HasError());
        REQUIRE(policy.CurrentPath(rolledBack) == nullptr);
        REQUIRE(policy.Decide(12).action == NavigationRepathAction::None);
        REQUIRE(policy.Decide(11).goalRevision == 12);
    }

    TEST_CASE("Invalid publication and observations cannot replace a current path", "[unit][navigation][path-policy]") {
        const auto source = Source();
        const auto coverage = NavigationCoverageEvidence::CompleteTopology();
        auto now = Observation(source, {});
        NavigationPathPolicy policy({});
        REQUIRE(policy.Install(Id(source.world), Path(source), source, coverage, 9, 10, now).HasValue());
        REQUIRE(policy.Install(Id(source.world), Path(source), source, coverage, 9, 10, now).HasError());
        REQUIRE(policy.Id() == Id(source.world));

        auto stale = now;
        stale.provenance.topology = Identity<NavigationGeneration>(4);
        REQUIRE(policy.Install(Id(source.world, 2), Path(source), source, coverage, 9, 10, stale).HasError());
        REQUIRE(policy.Id() == Id(source.world));
        auto noPath = Path(source);
        noPath.status = NavigationPathStatus::Unreachable;
        REQUIRE(policy.Install(Id(source.world, 2), noPath, source, coverage, 9, 10, now).HasError());
        auto wrongGeneration = Path(source);
        wrongGeneration.sourceGeneration = Identity<NavigationGeneration>(4);
        REQUIRE(policy.Install(Id(source.world, 2), wrongGeneration, source, coverage, 9, 10, now).HasError());
        auto malformed = now;
        malformed.goalRevision = 0;
        REQUIRE(policy.Observe(malformed).HasError());
        const std::array outOfOrder{NavigationCoverageDependency{.region = 2, .generation = source.topology},
                                    NavigationCoverageDependency{.region = 1, .generation = source.topology}};
        malformed = Observation(source, outOfOrder);
        REQUIRE(policy.Observe(malformed).HasError());
        REQUIRE(policy.CurrentPath(now) != nullptr);
        REQUIRE(policy.ForceRepath(malformed).HasError());
        policy.Clear();
        REQUIRE_FALSE(policy.Id().IsValid());
        REQUIRE(policy.CurrentPath(now) == nullptr);
    }
}  // namespace Horo::Navigation
