#include "Horo/PlatformServices/PlatformProgressionReplay.h"

#include <utility>

namespace Horo::PlatformServices {
    namespace {
        /** @brief Rejects absent provenance; nonzero alone is never proof of qualification authenticity. */
        bool HasFingerprint(const Sha256Digest &fingerprint) noexcept {
            for (const auto byte : fingerprint.bytes) {
                if (byte != 0)
                    return true;
            }
            return false;
        }

        /** @brief Enforces the architecture's primitive classes without claiming a provider actually implements them. */
        bool Matches(const PlatformProgressionMutationKind kind, const PlatformProgressionReplaySafety safety) noexcept {
            using enum PlatformProgressionMutationKind;
            switch (kind) {
                case UnlockOnce:
                    return safety == PlatformProgressionReplaySafety::IntrinsicIdempotence;
                case SetProgressMaximum:
                case SetStatMaximum:
                case SetStatMinimum:
                case SubmitBestScore:
                    return safety == PlatformProgressionReplaySafety::AtomicComparison;
                case SetStatSnapshot:
                case ReplaceScoreAtRevision:
                    return safety == PlatformProgressionReplaySafety::ConditionalRevision;
                case AddStatOnce:
                    return safety == PlatformProgressionReplaySafety::DurableMutationIdDeduplication;
            }
            return false;
        }
    }  // namespace

    /** @copydoc PlatformProgressionReplayRequirements::IsValid */
    bool PlatformProgressionReplayRequirements::IsValid() const noexcept {
        return provider.IsValid() && providerGeneration.IsValid() && policy.IsValid() && HasFingerprint(providerQualificationFingerprint) &&
               HasFingerprint(effectiveSemanticsFingerprint) && HasFingerprint(projectPolicyFingerprint) &&
               HasFingerprint(stableIdRegistryFingerprint) && HasFingerprint(definitionRegistryFingerprint) &&
               HasFingerprint(mappingGenerationFingerprint);
    }

    PlatformProgressionQualifiedReplayIntent::PlatformProgressionQualifiedReplayIntent(PlatformProgressionMutationEnvelope envelope,
                                                                                       PlatformProgressionReplayRequirements requirements,
                                                                                       const PlatformProgressionReplaySafety safety,
                                                                                       const LeaderboardOrdering ordering)
        : envelope_(std::move(envelope)), requirements_(std::move(requirements)), safety_(safety), ordering_(ordering) {}

    /** @copydoc PlatformProgressionQualifiedReplayIntent::Envelope */
    const PlatformProgressionMutationEnvelope &PlatformProgressionQualifiedReplayIntent::Envelope() const noexcept {
        return envelope_;
    }

    /** @copydoc PlatformProgressionQualifiedReplayIntent::Requirements */
    const PlatformProgressionReplayRequirements &PlatformProgressionQualifiedReplayIntent::Requirements() const noexcept {
        return requirements_;
    }

    /** @copydoc PlatformProgressionQualifiedReplayIntent::Safety */
    PlatformProgressionReplaySafety PlatformProgressionQualifiedReplayIntent::Safety() const noexcept {
        return safety_;
    }

    /** @copydoc PlatformProgressionQualifiedReplayIntent::Ordering */
    LeaderboardOrdering PlatformProgressionQualifiedReplayIntent::Ordering() const noexcept {
        return ordering_;
    }

    /** @copydoc IPlatformProgressionReplayQualifier::SealQualifiedIntent */
    Result<PlatformProgressionQualifiedReplayIntent> IPlatformProgressionReplayQualifier::SealQualifiedIntent(
        const PlatformProgressionMutationEnvelope &envelope, const PlatformProgressionReplayRequirements &requirements,
        const PlatformProgressionReplaySafety safety, const LeaderboardOrdering ordering) {
        if (!envelope.IsValid() || !requirements.IsValid() || envelope.scope.provider != requirements.providerGeneration ||
            envelope.policy != requirements.policy || !Matches(envelope.kind, safety) ||
            (ordering != LeaderboardOrdering::HighestFirst && ordering != LeaderboardOrdering::LowestFirst) ||
            (envelope.definitionKind != PlatformServiceIdKind::Leaderboard && ordering != LeaderboardOrdering::HighestFirst))
            return Result<PlatformProgressionQualifiedReplayIntent>::Failure(MakeError(PlatformProgressionErrors::InvalidMutation));
        return Result<PlatformProgressionQualifiedReplayIntent>::Success(
            PlatformProgressionQualifiedReplayIntent(envelope, requirements, safety, ordering));
    }
}  // namespace Horo::PlatformServices
