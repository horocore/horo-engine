#pragma once

/**
 * @file SequencePlaybackRuntime.h
 * @brief Session-owned cinematic playback admission, authority, blending, and restoration contracts.
 */

#include "Horo/Cinematic/SequenceEvaluation.h"

#include <compare>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace Horo::Cinematic {
    /** @brief Maximum restore entries admitted by one playback activation. */
    inline constexpr std::size_t MaximumSequenceRestoreEntries = MaximumFrameEvaluationTracks;
    /** @brief Maximum owner authority claims admitted by one playback activation. */
    inline constexpr std::size_t MaximumSequenceAuthorityClaims = MaximumFrameEvaluationTracks;

    /** @brief Fixed CPU and retained-payload limits selected for one runtime session. */
    struct SequenceEvaluationBudget final {
        SequenceCookTier tier{SequenceCookTier::Compact};
        std::uint32_t maximumActivePlayers{};
        std::uint32_t maximumTracksPerPlayer{};
        std::uint32_t maximumAggregateTracks{};
        std::uint32_t maximumBoundaryOccurrences{};
        std::uint32_t maximumLoopCrossings{};
        std::uint64_t maximumRetainedBytes{};

        [[nodiscard]] constexpr auto operator<=>(const SequenceEvaluationBudget &) const noexcept = default;
    };

    /** @brief Current reservation totals owned by one session service. */
    struct SequenceEvaluationUsage final {
        std::uint32_t activePlayers{};
        std::uint32_t aggregateTracks{};
        std::uint32_t boundaryOccurrences{};
        std::uint64_t retainedBytes{};
        std::uint32_t maximumLoopCrossings{};

        [[nodiscard]] constexpr auto operator<=>(const SequenceEvaluationUsage &) const noexcept = default;
    };

    /**
     * @brief Returns the canonical runtime budget for a provider-neutral cook tier.
     * @param tier Compact, Standard, or Large profile selected by the host.
     * @return Exact budget or a typed malformed-tier failure.
     */
    [[nodiscard]] Result<SequenceEvaluationBudget> GetSequenceEvaluationBudget(SequenceCookTier tier);

    /**
     * @brief Validates one aggregate reservation against a canonical session budget.
     * @param current Existing session reservation.
     * @param additional Candidate reservation to admit atomically.
     * @param budget Canonical host-selected budget.
     * @return Success or a typed malformed/capacity failure; no state is mutated.
     */
    [[nodiscard]] Result<void> AdmitSequenceEvaluationUsage(const SequenceEvaluationUsage &current,
                                                            const SequenceEvaluationUsage &additional,
                                                            const SequenceEvaluationBudget &budget);

    /** @brief Explicit handoff mode for one blend window. */
    enum class SequenceBlendMode : std::uint8_t {
        Cut,
        Blend,
        Count
    };

    /** @brief Policy applied when a player stops or is cancelled. */
    enum class SequenceRestorePolicy : std::uint8_t {
        KeepFinalState,
        RestorePrePlayback,
        Count
    };

    /** @brief One finite enter or exit blend window. */
    struct SequenceBlendWindow final {
        SequenceBlendMode mode{SequenceBlendMode::Cut};
        SequenceTime duration{};

        [[nodiscard]] constexpr auto operator<=>(const SequenceBlendWindow &) const noexcept = default;
    };

    /** @brief Immutable playback handoff and restore policy captured at activation. */
    struct SequencePlaybackBlendSettings final {
        SequenceBlendWindow blendIn{};
        SequenceBlendWindow blendOut{};
        SequenceRestorePolicy restorePolicy{SequenceRestorePolicy::KeepFinalState};

        [[nodiscard]] constexpr auto operator<=>(const SequencePlaybackBlendSettings &) const noexcept = default;
    };

    /**
     * @brief Validates a playback blend and restore policy before activation.
     * @param settings Candidate handoff settings.
     * @return Success or a typed invalid-blend failure.
     */
    [[nodiscard]] Result<void> ValidateSequencePlaybackBlendSettings(const SequencePlaybackBlendSettings &settings);

    /**
     * @brief Computes a bounded linear handoff weight.
     * @param elapsed Non-negative elapsed time in the blend window.
     * @param duration Positive blend duration.
     * @return Weight in [0,1], clamped at the end, or a typed invalid-blend failure.
     */
    [[nodiscard]] Result<float> EvaluateSequenceBlendWeight(SequenceTime elapsed, SequenceTime duration);

    /**
     * @brief Blends one scalar without changing the authored or captured values.
     * @param baseline Pre-cinematic owner value.
     * @param cinematic Sampled cinematic value.
     * @param weight Finite blend weight in [0,1].
     * @return Deterministic scalar result or a typed invalid-blend failure.
     */
    [[nodiscard]] Result<float> BlendSequenceScalar(float baseline, float cinematic, float weight);

    /** @brief Host-facing gameplay pause response selected for a running cinematic clock. */
    enum class SequenceGameplayPauseOutcome : std::uint8_t {
        Resumed,
        HeldByGameplayPause,
        ContinuedDuringGameplayPause,
        Unchanged,
        StaleAuthority,
        Count
    };

    /** @brief Runtime clock, pause-domain, HUD, and scoped gameplay-pause policy captured at activation. */
    struct SequencePlaybackCoordinationSettings final {
        SequenceClockSource clockSource{SequenceClockSource::CommittedSimulation};
        SequencePausePolicy pausePolicy{SequencePausePolicy::FollowGameplay};
        SequenceDilationPolicy dilationPolicy{SequenceDilationPolicy::SourceNative};
        bool pauseGameplay{};
        bool hideHud{};

        [[nodiscard]] constexpr auto operator<=>(const SequencePlaybackCoordinationSettings &) const noexcept = default;
    };

    /**
     * @brief Validates clock and pause-domain combinations before owner admission.
     * @param settings Candidate coordination policy.
     * @return Success or a typed invalid activation failure.
     * @note Committed simulation cannot invent ticks while paused; a player-owned gameplay pause therefore requires an unscaled,
     * player-only presentation domain.
     */
    [[nodiscard]] Result<void> ValidateSequencePlaybackCoordinationSettings(const SequencePlaybackCoordinationSettings &settings);

    /** @brief Revision-fenced observation of the host gameplay pause authority. */
    struct SequenceGameplayPauseRequest final {
        std::uint64_t authorityRevision{};
        bool paused{};
    };

    /** @brief Typed response to one accepted or rejected gameplay-pause observation. */
    struct SequenceGameplayPauseResult final {
        SequenceGameplayPauseOutcome outcome{SequenceGameplayPauseOutcome::Unchanged};
        std::uint64_t authorityRevision{};

        [[nodiscard]] constexpr auto operator<=>(const SequenceGameplayPauseResult &) const noexcept = default;
    };

    /** @brief Kind of host-owned lease acquired by one playback activation. */
    enum class SequenceCoordinationLeaseKind : std::uint8_t {
        GameplayPause,
        HudSuppression,
        Count
    };

    /** @brief Generation- and revision-bound host lease retained only by its owning player. */
    struct SequenceCoordinationLease final {
        SequencePlayerHandle player;
        SequenceCoordinationLeaseKind kind{SequenceCoordinationLeaseKind::GameplayPause};
        std::uint64_t ownerRevision{};

        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return player.IsValid() && kind < SequenceCoordinationLeaseKind::Count && ownerRevision != 0;
        }
    };

    using SequenceCoordinationAcquireFunction = Result<SequenceCoordinationLease> (*)(void *context, const SequencePlayerHandle &player,
                                                                                      SequenceCoordinationLeaseKind kind);
    using SequenceCoordinationReleaseFunction = void (*)(void *context, const SequenceCoordinationLease &lease) noexcept;

    /** @brief Injected host seam for composing pause and HUD leases without a dependency on concrete Runtime/UI systems. */
    struct SequencePlaybackCoordinationHooks final {
        void *context{};
        SequenceCoordinationAcquireFunction acquire{};
        SequenceCoordinationReleaseFunction release{};
    };

    struct SequenceRestoreTargetIdentityTag;
    /** @brief Stable generation-safe identity of one restorable owner target. */
    using SequenceRestoreTargetId = CinematicIdentity<SequenceRestoreTargetIdentityTag>;

    /** @brief One captured pre-playback value and its exact target generation fence. */
    struct SequenceRestoreEntry final {
        TrackId track;
        SequenceRestoreTargetId target;
        std::uint64_t targetRevision{};
        float value{};

        [[nodiscard]] constexpr auto operator<=>(const SequenceRestoreEntry &) const noexcept = default;
    };

    /** @brief One current owner target snapshot supplied at the restore safe point. */
    struct SequenceRestoreTargetSnapshot final {
        SequenceRestoreTargetId target;
        std::uint64_t targetRevision{};
        void *context{};
        bool (*apply)(void *context, float value) noexcept {};
    };

    /** @brief Typed result for one restore entry. */
    enum class SequenceRestoreOutcome : std::uint8_t {
        Restored,
        TargetMissing,
        StaleGeneration,
        WriteRejected,
        Count
    };

    /** @brief Caller-owned diagnostic proving why one captured value was not restored. */
    struct SequenceRestoreDiagnostic final {
        TrackId track;
        SequenceRestoreTargetId target;
        SequenceRestoreOutcome outcome{SequenceRestoreOutcome::TargetMissing};

        [[nodiscard]] constexpr auto operator<=>(const SequenceRestoreDiagnostic &) const noexcept = default;
    };

    /** @brief Aggregate result of one safe-point restore attempt. */
    struct SequenceRestoreResult final {
        std::size_t restored{};
        std::size_t missing{};
        std::size_t stale{};
        std::size_t rejected{};

        [[nodiscard]] constexpr auto operator<=>(const SequenceRestoreResult &) const noexcept = default;
    };

    /**
     * @brief Immutable activation-owned pre-playback snapshot.
     * @note Creation may allocate and canonicalizes entries; application never allocates.
     */
    class SequenceRestoreSnapshot final {
    public:
        /**
         * @brief Validates and owns one bounded snapshot.
         * @param entries Captured values with unique stable targets.
         * @return Snapshot or a typed malformed/capacity failure.
         */
        [[nodiscard]] static Result<SequenceRestoreSnapshot> Create(std::span<const SequenceRestoreEntry> entries);

        /** @brief Returns immutable canonical entries. @return Borrowed snapshot storage. */
        [[nodiscard]] std::span<const SequenceRestoreEntry> Entries() const noexcept;
        /** @brief Returns the captured entry count. @return Number of restorable targets. */
        [[nodiscard]] std::size_t Size() const noexcept;

    private:
        explicit SequenceRestoreSnapshot(std::vector<SequenceRestoreEntry> entries) noexcept;

        std::vector<SequenceRestoreEntry> entries_;
    };

    /**
     * @brief Restores a snapshot against the owner’s current generation-fenced targets.
     * @param snapshot Activation-owned pre-playback values.
     * @param targets Current owner-safe-point target views; order need not match snapshot entries.
     * @param diagnostics Caller storage with at least snapshot.Size() entries.
     * @return Per-entry typed outcomes; destroyed or replaced targets are never dereferenced.
     * @note Valid targets may restore when another target was destroyed; no allocation occurs.
     */
    [[nodiscard]] Result<SequenceRestoreResult> ApplySequenceRestoreSnapshot(const SequenceRestoreSnapshot &snapshot,
                                                                             std::span<const SequenceRestoreTargetSnapshot> targets,
                                                                             std::span<SequenceRestoreDiagnostic> diagnostics);

    struct SequenceAuthorityTargetIdentityTag;
    /** @brief Stable actor/property identity used by owner-side cinematic authority arbitration. */
    using SequenceAuthorityTargetId = CinematicIdentity<SequenceAuthorityTargetIdentityTag>;

    /** @brief Domain channel a cinematic may claim; each channel is independently arbitrated. */
    enum class CinematicControlChannel : std::uint8_t {
        SkeletalPose,
        AnimationParameter,
        CharacterTranslation,
        CharacterHeading,
        CharacterStance,
        CharacterJump,
        CharacterTeleport,
        GameplayAction,
        Count
    };

    /** @brief Owner-side arbitration mode for one cinematic claim. */
    enum class CinematicClaimMode : std::uint8_t {
        ObserveOnly,
        Exclusive,
        Blend,
        PresentationOverlay,
        Count
    };

    /** @brief Generation and ordering evidence for one admitted cinematic owner claim. */
    struct SequenceAuthorityClaim final {
        SequencePlayerHandle player;
        SequenceAuthorityTargetId target;
        CinematicControlChannel channel{CinematicControlChannel::SkeletalPose};
        CinematicClaimMode mode{CinematicClaimMode::ObserveOnly};
        std::int32_t priority{};
        std::uint64_t authorityRevision{};
        std::uint64_t eligibleSimulationTick{};
        bool required{};

        [[nodiscard]] constexpr auto operator<=>(const SequenceAuthorityClaim &) const noexcept = default;
    };

    /** @brief Ordinary gameplay write presented to the owner’s exact authority snapshot. */
    struct SequenceGameplayWriteRequest final {
        SequenceAuthorityTargetId target;
        CinematicControlChannel channel{CinematicControlChannel::SkeletalPose};
        std::uint64_t authorityRevision{};
        std::uint64_t simulationTick{};

        [[nodiscard]] constexpr auto operator<=>(const SequenceGameplayWriteRequest &) const noexcept = default;
    };

    /** @brief Typed result of gameplay versus cinematic authority arbitration. */
    enum class SequenceGameplayWriteOutcome : std::uint8_t {
        AcceptedGameplay,
        SuppressedByCinematic,
        AcceptedForOwnerBlend,
        StaleAuthority,
        AuthorityDenied,
        UnsupportedAuthorityMode,
        Count
    };

    /** @brief Immutable owner decision including the deterministic winning player when applicable. */
    struct SequenceGameplayWriteResult final {
        SequenceGameplayWriteOutcome outcome{SequenceGameplayWriteOutcome::AcceptedGameplay};
        std::optional<SequencePlayerHandle> owner;

        [[nodiscard]] constexpr auto operator<=>(const SequenceGameplayWriteResult &) const noexcept = default;
    };

    /**
     * @brief Activation-built deterministic authority claim set.
     * @note Claims are sorted by target/channel, descending priority, then stable player identity.
     */
    class SequenceAuthorityPlan final {
    public:
        /**
         * @brief Validates and canonicalizes owner claims.
         * @param authorityRevision Non-zero authority publication revision.
         * @param eligibleSimulationTick Non-zero tick boundary for the claims.
         * @param claims Required/optional typed claims.
         * @return Immutable plan or a typed malformed/conflict/capacity failure.
         */
        [[nodiscard]] static Result<SequenceAuthorityPlan> Create(std::uint64_t authorityRevision, std::uint64_t eligibleSimulationTick,
                                                                  std::span<const SequenceAuthorityClaim> claims);

        /**
         * @brief Resolves one gameplay write without mutating the plan.
         * @param request Exact target/channel and owner snapshot evidence.
         * @return Typed acceptance, suppression, blend, or stale-authority result.
         */
        [[nodiscard]] Result<SequenceGameplayWriteResult> ResolveGameplayWrite(const SequenceGameplayWriteRequest &request) const;

        /** @brief Returns the immutable claim set. @return Borrowed canonical claims. */
        [[nodiscard]] std::span<const SequenceAuthorityClaim> Claims() const noexcept;
        /** @brief Returns the exact authority publication revision. @return Non-zero revision. */
        [[nodiscard]] std::uint64_t AuthorityRevision() const noexcept;
        /** @brief Returns the exact eligible fixed-tick boundary. @return Non-zero tick. */
        [[nodiscard]] std::uint64_t EligibleSimulationTick() const noexcept;

    private:
        SequenceAuthorityPlan(std::uint64_t authorityRevision, std::uint64_t eligibleSimulationTick,
                              std::vector<SequenceAuthorityClaim> claims) noexcept;

        std::uint64_t authorityRevision_{};
        std::uint64_t eligibleSimulationTick_{};
        std::vector<SequenceAuthorityClaim> claims_;
    };

    /** @brief Host-selected session composition for the live cinematic registry. */
    struct CinematicRuntimeServiceConfig final {
        CinematicRuntimeSessionId session;
        SequenceCookTier tier{SequenceCookTier::Standard};
    };

    /** @brief Complete immutable activation transaction admitted into one runtime session. */
    struct SequencePlaybackActivation final {
        SequencePlayerDescriptor player;
        SequenceFrameEvaluationPlan plan;
        SequencePlaybackBlendSettings blend{};
        std::optional<SequenceAuthorityPlan> authority;
        std::optional<SequenceRestoreSnapshot> restore;
        std::uint64_t retainedBytes{};
        SequencePlaybackCoordinationSettings coordination{};
        SequencePlaybackCoordinationHooks coordinationHooks{};
    };

    /** @brief Observable owner-coordination state retained by one live player. */
    struct SequencePlaybackCoordinationSnapshot final {
        SequencePlaybackCoordinationSettings settings;
        std::uint64_t gameplayPauseRevision{};
        bool gameplayPaused{};
        bool gameplayPauseLeaseOwned{};
        bool hudSuppressionLeaseOwned{};

        [[nodiscard]] constexpr auto operator<=>(const SequencePlaybackCoordinationSnapshot &) const noexcept = default;
    };

    /** @brief Observable service state and reservation evidence at one owner boundary. */
    struct CinematicRuntimeServiceSnapshot final {
        bool admissionOpen{};
        SequenceEvaluationBudget budget;
        SequenceEvaluationUsage usage;

        [[nodiscard]] constexpr auto operator<=>(const CinematicRuntimeServiceSnapshot &) const noexcept = default;
    };

    /**
     * @brief Session-owned registry that combines player lifecycle with the deterministic frame core.
     *
     * The service owns live players, cursors, plans, authority claims, restore snapshots, and
     * aggregate reservations. Scene components and callers retain only handles. Activation may
     * allocate; successful command, evaluation, cancellation, restore, and release paths do not
     * grow storage.
     */
    class CinematicRuntimeService final {
    public:
        /**
         * @brief Creates a fixed-capacity runtime service.
         * @param config Session identity and canonical evaluation tier.
         * @return Prepared service or a typed invalid-session/tier failure.
         */
        [[nodiscard]] static Result<CinematicRuntimeService> Create(const CinematicRuntimeServiceConfig &config);

        CinematicRuntimeService(const CinematicRuntimeService &) = delete;
        CinematicRuntimeService &operator=(const CinematicRuntimeService &) = delete;
        CinematicRuntimeService(CinematicRuntimeService &&other) noexcept;
        CinematicRuntimeService &operator=(CinematicRuntimeService &&other) noexcept;
        ~CinematicRuntimeService() noexcept;

        /**
         * @brief Atomically admits one prepared player and its bounded runtime resources.
         * @param activation Complete plan, policy, and owner claim transaction.
         * @return Owner-issued handle or a typed duplicate/capacity/authority/restore failure.
         */
        [[nodiscard]] Result<SequencePlayerHandle> Activate(SequencePlaybackActivation activation);

        /** @brief Reads one exact player snapshot. @param handle Generation-checked handle. @return Snapshot or typed handle failure. */
        [[nodiscard]] Result<SequencePlayerSnapshot> Snapshot(const SequencePlayerHandle &handle) const;
        /** @brief Starts or resumes one player and synchronizes its event cursor. @param handle Exact handle. @return Transition. */
        [[nodiscard]] Result<SequencePlayerTransition> Play(const SequencePlayerHandle &handle);
        /** @brief Pauses one player at the command boundary. @param handle Exact handle. @return Transition. */
        [[nodiscard]] Result<SequencePlayerTransition> Pause(const SequencePlayerHandle &handle);
        /** @brief Requests orderly stop and occurrence draining. @param handle Exact handle. @return Transition. */
        [[nodiscard]] Result<SequencePlayerTransition> Stop(const SequencePlayerHandle &handle);
        /** @brief Publishes the terminal state after admitted stop work retires. @param handle Exact handle. @return Transition. */
        [[nodiscard]] Result<SequencePlayerTransition> FinishStop(const SequencePlayerHandle &handle);
        /** @brief Performs silent random-access seek and resets event traversal. @param handle Exact handle. @param target Direct target.
         * @return Transition. */
        [[nodiscard]] Result<SequencePlayerTransition> Seek(const SequencePlayerHandle &handle, SequenceTime target);
        /** @brief Changes exact playback rate and preserves pause state. @param handle Exact handle. @param rate Bounded rational. @return
         * Transition. */
        [[nodiscard]] Result<SequencePlayerTransition> SetPlaybackSpeed(const SequencePlayerHandle &handle, SequencePlaybackRate rate);
        /** @brief Requests cancellation and completes the closing transition at this owner boundary. @param handle Exact handle. @return
         * Success or typed handle/lifecycle failure. */
        [[nodiscard]] Result<void> Cancel(const SequencePlayerHandle &handle);
        /** @brief Publishes a terminal failure for one non-terminal player. @param handle Exact handle. @return Transition. */
        [[nodiscard]] Result<SequencePlayerTransition> Fail(const SequencePlayerHandle &handle);

        /**
         * @brief Evaluates one committed/service boundary and publishes the resulting player position.
         * @param handle Exact player handle.
         * @param sourceDelta Non-negative source-clock delta.
         * @param scratch Caller-owned bounded values/events/camera storage.
         * @param hooks Typed destination seams; required only for crossed occurrences.
         * @return Atomic frame result; Once players terminalize after reaching their end.
         */
        [[nodiscard]] Result<SequenceFrameEvaluationResult> Evaluate(const SequencePlayerHandle &handle, SequenceTime sourceDelta,
                                                                     const SequenceFrameScratch &scratch, const SequenceFrameHooks &hooks);

        /**
         * @brief Applies one newer host gameplay-pause observation to the active player.
         * @param handle Exact player handle.
         * @param request Monotonic host authority revision and pause state.
         * @return Typed hold/continue/resume outcome or a handle/activation failure.
         */
        [[nodiscard]] Result<SequenceGameplayPauseResult> ResolveGameplayPause(const SequencePlayerHandle &handle,
                                                                               const SequenceGameplayPauseRequest &request);

        /**
         * @brief Reads the captured coordination policy and current host-pause evidence.
         * @param handle Exact player handle.
         * @return Immutable coordination snapshot or a typed handle failure.
         */
        [[nodiscard]] Result<SequencePlaybackCoordinationSnapshot> CoordinationSnapshot(const SequencePlayerHandle &handle) const;

        /**
         * @brief Canonically orders a caller-captured active-player batch before same-boundary evaluation.
         * @param unordered Active handles and declared priorities in arrival/container order.
         * @param ordered Caller storage with at least unordered.size() entries.
         * @return Ordered entry count or a typed ownership, identity, duplicate, or capacity failure.
         */
        [[nodiscard]] Result<std::size_t> OrderActivePlayers(std::span<const SequenceFramePlayerOrder> unordered,
                                                             std::span<SequenceFramePlayerOrder> ordered) const;

        /**
         * @brief Resolves an ordinary gameplay write through the player’s immutable authority plan.
         * @param handle Exact player handle.
         * @param request Target/channel and tick evidence.
         * @return Typed owner decision or a handle failure.
         */
        [[nodiscard]] Result<SequenceGameplayWriteResult> ResolveGameplayWrite(const SequencePlayerHandle &handle,
                                                                               const SequenceGameplayWriteRequest &request) const;

        /**
         * @brief Restores pre-playback values at a terminal owner safe point.
         * @param handle Exact terminal player handle.
         * @param targets Current generation-fenced targets in snapshot order.
         * @param diagnostics Caller storage for per-target outcomes.
         * @return Aggregate restore result or a typed lifecycle/capacity failure.
         */
        [[nodiscard]] Result<SequenceRestoreResult> Restore(const SequencePlayerHandle &handle,
                                                            std::span<const SequenceRestoreTargetSnapshot> targets,
                                                            std::span<SequenceRestoreDiagnostic> diagnostics);

        /**
         * @brief Releases one terminal player and its reservations for generation-safe reuse.
         * @param handle Exact terminal handle.
         * @return Success or a typed non-terminal/restore-required/handle failure.
         */
        [[nodiscard]] Result<void> Release(const SequencePlayerHandle &handle);

        /** @brief Closes admission and cancels all non-terminal players idempotently. @return Success or an owner failure. */
        [[nodiscard]] Result<void> BeginShutdown();
        /** @brief Returns current immutable service evidence. @return Snapshot value. */
        [[nodiscard]] CinematicRuntimeServiceSnapshot ServiceSnapshot() const noexcept;
        /** @brief Returns the exact active-player count. @return Number of unreleased players. */
        [[nodiscard]] std::size_t ActivePlayerCount() const noexcept;

    private:
        struct Instance final {
            SequencePlayer player;
            SequenceFrameEvaluationPlan plan;
            SequenceFrameCursor cursor;
            SequencePlaybackBlendSettings blend;
            std::optional<SequenceAuthorityPlan> authority;
            std::optional<SequenceRestoreSnapshot> restore;
            SequencePlaybackCoordinationSettings coordination;
            SequencePlaybackCoordinationHooks coordinationHooks;
            std::optional<SequenceCoordinationLease> gameplayPauseLease;
            std::optional<SequenceCoordinationLease> hudSuppressionLease;
            std::uint64_t retainedBytes{};
            std::uint64_t gameplayPauseRevision{};
            bool gameplayPaused{};
            bool resumeBaselinePending{};
            bool suppressNextPlayBoundary{};
            bool restoreApplied{};

            Instance(SequencePlayer playerValue, SequenceFrameCursor cursorValue, SequencePlaybackActivation activationValue,
                     std::optional<SequenceCoordinationLease> gameplayPauseLeaseValue,
                     std::optional<SequenceCoordinationLease> hudSuppressionLeaseValue) noexcept;
        };

        struct Slot final {
            std::optional<Instance> instance;
            SequencePlayerHandle retiredHandle;
            bool hasRetiredHandle{};
        };

        explicit CinematicRuntimeService(CinematicRuntimeSessionId session, SequenceEvaluationBudget budget,
                                         std::vector<Slot> slots) noexcept;

        [[nodiscard]] Result<std::size_t> ResolveSlot(const SequencePlayerHandle &handle) const;
        [[nodiscard]] Result<void> SynchronizeCursor(Instance &instance, SequenceCursorResetPolicy resetPolicy);
        [[nodiscard]] Result<void> RebindCursorFence(Instance &instance);
        [[nodiscard]] Result<void> ValidateActivation(const SequencePlaybackActivation &activation) const;
        [[nodiscard]] Result<void> AdmitActivation(const SequencePlaybackActivation &activation, SequenceEvaluationUsage &additional,
                                                   std::size_t &slotIndex) const;
        [[nodiscard]] Result<void> AcquireCoordinationLeases(const SequencePlaybackActivation &activation,
                                                             std::optional<SequenceCoordinationLease> &gameplayPauseLease,
                                                             std::optional<SequenceCoordinationLease> &hudSuppressionLease) const;
        void ReleaseCoordination(Instance &instance) noexcept;
        void RecalculateMaximumLoopCrossings() noexcept;

        CinematicRuntimeSessionId session_;
        SequenceEvaluationBudget budget_;
        SequenceEvaluationUsage usage_;
        bool admissionOpen_{true};
        std::vector<Slot> slots_;
    };
}  // namespace Horo::Cinematic
