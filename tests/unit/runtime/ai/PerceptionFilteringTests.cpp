#include "AiTestSupport.h"
#include "Horo/AI/PerceptionFiltering.h"

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <utility>

namespace Horo::AI {
    namespace {
        using TestSupport::MakeIdentity;

        [[nodiscard]] AgentHandle Agent() {
            return {.incarnation = AiRuntimeIncarnation::Create(77).Value(), .slot = {.index = 1, .generation = 1}};
        }

        [[nodiscard]] PerceptionObservation Observation(const std::uint32_t slot = 1) {
            return {.key = {.listener = MakeIdentity<PerceptionListenerTypeId>(9),
                            .sense = MakeIdentity<SenseTypeId>(1),
                            .stimulus = MakeIdentity<StimulusTypeId>(1),
                            .source = {.sceneIncarnation = 77, .slot = slot, .generation = 1}}};
        }

        [[nodiscard]] bool Alive(void *, const PerceptionSourceRef &) {
            return true;
        }

        [[nodiscard]] PerceptionSourceLiveness Liveness() {
            return {.isAlive = Alive};
        }

        [[nodiscard]] PerceptionFilterFacts Facts(const std::uint32_t slot = 1) {
            return {.key = Observation(slot).key};
        }

        [[nodiscard]] PerceptionFilteredMemory Gate(PerceptionFilterPolicy policy = {}) {
            auto created = PerceptionFilteredMemory::Create(77, Agent(), {}, policy);
            REQUIRE(created.HasValue());
            return std::move(created).Value();
        }

        TEST_CASE("perception filters affiliation and custom identity before observation", "[unit][ai][perception][filter]") {
            PerceptionFilterPolicy policy;
            policy.affiliations.bits = (1U << static_cast<unsigned>(PerceptionAffiliation::Friendly)) |
                                       (1U << static_cast<unsigned>(PerceptionAffiliation::Custom));
            policy.customAffiliations[0] = MakeIdentity<PerceptionCustomAffiliationId>(42);
            policy.customAffiliationCount = 1;
            auto gate = Gate(policy);
            auto facts = Facts();
            CHECK(gate.Evaluate(facts, 1).Value().outcome == PerceptionFilterOutcome::AffiliationDenied);
            CHECK(gate.Observe(Observation(), facts, 1, 0).Value().outcome == PerceptionFilterOutcome::AffiliationDenied);
            facts.affiliation = PerceptionAffiliation::Hostile;
            CHECK(gate.Evaluate(facts, 1).Value().outcome == PerceptionFilterOutcome::AffiliationDenied);
            facts.affiliation = PerceptionAffiliation::Custom;
            facts.customAffiliation = MakeIdentity<PerceptionCustomAffiliationId>(41);
            CHECK(gate.Evaluate(facts, 1).Value().outcome == PerceptionFilterOutcome::AffiliationDenied);
            facts.customAffiliation = MakeIdentity<PerceptionCustomAffiliationId>(42);
            CHECK(gate.Evaluate(facts, 1).Value().Allowed());
            CHECK(gate.Observe(Observation(), facts, 1, 0).Value().Allowed());
            CHECK(gate.SnapshotForPublication(0, Liveness()).Value().count == 1);
            facts.affiliation = PerceptionAffiliation::Friendly;
            facts.customAffiliation = {};
            CHECK(gate.Evaluate(facts, 1).Value().Allowed());
        }

        TEST_CASE("team layer and gameplay visibility denial stays out of the filtered publication snapshot",
                  "[unit][ai][perception][filter]") {
            PerceptionFilterPolicy policy;
            policy.teamRule = PerceptionTeamRule::SameTeam;
            auto gate = Gate(policy);
            auto facts = Facts();
            facts.listenerTeam = MakeIdentity<PerceptionTeamId>(5);
            facts.sourceTeam = MakeIdentity<PerceptionTeamId>(6);
            CHECK(gate.Evaluate(facts, 1).Value().outcome == PerceptionFilterOutcome::TeamDenied);
            CHECK(gate.Observe(Observation(1), facts, 1, 0).Value().outcome == PerceptionFilterOutcome::TeamDenied);
            facts.sourceTeam = facts.listenerTeam;
            facts.sourceLayers = 0b10;
            facts.listenerMask = 0b01;
            CHECK(gate.Evaluate(facts, 1).Value().outcome == PerceptionFilterOutcome::LayerDenied);
            facts.key = Observation(2).key;
            CHECK(gate.Observe(Observation(2), facts, 1, 0).Value().outcome == PerceptionFilterOutcome::LayerDenied);
            facts.listenerMask = 0b10;
            facts.sourceMask = 0b10;
            CHECK(gate.Evaluate(facts, 1).Value().outcome == PerceptionFilterOutcome::LayerDenied);
            facts.sourceMask = 0b01;
            facts.visibility = GameplayPerceptionVisibility::Hidden;
            CHECK(gate.Evaluate(facts, 1).Value().outcome == PerceptionFilterOutcome::VisibilityDenied);
            facts.key = Observation(3).key;
            CHECK(gate.Observe(Observation(3), facts, 1, 0).Value().outcome == PerceptionFilterOutcome::VisibilityDenied);
            facts.visibility = GameplayPerceptionVisibility::TeamOnly;
            facts.key = Observation(4).key;
            CHECK(gate.Observe(Observation(4), facts, 1, 0).Value().Allowed());
            const auto published = gate.SnapshotForPublication(0, Liveness()).Value();
            REQUIRE(published.count == 1);
            CHECK(published.entries[0].key.source.slot == 4);
            facts.sourceTeam = MakeIdentity<PerceptionTeamId>(6);
            CHECK(gate.Evaluate(facts, 1).Value().outcome == PerceptionFilterOutcome::TeamDenied);
        }

