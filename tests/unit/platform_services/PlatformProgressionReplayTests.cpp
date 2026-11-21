#include "Horo/PlatformServices/PlatformProgressionReplay.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <type_traits>

namespace {
    using namespace Horo;
    using namespace Horo::PlatformServices;

    class ScriptedQualifier final : public IPlatformProgressionReplayQualifier {
    public:
        PlatformProgressionReplaySafety safety{PlatformProgressionReplaySafety::AtomicComparison};
        std::optional<Error> failure;
        std::optional<PlatformProgressionReplayRequirements> current;

        Result<PlatformProgressionQualifiedReplayIntent> Qualify(const PlatformProgressionMutationEnvelope &envelope,
                                                                 const PlatformProgressionReplayRequirements &requirements,
                                                                 LeaderboardOrdering ordering) const override {
            if (failure)
                return Result<PlatformProgressionQualifiedReplayIntent>::Failure(*failure);
            return SealQualifiedIntent(envelope, requirements, safety, ordering);
        }

        Result<void> Revalidate(const PlatformProgressionQualifiedReplayIntent &intent) const override {
            if (failure)
                return Result<void>::Failure(*failure);
            if (!current || *current != intent.Requirements())
                return Result<void>::Failure(MakeError(PlatformProgressionErrors::InvalidMutation));
            return Result<void>::Success();
        }
    };

    PlatformProgressionReplayRequirements Requirements() {
        PlatformProgressionReplayRequirements requirements{.provider = {1}, .providerGeneration = {3}, .policy = {5}};
        requirements.providerQualificationFingerprint.bytes.fill(1);
        requirements.effectiveSemanticsFingerprint.bytes.fill(2);
        requirements.projectPolicyFingerprint.bytes.fill(3);
        requirements.stableIdRegistryFingerprint.bytes.fill(4);
        requirements.definitionRegistryFingerprint.bytes.fill(5);
        requirements.mappingGenerationFingerprint.bytes.fill(6);
        return requirements;
    }

    PlatformProgressionMutationEnvelope Envelope(PlatformProgressionMutationKind kind) {
        PlatformProgressionMutationCandidate candidate;
        candidate.occurrence.bytes.back() = std::byte{1};
        candidate.scope = {.provider = {3}, .session = {4}, .accessPolicy = {5}};
        candidate.scope.subjectPartition.digest.bytes.back() = 6;
        candidate.definition = {7};
        candidate.kind = kind;
        candidate.policy = {5};
        candidate.value = std::int64_t{8};
        using enum PlatformProgressionMutationKind;
        switch (kind) {
            case UnlockOnce:
                candidate.definitionKind = PlatformServiceIdKind::Achievement;
                candidate.value = std::monostate{};
                break;
            case SetProgressMaximum:
                candidate.definitionKind = PlatformServiceIdKind::Achievement;
                candidate.value = std::uint64_t{8};
                break;
            case SetStatMaximum:
            case SetStatMinimum:
            case AddStatOnce:
                candidate.definitionKind = PlatformServiceIdKind::Stat;
                break;
            case SetStatSnapshot:
                candidate.definitionKind = PlatformServiceIdKind::Stat;
                candidate.expectedRevision = 9;
                break;
            case SubmitBestScore:
                candidate.definitionKind = PlatformServiceIdKind::Leaderboard;
                break;
            case ReplaceScoreAtRevision:
                candidate.definitionKind = PlatformServiceIdKind::Leaderboard;
                candidate.expectedRevision = 9;
                break;
        }
        auto result = BuildPlatformProgressionMutationEnvelope(candidate);
        REQUIRE(result.HasValue());
        return std::move(result).Value();
    }
}  // namespace

static_assert(!std::is_default_constructible_v<PlatformProgressionQualifiedReplayIntent>);

TEST_CASE("Replay qualification handoff preserves every original envelope and distinct primitive class",
          "[platform-services][progression][pls004-contract][replay]") {
    using enum PlatformProgressionMutationKind;
    const std::array kinds{UnlockOnce,      SetProgressMaximum, SetStatMaximum,  SetStatMinimum,
                           SetStatSnapshot, AddStatOnce,        SubmitBestScore, ReplaceScoreAtRevision};
    for (const auto kind : kinds) {
        const auto envelope = Envelope(kind);
        const auto requirements = Requirements();
        ScriptedQualifier qualifier;
        if (kind == UnlockOnce)
            qualifier.safety = PlatformProgressionReplaySafety::IntrinsicIdempotence;
        else if (kind == SetStatSnapshot || kind == ReplaceScoreAtRevision)
            qualifier.safety = PlatformProgressionReplaySafety::ConditionalRevision;
        else if (kind == AddStatOnce)
            qualifier.safety = PlatformProgressionReplaySafety::DurableMutationIdDeduplication;
        const auto qualified = qualifier.Qualify(envelope, requirements, LeaderboardOrdering::HighestFirst);
        REQUIRE(qualified.HasValue());
        CHECK(qualified.Value().Envelope() == envelope);
        CHECK(qualified.Value().Requirements() == requirements);
        CHECK(qualified.Value().Safety() == qualifier.safety);
        qualifier.current = requirements;
        REQUIRE(qualifier.Revalidate(qualified.Value()).HasValue());
        qualifier.current->mappingGenerationFingerprint.bytes.back() = 9;
        CHECK(qualifier.Revalidate(qualified.Value()).HasError());
        CHECK(qualified.Value().Envelope() == envelope);
    }
}

TEST_CASE("Replay evidence cannot seal missing provenance mismatched generations or an unsafe additive primitive",
          "[platform-services][progression][pls004-contract][replay]") {
    ScriptedQualifier qualifier;
    const auto envelope = Envelope(PlatformProgressionMutationKind::SetStatMaximum);
    for (auto member : {&PlatformProgressionReplayRequirements::providerQualificationFingerprint,
                        &PlatformProgressionReplayRequirements::effectiveSemanticsFingerprint,
                        &PlatformProgressionReplayRequirements::projectPolicyFingerprint,
                        &PlatformProgressionReplayRequirements::stableIdRegistryFingerprint,
                        &PlatformProgressionReplayRequirements::definitionRegistryFingerprint,
                        &PlatformProgressionReplayRequirements::mappingGenerationFingerprint}) {
        auto incomplete = Requirements();
        incomplete.*member = {};
        CHECK(qualifier.Qualify(envelope, incomplete, LeaderboardOrdering::HighestFirst).HasError());
    }
    auto wrongGeneration = Requirements();
    ++wrongGeneration.providerGeneration.value;
    CHECK(qualifier.Qualify(envelope, wrongGeneration, LeaderboardOrdering::HighestFirst).HasError());
    auto wrongPolicy = Requirements();
    ++wrongPolicy.policy.value;
    CHECK(qualifier.Qualify(envelope, wrongPolicy, LeaderboardOrdering::HighestFirst).HasError());
    CHECK(qualifier.Qualify(Envelope(PlatformProgressionMutationKind::AddStatOnce), Requirements(), LeaderboardOrdering::HighestFirst)
              .HasError());
}
