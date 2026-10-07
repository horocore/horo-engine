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
         * @param header Immutable publication metadata borrowed only until this synchronous call returns; world and base scene must match
         * the bound world.
         * @param version Trusted supported container version.
         * @param limits Finite writer/reader admission bounds.
         * @return Privately finalized publishable bytes only after production readback and unknown-data roundtrip verification.
         * @details Accepted immutable captures may be encoded on worker threads after live Scene/native retirement.
         *          This method reads no live Scene/installation state and invokes no source callbacks; reader work is atomically bounded.
         */
        [[nodiscard]] Result<FinalizedSaveArchive> ReSave(const SaveArchiveHeader &header, ArchiveFormatVersion version,
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

    /** @brief Inert requested capture metadata; neither construction nor copying admits callbacks or certifies a world.
     * @details The content-owned world and existing barrier validate every field against their actual private authorities.
     */
    struct SaveContentCaptureRequest final {
        RuntimePhase phase;                /**< Current host phase checked by the existing barrier. */
        SaveRuntimeGeneration generation;  /**< Exact session, Scene and registry generations. */
        CapturedStateId capturedState;     /**< Stable requested detached capture identity. */
        CanonicalCaptureEpoch epoch;       /**< Coherent host logical epoch checked by registered authorities. */
        RuntimeSaveCaptureLimits limits{}; /**< Finite coordinated capture bounds. */
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
         * @param request Inert host phase, generation, identity, epoch and limits; all admitted against actual owner/barrier authority.
         * @param participants Actual pinned registry snapshot containing this world’s bound required declaration adapter.
         * @param policy Explicit project re-save policy; degraded denial occurs before any capture callback.
         * @return Real barrier outcome and privately admitted immutable snapshot, or typed moved/stale/revoked/policy error.
         * @throws std::bad_alloc If preparing owned allocation-failure storage cannot complete, before any admission mutation.
         * @details Once failure storage is prepared, allocation faults return CanonicalCodecAllocationFailed without allocating
         *          during unwinding. World state, barrier admission and participant callbacks remain untouched by preparation failure.
         */
        [[nodiscard]] Result<SaveContentCaptureOutcome> CaptureAtSafePoint(SaveCaptureBarrier &barrier,
                                                                           const SaveContentCaptureRequest &request,
                                                                           SaveParticipantRegistrySnapshot participants,
                                                                           SaveDegradedWorldPolicy policy) const;

    private:
        friend class QueuedSavedSceneBootstrap;

        explicit SaveContentWorld(std::shared_ptr<SaveContentDetail::WorldState> state) noexcept : state_(std::move(state)) {}

        std::shared_ptr<SaveContentDetail::WorldState> state_;
    };
}  // namespace Horo::Runtime