        TEST_CASE("team-only gameplay visibility denies unrelated teams independently of listener team rule",
                  "[unit][ai][perception][filter]") {
            auto gate = Gate();
            auto facts = Facts();
            facts.listenerTeam = MakeIdentity<PerceptionTeamId>(5);
            facts.sourceTeam = MakeIdentity<PerceptionTeamId>(6);
            facts.visibility = GameplayPerceptionVisibility::TeamOnly;
            CHECK(gate.Evaluate(facts, 1).Value().outcome == PerceptionFilterOutcome::VisibilityDenied);
            CHECK(gate.Observe(Observation(), facts, 1, 0).Value().outcome == PerceptionFilterOutcome::VisibilityDenied);
            CHECK(gate.SnapshotForPublication(0, Liveness()).Value().count == 0);
        }

        TEST_CASE("policy commit advances once and removes denied memory while a no-op preserves it", "[unit][ai][perception][filter]") {
            auto gate = Gate();
            auto facts = Facts();
            REQUIRE(gate.Observe(Observation(), facts, 1, 5).Value().Allowed());
            REQUIRE(gate.StagePolicy({}, 1).HasValue());
            const auto noChange = gate.CommitAtSafePoint(5);
            REQUIRE(noChange.HasValue());
            CHECK_FALSE(noChange.Value().changed);
            CHECK(noChange.Value().policyRevision == 1);
            CHECK(noChange.Value().invalidatedMemoryEntries == 0);
            CHECK(gate.SnapshotForPublication(5, Liveness()).Value().count == 1);
            auto policy = PerceptionFilterPolicy{};
            policy.affiliations.bits = 1U << static_cast<unsigned>(PerceptionAffiliation::Friendly);
            REQUIRE(gate.StagePolicy(policy, 1).HasValue());
            CHECK(gate.StagePolicy(policy, 1).HasError());
            CHECK(gate.CommitAtSafePoint(4).HasError());
            CHECK(gate.PolicyRevision() == 1);
            CHECK(gate.SnapshotForPublication(5, Liveness()).Value().count == 1);
            const auto commit = gate.CommitAtSafePoint(5);
            REQUIRE(commit.HasValue());
            CHECK(commit.Value().policyRevision == 2);
            CHECK(commit.Value().simulationTick == 5);
            CHECK(commit.Value().invalidatedMemoryEntries == 1);
            CHECK(commit.Value().changed);
            CHECK(gate.CommitAtSafePoint(5).HasError());
            CHECK(gate.StagePolicy(policy, 1).HasError());
            CHECK(gate.Evaluate(facts, 1).Value().outcome == PerceptionFilterOutcome::StalePolicy);
            facts.key = Observation(2).key;
            CHECK(gate.Observe(Observation(2), facts, 1, 5).Value().outcome == PerceptionFilterOutcome::StalePolicy);
            facts.key = Observation(3).key;
            CHECK(gate.Observe(Observation(3), facts, 2, 5).Value().outcome == PerceptionFilterOutcome::AffiliationDenied);
            CHECK(gate.SnapshotForPublication(5, Liveness()).Value().count == 0);
            auto friendly = facts;
            friendly.affiliation = PerceptionAffiliation::Friendly;
            friendly.key = Observation(4).key;
            CHECK(gate.Observe(Observation(4), friendly, 2, 5).Value().Allowed());
            CHECK(gate.SnapshotForPublication(5, Liveness()).Value().count == 1);
        }

