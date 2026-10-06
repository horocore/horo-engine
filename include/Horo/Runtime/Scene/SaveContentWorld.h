#pragma once

/** @file SaveContentWorld.h
 * @brief Scene-owned content provenance, safe-point capture and retained opaque-data re-save.
 */
#include "Horo/Runtime/Save/SaveArchiveContainerWriter.h"
#include "Horo/Runtime/Save/SaveCaptureBarrier.h"
#include "Horo/Runtime/Scene/SaveContentReconciliation.h"

namespace Horo::Runtime {
    namespace SaveContentDetail {
        struct WorldState;
        struct AcceptedCaptureSeal;
    }  // namespace SaveContentDetail
    class QueuedSavedSceneBootstrap;

    /** @brief Explicit project decision required at every capture of a degraded world. */
    enum class SaveDegradedWorldPolicy : std::uint8_t {
        Reject,
        PreserveOpaque
    };

    /** @brief Privately admitted detached world capture; a bare RuntimeSaveSnapshot cannot assert content provenance. */
    class SaveContentSnapshot final {
    public:
        SaveContentSnapshot(const SaveContentSnapshot &) noexcept = default;
        SaveContentSnapshot &operator=(const SaveContentSnapshot &) noexcept = default;
        SaveContentSnapshot(SaveContentSnapshot &&) noexcept = default;
        SaveContentSnapshot &operator=(SaveContentSnapshot &&) noexcept = default;
        /** @brief Returns actual immutable safe-point capture evidence. @return Owned-lifetime capture borrow. */
        [[nodiscard]] const RuntimeSaveSnapshot &Capture() const noexcept;
        /** @brief Returns immutable source decisions retained by the accepted capture, including after world retirement.
         * @return Borrow valid while this snapshot retains its seal; empty for a moved-from snapshot. Safe on worker threads.
         */
        [[nodiscard]] std::span<const SaveContentDiagnostic> Diagnostics() const noexcept;
        /** @brief Builds a reader-admitted archive, preserving every approved unknown chunk byte, codec and digest.
         * @param header Publication metadata; world and base scene must match the bound world.
         * @param version Trusted supported container version.
         * @param limits Finite writer/reader admission bounds.
         * @return Privately finalized publishable bytes only after production readback and unknown-data roundtrip verification.
         * @details Accepted immutable captures may be encoded on worker threads after live Scene/native retirement.
         *          This method reads no live Scene/installation state and invokes no source callbacks; reader work is atomically bounded.
         */
        [[nodiscard]] Result<FinalizedSaveArchive> ReSave(SaveArchiveHeader header, ArchiveFormatVersion version,
                                                          const SaveArchiveReaderLimits &limits = {}) const;

    private:
        friend class SaveContentWorld;
        SaveContentSnapshot(RuntimeSaveSnapshot capture, std::shared_ptr<const SaveContentDetail::AcceptedCaptureSeal> seal) noexcept;
        RuntimeSaveSnapshot capture_;
        std::shared_ptr<const SaveContentDetail::AcceptedCaptureSeal> seal_;
    };

    /** @brief Bounded barrier progress; only Captured contains a privately admitted content snapshot. */
    struct SaveContentCaptureOutcome final {
        SaveCaptureBarrierSnapshot barrier;
        std::optional<SaveContentSnapshot> capture;
        std::optional<Error> error;
    };

    /** @brief Actual published Scene ownership bound to its source/install proof; no cached borrowed SceneView survives a call. */
    class SaveContentWorld final {
    public:
        SaveContentWorld(const SaveContentWorld &) = delete;
        SaveContentWorld &operator=(const SaveContentWorld &) = delete;
        SaveContentWorld(SaveContentWorld &&) noexcept = default;
        SaveContentWorld &operator=(SaveContentWorld &&) noexcept = default;
        /** @brief Returns immutable reconciliation decisions pinned by this world; observes no live installation state.
         * @return Borrow valid while this world retains its source; empty for a moved-from world. Read on the owner thread.
         */
        [[nodiscard]] std::span<const SaveContentDiagnostic> Diagnostics() const noexcept;
        /** @brief Produces the built-in declaration descriptor and bound adapter for explicit host registration.
         * @param registry Actual host registry at its quiescent owner boundary.
         * @return Registration evidence; failure changes no world state. Host must use the resulting snapshot for capture.
         */
        [[nodiscard]] Result<SaveParticipantRegistration> RegisterRequirements(CanonicalStateParticipantRegistry &registry) const;
        /** @brief Polls the real host capture barrier with actual active Scene/source provenance and explicit degraded-world policy.
         * @param barrier Existing owner-thread quiescence authority with an admitted pending operation.
         * @param phase Current runtime phase; the existing barrier enforces the lifecycle safe point.
         * @param generation Actual session/Scene/registry generation, checked against this published Scene and registry snapshot.
         * @param capturedState Stable requested capture identity.
         * @param epoch Actual coherent host logical epoch, independently checked by the barrier's registered authorities.
         * @param participants Actual pinned registry snapshot containing this world’s bound required declaration adapter.
         * @param policy Explicit project re-save policy; degraded denial occurs before any capture callback.
         * @param limits Finite existing capture bounds.
         * @return Real barrier outcome and privately admitted immutable snapshot, or typed moved/stale/revoked/policy error.
         * @throws std::bad_alloc If preparing owned allocation-failure storage cannot complete, before any admission mutation.
         * @details Once failure storage is prepared, allocation faults return CanonicalCodecAllocationFailed without allocating
         *          during unwinding. World state, barrier admission and participant callbacks remain untouched by preparation failure.
         */
        [[nodiscard]] Result<SaveContentCaptureOutcome> CaptureAtSafePoint(SaveCaptureBarrier &barrier, RuntimePhase phase,
                                                                           SaveRuntimeGeneration generation, CapturedStateId capturedState,
                                                                           CanonicalCaptureEpoch epoch,
                                                                           SaveParticipantRegistrySnapshot participants,
                                                                           SaveDegradedWorldPolicy policy,
                                                                           const RuntimeSaveCaptureLimits &limits = {}) const;

    private:
        friend class QueuedSavedSceneBootstrap;

        explicit SaveContentWorld(std::shared_ptr<SaveContentDetail::WorldState> state) noexcept : state_(std::move(state)) {}

        std::shared_ptr<SaveContentDetail::WorldState> state_;
    };
}  // namespace Horo::Runtime
