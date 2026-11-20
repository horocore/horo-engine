#include "Horo/AI/AIReplayEvidence.h"

#include "Horo/AI/AIErrors.h"

#include <algorithm>

namespace Horo::AI {
    namespace {
        using enum AiReplayCoverage;
        using Comparison = Result<std::optional<AiReplayDivergence>>;
        constexpr auto DecisionIndex = static_cast<std::size_t>(AiReplayBoundary::Decision);
        constexpr auto CanonicalIndex = static_cast<std::size_t>(AiReplayBoundary::CanonicalAi);

        /** @brief Validates the closed coverage vocabulary and the known AI/decision contract versions. */
        bool ValidManifest(const AiReplayManifest &manifest) noexcept {
            if (manifest.version != AiReplayManifest::CurrentVersion || std::ranges::none_of(manifest.boundaries, [](const auto &source) {
                return source.coverage == Deterministic;
            }))
                return false;
            const auto &decision = manifest.boundaries[DecisionIndex];
            const auto &canonical = manifest.boundaries[CanonicalIndex];
            if ((decision.coverage == Deterministic) != manifest.decisionSubject.IsValid() ||
                (decision.coverage == Deterministic && decision.schemaVersion != DecisionTraceHeader::CurrentVersion) ||
                (canonical.coverage == Deterministic && canonical.schemaVersion != AiCanonicalRecordVersion &&
                 canonical.schemaVersion != AiCanonicalReferenceRecordVersion))
                return false;
            return std::ranges::all_of(manifest.boundaries, [](const auto &source) {
                if (source.coverage == Omitted)
                    return source.schemaVersion == 0 && source.content == Sha256Digest{};
                return (source.coverage == Deterministic || source.coverage == Nondeterministic) && source.schemaVersion != 0 &&
                       source.content != Sha256Digest{};
            });
        }

        /** @brief Checks original frozen evidence authority and refuses missing or truncated decision observations. */
        bool ValidDecision(const AiReplayManifest &manifest, const AiReplayTick &frame) noexcept {
            const auto &profile = manifest.boundaries[DecisionIndex];
            if (profile.coverage != Deterministic)
                return !frame.decision;
            if (!frame.decision)
                return false;
            const auto &observation = *frame.decision;
            const auto *trace = observation.trace;
            if (observation.subject != manifest.decisionSubject || !observation.blackboard.IsValid() ||
                observation.blackboardRevision == 0 || !trace || trace->DroppedRecords() != 0 || !trace->Header().root.IsValid() ||
                !trace->Header().MatchesObservation(profile.content, frame.tick, observation.blackboard, observation.blackboardRevision))
                return false;
            const auto incarnation = observation.blackboard.agent.incarnation;
            return std::ranges::all_of(trace->Records(), [&](const auto &record) {
                return !record.handle.IsValid() || record.handle.incarnation == incarnation;
            });
        }

        /** @brief Refuses undeclared, missing, sparse or over-budget evidence before comparison. */
        bool ValidTicks(const AiReplayManifest &manifest, const std::span<const AiReplayTick> ticks) noexcept {
            if (ticks.empty() || ticks.size() > MaximumAiReplayTicks || ticks.front().tick == 0)
                return false;
            auto previous = ticks.front().tick - 1;
            for (const auto &frame : ticks) {
                if (frame.tick <= previous || frame.tick - previous != 1 || !ValidDecision(manifest, frame))
                    return false;
                for (std::size_t index = 0; index < frame.observations.size(); ++index) {
                    const auto &observation = frame.observations[index];
                    const bool expected = index != DecisionIndex && manifest.boundaries[index].coverage == Deterministic;
                    if (observation.has_value() != expected ||
                        (observation &&
                         (!observation->stamp.generation || !observation->stamp.revision || observation->canonical == Sha256Digest{})))
                        return false;
                }
                previous = frame.tick;
            }
            return true;
        }