        TEST_CASE("changed policy retains unrelated memory and gameplay visibility changes remove exact facts",
                  "[unit][ai][perception][filter]") {
            auto gate = Gate();
            auto neutral = Facts(1);
            auto friendly = Facts(2);
            friendly.affiliation = PerceptionAffiliation::Friendly;
            REQUIRE(gate.Observe(Observation(1), neutral, 1, 5).Value().Allowed());
            REQUIRE(gate.Observe(Observation(2), friendly, 1, 5).Value().Allowed());
            auto sibling = Observation(2);
            sibling.key.listener = MakeIdentity<PerceptionListenerTypeId>(10);
            auto siblingFacts = Facts(2);
            siblingFacts.key = sibling.key;
            REQUIRE(gate.Observe(sibling, siblingFacts, 1, 5).Value().Allowed());
            auto policy = PerceptionFilterPolicy{};
            policy.affiliations.bits = 1U << static_cast<unsigned>(PerceptionAffiliation::Friendly);
            REQUIRE(gate.StagePolicy(policy, 1).HasValue());
            const auto committed = gate.CommitAtSafePoint(6);
            REQUIRE(committed.HasValue());
            CHECK(committed.Value().changed);
            CHECK(committed.Value().policyRevision == 2);
            CHECK(committed.Value().invalidatedMemoryEntries == 2);
            const auto retained = gate.SnapshotForPublication(6, Liveness()).Value();
            REQUIRE(retained.count == 1);
            CHECK(retained.entries[0].key == friendly.key);
            CHECK(retained.entries[0].firstSensedTick == 5);

            auto anotherFriendly = Facts(3);
            anotherFriendly.affiliation = PerceptionAffiliation::Friendly;
            REQUIRE(gate.Observe(Observation(3), anotherFriendly, 2, 6).Value().Allowed());
            friendly.visibility = GameplayPerceptionVisibility::Hidden;
            CHECK(gate.RecheckFactsAtSafePoint(friendly, 2, 6).Value().outcome == PerceptionFilterOutcome::VisibilityDenied);
            const auto published = gate.SnapshotForPublication(6, Liveness()).Value();
            REQUIRE(published.count == 1);
            CHECK(published.entries[0].key == anotherFriendly.key);
        }

        TEST_CASE("admission facts stay bounded across memory eviction and policy replacement", "[unit][ai][perception][filter]") {
            auto gate = Gate();
            for (std::uint32_t slot = 1; slot <= 48; ++slot)
                REQUIRE(gate.Observe(Observation(slot), Facts(slot), 1, slot).Value().Allowed());
            CHECK(gate.SnapshotForPublication(48, Liveness()).Value().count == 16);
            auto policy = PerceptionFilterPolicy{};
            policy.affiliations.bits = 1U << static_cast<unsigned>(PerceptionAffiliation::Friendly);
            REQUIRE(gate.StagePolicy(policy, 1).HasValue());
            const auto committed = gate.CommitAtSafePoint(48);
            REQUIRE(committed.HasValue());
            CHECK(committed.Value().invalidatedMemoryEntries == 16);
            CHECK(gate.SnapshotForPublication(48, Liveness()).Value().count == 0);
        }

        TEST_CASE("perception policy and facts reject malformed typed inputs without publication", "[unit][ai][perception][filter]") {
            auto invalid = PerceptionFilterPolicy{};
            invalid.affiliations.bits = 0xff;
            CHECK(PerceptionFilteredMemory::Create(77, Agent(), {}, invalid).HasError());
            auto gate = Gate();
            CHECK(gate.StagePolicy(invalid, 1).HasError());
            invalid = {};
            invalid.customAffiliationCount = 2;
            invalid.customAffiliations[0] = MakeIdentity<PerceptionCustomAffiliationId>(42);
            invalid.customAffiliations[1] = invalid.customAffiliations[0];
            CHECK(gate.StagePolicy(invalid, 1).HasError());
            auto facts = Facts();
            facts.affiliation = PerceptionAffiliation::Custom;
            CHECK(gate.Evaluate(facts, 1).HasError());
            facts.affiliation = PerceptionAffiliation::Neutral;
            facts.sourceLayers = 0;
            CHECK(gate.Observe(Observation(), facts, 1, 0).HasError());
            facts.sourceLayers = 1;
            facts.visibility = static_cast<GameplayPerceptionVisibility>(255);
            CHECK(gate.Evaluate(facts, 1).HasError());
            CHECK(gate.SnapshotForPublication(0, Liveness()).Value().count == 0);
            facts = Facts();
            CHECK(gate.Observe(Observation(2), facts, 1, 0).HasError());
            facts.key.source.sceneIncarnation = 88;
            CHECK(gate.Evaluate(facts, 1).HasError());
        }
    }  // namespace
}  // namespace Horo::AI
