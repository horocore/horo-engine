#pragma once

/** @file AIReplayEvidence.h @brief Bounded comparison of detached, owner-captured AI replay evidence. */

#include "Horo/AI/AICanonicalState.h"
#include "Horo/AI/DecisionStepTrace.h"

#include <array>
#include <optional>
#include <span>

namespace Horo::AI {
    /** @brief Comparison order for observations at their declared fixed-tick boundaries. */
    enum class AiReplayBoundary : std::uint8_t {
        SceneInput,
        PerceptionInput,
        QueryPublication,
        BlackboardSync,
        Decision,
        Navigation,
        Gameplay,
        Physics,
        AchievedCharacterMotion,
        CanonicalAi,
        Count,
    };

    /** @brief Explicitly distinguishes comparable evidence from declared exclusions. */
    enum class AiReplayCoverage : std::uint8_t {
        Omitted,
        Deterministic,
        Nondeterministic
    };

    /** @brief Exact semantic schema and content identity given by the boundary's actual owner. */
    struct AiReplayBoundaryProfile final {
        AiReplayCoverage coverage{AiReplayCoverage::Omitted};
        std::uint32_t schemaVersion{};
        Sha256Digest content;
        bool operator==(const AiReplayBoundaryProfile &) const = default;
    };

    inline constexpr std::size_t AiReplayBoundaryCount = static_cast<std::size_t>(AiReplayBoundary::Count);
    inline constexpr std::size_t MaximumAiReplayTicks = 4096;

    /** @brief Declared comparison coverage, using boundary order as the index.
     * @details Version 1 qualifies at most one selected decision subject. Omitted boundaries carry no metadata;
     * nondeterministic boundaries carry their exact schema/content but publish no comparable tick observations. */
    struct AiReplayManifest final {
        static constexpr std::uint32_t CurrentVersion = 1;
        std::uint32_t version{CurrentVersion};
        std::array<AiReplayBoundaryProfile, AiReplayBoundaryCount> boundaries;
        AgentId decisionSubject;
        bool operator==(const AiReplayManifest &) const = default;
    };

    /** @brief Exact logical publication identity issued by the boundary owner and comparable across recordings. */
    struct AiReplayStamp final {
        std::uint64_t generation{};
        std::uint64_t revision{};
        bool operator==(const AiReplayStamp &) const = default;
    };

    /** @brief Digest of the boundary's canonical input or output, owned by its typed subsystem adapter. */
    struct AiReplayObservation final {
        AiReplayStamp stamp;
        Sha256Digest canonical;
        bool operator==(const AiReplayObservation &) const = default;
    };

    /** @brief Correlates an existing decision trace to the independently captured observation consumed by the kernel. */
    struct AiReplayDecisionObservation final {
        AgentId subject;
        BlackboardInstanceBinding blackboard;
        std::uint64_t blackboardRevision{};
        const DecisionStepTrace *trace{}; /**< Immutable borrowed GAI-003.12 evidence, alive throughout comparison. */
    };

    /** @brief One complete immutable post-tick evidence record for the declared profile. */
    struct AiReplayTick final {
        std::uint64_t tick{};
        std::array<std::optional<AiReplayObservation>, AiReplayBoundaryCount> observations; /**< Decision uses the trace below. */
        std::optional<AiReplayDecisionObservation> decision;
    };

    /** @brief First known difference, with producer identities when both runs observed the differing boundary. */
    struct AiReplayDivergence final {
        std::uint64_t tick{};
        std::optional<AiReplayBoundary> boundary; /**< Absence denotes a missing tick in one recording. */
        std::optional<AiReplayStamp> recorded;
        std::optional<AiReplayStamp> replayed;
    };

    /** @brief Hashes a detached base-agent snapshot through the canonical AI record encoder.
     * @param state Exact immutable canonical AI population from the Scene owner.
     * @param maximumBytes Explicit record budget.
     * @param project Optional persistent identity projection for reference-bearing snapshots.
     * @return Canonical digest, or original shape, identity or capacity failure.
     * @details Intended for qualification/off-owner comparison after capture, before any record storage. */
    [[nodiscard]] Result<Sha256Digest> HashAiReplayCanonicalState(const AiCanonicalState &state, std::size_t maximumBytes,
                                                                  const AiSaveEntityProjector &project = {});

    /** @brief Validates two bounded declared captures before comparing their earliest differing boundary.
     * @param recordedManifest Recorded schema, content and coverage intent.
     * @param recorded Complete contiguous recorded tick window.
     * @param replayedManifest Replayed schema, content and coverage intent.
     * @param replayed Complete contiguous replayed tick window.
     * @return Empty success for equality within declared coverage, first divergence, or typed malformed/incompatible evidence failure.
     * @details Pure comparison, with no simulation callbacks. Runtime task handles are validated against each trace's original owner;
     * replay comparison uses the stable authored identities from that same trace and the exact consumed Blackboard revision. */
    [[nodiscard]] Result<std::optional<AiReplayDivergence>> CompareAiReplayEvidence(const AiReplayManifest &recordedManifest,
                                                                                    std::span<const AiReplayTick> recorded,
                                                                                    const AiReplayManifest &replayedManifest,
                                                                                    std::span<const AiReplayTick> replayed);
}  // namespace Horo::AI