        /** @brief Compares decision semantics using v4 evidence while ignoring process-local handle allocation. */
        bool SameDecision(const AiReplayDecisionObservation &left, const AiReplayDecisionObservation &right) noexcept {
            const auto &a = left.trace->Header();
            const auto &b = right.trace->Header();
            if (a.root != b.root || a.outcome != b.outcome || left.blackboardRevision != right.blackboardRevision ||
                left.blackboard.schema != right.blackboard.schema || left.blackboard.schemaVersion != right.blackboard.schemaVersion ||
                left.blackboard.schemaGeneration != right.blackboard.schemaGeneration ||
                left.blackboard.instanceGeneration != right.blackboard.instanceGeneration)
                return false;
            return std::ranges::equal(left.trace->Records(), right.trace->Records(), [](DecisionStepRecord x, DecisionStepRecord y) {
                if (x.handle.IsValid() != y.handle.IsValid())
                    return false;
                x.handle = {};
                y.handle = {};
                return x == y;
            });
        }

        /** @brief Reports the first differing captured phase within one matching tick. */
        std::optional<AiReplayDivergence> CompareTick(const AiReplayTick &left, const AiReplayTick &right) {
            for (std::size_t index = 0; index < left.observations.size(); ++index) {
                const auto boundary = static_cast<AiReplayBoundary>(index);
                if (index == DecisionIndex && left.decision && !SameDecision(*left.decision, *right.decision))
                    return AiReplayDivergence{left.tick, boundary,
                                              AiReplayStamp{left.decision->blackboard.schemaGeneration, left.decision->blackboardRevision},
                                              AiReplayStamp{right.decision->blackboard.schemaGeneration,
                                                            right.decision->blackboardRevision}};
                if (left.observations[index] && *left.observations[index] != *right.observations[index])
                    return AiReplayDivergence{left.tick, boundary, left.observations[index]->stamp, right.observations[index]->stamp};
            }
            return std::nullopt;
        }
    }  // namespace

    /** @copydoc HashAiReplayCanonicalState */
    Result<Sha256Digest> HashAiReplayCanonicalState(const AiCanonicalState &state, const std::size_t maximumBytes,
                                                    const AiSaveEntityProjector &project) {
        const auto encoded = EncodeAiCanonicalStateRecord(state, maximumBytes, project);
        if (encoded.HasError())
            return Result<Sha256Digest>::Failure(encoded.ErrorValue());
        return Result<Sha256Digest>::Success(ComputeSha256(std::as_bytes(std::span{encoded.Value()})));
    }

    /** @copydoc CompareAiReplayEvidence */
    Result<std::optional<AiReplayDivergence>> CompareAiReplayEvidence(const AiReplayManifest &recordedManifest,
                                                                      const std::span<const AiReplayTick> recorded,
                                                                      const AiReplayManifest &replayedManifest,
                                                                      const std::span<const AiReplayTick> replayed) {
        if (!ValidManifest(recordedManifest) || recordedManifest != replayedManifest)
            return Comparison::Failure(MakeError(AIErrors::CanonicalSchemaUnsupported));
        if (!ValidTicks(recordedManifest, recorded) || !ValidTicks(replayedManifest, replayed))
            return Comparison::Failure(MakeError(AIErrors::CanonicalStateInvalid));
        const auto common = std::min(recorded.size(), replayed.size());
        for (std::size_t index = 0; index < common; ++index) {
            if (recorded[index].tick != replayed[index].tick)
                return Comparison::Success(AiReplayDivergence{std::min(recorded[index].tick, replayed[index].tick)});
            if (const auto difference = CompareTick(recorded[index], replayed[index]))
                return Comparison::Success(difference);
        }
        if (recorded.size() != replayed.size())
            return Comparison::Success(AiReplayDivergence{(recorded.size() > common ? recorded : replayed)[common].tick});
        return Comparison::Success(std::nullopt);
    }
}  // namespace Horo::AI
