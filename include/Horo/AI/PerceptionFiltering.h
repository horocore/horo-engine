#pragma once

/**
 * @file PerceptionFiltering.h
 * @brief Typed, revisioned perception admission before sensing and publication.
 */

#include "Horo/AI/PerceptionMemory.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace Horo::AI {
    struct PerceptionTeamTag;
    struct PerceptionCustomAffiliationTag;
    /** @brief Stable gameplay team identity; zero represents no team. */
    using PerceptionTeamId = AiStableIdentity<PerceptionTeamTag>;
    /** @brief Stable authored identity for a custom affiliation. */
    using PerceptionCustomAffiliationId = AiStableIdentity<PerceptionCustomAffiliationTag>;

    /** @brief Relationship supplied by authoritative gameplay affiliation state. */
    enum class PerceptionAffiliation : std::uint8_t {
        Friendly,
        Neutral,
        Hostile,
        Custom,
        Count
    };
    /** @brief Gameplay-owned source visibility, independent of render visibility. */
    enum class GameplayPerceptionVisibility : std::uint8_t {
        Public,
        TeamOnly,
        Hidden,
        Count
    };
    /** @brief Additional team relation required by a listener. */
    enum class PerceptionTeamRule : std::uint8_t {
        Any,
        SameTeam,
        DifferentTeam,
        Count
    };

    /** @brief Typed set of admissible affiliation kinds. */
    struct PerceptionAffiliationSet final {
        std::uint8_t bits{0x0f}; /**< Four low bits correspond to PerceptionAffiliation values. */

        /** @brief Tests one affiliation kind. @param kind Kind to test. @return Whether its bit is set. */
        [[nodiscard]] constexpr bool Contains(PerceptionAffiliation kind) const noexcept {
            const auto index = static_cast<std::uint8_t>(kind);
            return index < static_cast<std::uint8_t>(PerceptionAffiliation::Count) && (bits & (std::uint8_t{1} << index)) != 0;
        }

        /** @brief Tests representation. @return True when only defined bits are set. */
        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return (bits & 0xf0) == 0;
        }

        constexpr bool operator==(const PerceptionAffiliationSet &) const noexcept = default;
    };

    /** @brief One listener's bounded authored filter policy. */
    struct PerceptionFilterPolicy final {
        static constexpr std::size_t MaximumCustomAffiliations = 8;
        PerceptionAffiliationSet affiliations{};              /**< Allowed affiliation kinds. */
        PerceptionTeamRule teamRule{PerceptionTeamRule::Any}; /**< Additional team comparison. */
        std::array<PerceptionCustomAffiliationId, MaximumCustomAffiliations> customAffiliations{};
        std::size_t customAffiliationCount{}; /**< Active prefix; empty means all custom identities. */

        bool operator==(const PerceptionFilterPolicy &) const = default;
    };

    /** @brief Payload-free candidate facts captured from authoritative gameplay state. */
    struct PerceptionFilterFacts final {
        PerceptionMemoryKey key; /**< Exact listener, sense, stimulus, and source identity being checked. */
        PerceptionAffiliation affiliation{PerceptionAffiliation::Neutral};
        PerceptionCustomAffiliationId customAffiliation; /**< Required exactly when affiliation is Custom. */
        PerceptionTeamId listenerTeam;
        PerceptionTeamId sourceTeam;
        std::uint64_t listenerLayers{1};              /**< Non-empty listener layer membership. */
        std::uint64_t listenerMask{~std::uint64_t{}}; /**< Layers the listener accepts. */
        std::uint64_t sourceLayers{1};                /**< Non-empty source layer membership. */
        std::uint64_t sourceMask{~std::uint64_t{}};   /**< Layers the source permits. */
        GameplayPerceptionVisibility visibility{GameplayPerceptionVisibility::Public};
    };

    /** @brief Stable reason for a candidate's admission decision. */
    enum class PerceptionFilterOutcome : std::uint8_t {
        Allowed,
        AffiliationDenied,
        TeamDenied,
        LayerDenied,
        VisibilityDenied,
        StalePolicy,
    };

    /** @brief Payload-free decision bound to exactly one committed policy revision. */
    struct PerceptionFilterDecision final {
        PerceptionFilterOutcome outcome{};
        std::uint64_t policyRevision{};

        /** @brief Tests successful admission. @return True only for Allowed. */
        [[nodiscard]] constexpr bool Allowed() const noexcept {
            return outcome == PerceptionFilterOutcome::Allowed;
        }
    };

    /** @brief Result of one owner-thread safe-point policy publication. */
    struct PerceptionPolicyCommit final {
        std::uint64_t policyRevision{};         /**< Advances once on change; unchanged for a no-op. */
        std::uint64_t simulationTick{};         /**< Committed fixed simulation tick. */
        std::size_t invalidatedMemoryEntries{}; /**< Facts denied by the changed policy. */
        bool changed{};                         /**< Whether the staged policy differed from the active policy. */
    };

    /**
     * @brief Scene-owned gate for observations and future perception publication.
     * @details The simulation owner alone stages and commits policy at a safe point. Sensing jobs may borrow the immutable
     * policy revision and payload-free facts, then submit their completed observation on the owner thread. A stale job
     * cannot publish. Bounded admission facts permit selective revalidation when policy changes; denied facts are
     * removed before publication while unrelated memory is retained. An unchanged policy preserves memory and revision.
     */
    class PerceptionFilteredMemory final {
    public:
        /**
         * @brief Creates one scene/agent filter gate and its bounded memory.
         * @param sceneIncarnation Exact non-zero scene identity.
         * @param agent Exact scene-owned agent handle.
         * @param memoryPolicy Admitted memory policy.
         * @param filterPolicy Initial typed filter policy.
         * @param initialSimulationTick Initial committed simulation tick.
         * @return Gate or typed invalid policy/memory failure.
         */
        [[nodiscard]] static Result<PerceptionFilteredMemory> Create(std::uint64_t sceneIncarnation, AgentHandle agent,
                                                                     PerceptionMemoryPolicy memoryPolicy = {},
                                                                     PerceptionFilterPolicy filterPolicy = {},
                                                                     std::uint64_t initialSimulationTick = 0);

        /**
         * @brief Filters payload-free facts before costly spatial or physics work.
         * @param facts Gameplay truth captured for the candidate.
         * @param expectedRevision Revision captured by the sensing job.
         * @return A payload-free, revisioned decision or invalid-facts failure.
         */
        [[nodiscard]] Result<PerceptionFilterDecision> Evaluate(const PerceptionFilterFacts &facts, std::uint64_t expectedRevision) const;

        /**
         * @brief Rechecks the current policy before admitting one completed observation to memory.
         * @param observation Completed typed observation.
         * @param facts The same captured authoritative filter facts used at candidate admission.
         * @param expectedRevision Revision captured before costly sensing.
         * @param simulationTick Current committed fixed simulation tick.
         * @return A payload-free decision or typed invalid observation/time/facts failure. Denial never stores payload.
         * @pre The facts key must equal the completed observation key.
         */
        [[nodiscard]] Result<PerceptionFilterDecision> Observe(const PerceptionObservation &observation, const PerceptionFilterFacts &facts,
                                                               std::uint64_t expectedRevision, std::uint64_t simulationTick);

        /**
         * @brief Rechecks an existing fact when authoritative team, layer, affiliation, or visibility state changes.
         * @param facts Current typed gameplay facts for the exact remembered stimulus.
         * @param expectedRevision Current policy revision captured by the event owner.
         * @param simulationTick Current committed fixed simulation tick.
         * @return Payload-free decision or typed invalid-facts/time failure. Denial removes only this exact memory fact.
         */
        [[nodiscard]] Result<PerceptionFilterDecision> RecheckFactsAtSafePoint(const PerceptionFilterFacts &facts,
                                                                               std::uint64_t expectedRevision,
                                                                               std::uint64_t simulationTick);

        /**
         * @brief Stages exactly one validated policy replacement on the simulation owner.
         * @param policy New typed policy.
         * @param expectedRevision Current committed revision; rejects stale writers.
         * @return Success or typed invalid/conflicting policy failure without altering active policy.
         */
        [[nodiscard]] Result<void> StagePolicy(PerceptionFilterPolicy policy, std::uint64_t expectedRevision);

        /**
         * @brief Publishes the staged policy once at the PerceptionSensePoll safe point.
         * @param simulationTick Monotonic committed fixed simulation tick.
         * @return One revisioned result or typed missing-stage/time/revision failure; old policy remains on failure.
         * A no-op preserves revision and memory. A changed policy removes only facts it denies before publication.
         */
        [[nodiscard]] Result<PerceptionPolicyCommit> CommitAtSafePoint(std::uint64_t simulationTick);

        /**
         * @brief Copies only admitted, live memory for future blackboard and debug publication.
         * @param simulationTick Current monotonic committed fixed simulation tick.
         * @param liveness Exact-generation scene residency check.
         * @return Bounded value snapshot or typed memory failure.
         */
        [[nodiscard]] Result<PerceptionMemorySnapshot> SnapshotForPublication(std::uint64_t simulationTick,
                                                                              PerceptionSourceLiveness liveness);

        /** @brief Returns the active policy revision. @return Non-zero monotonic revision. */
        [[nodiscard]] std::uint64_t PolicyRevision() const noexcept;

    private:
        PerceptionFilteredMemory(AIPerceptionMemory memory, PerceptionFilterPolicy policy, std::uint64_t sceneIncarnation) noexcept;
        [[nodiscard]] Result<void> RememberFacts(const PerceptionFilterFacts &facts, std::uint64_t simulationTick);
        void EraseFacts(PerceptionMemoryKey key) noexcept;
        AIPerceptionMemory memory_;
        PerceptionFilterPolicy policy_;
        PerceptionFilterPolicy pendingPolicy_;
        std::array<PerceptionFilterFacts, MaximumPerceptionMemoryEntries> admittedFacts_{};
        std::size_t admittedFactCount_{};
        bool hasPendingPolicy_{};
        std::uint64_t policyRevision_{1};
        std::uint64_t sceneIncarnation_{};
    };
}  // namespace Horo::AI
