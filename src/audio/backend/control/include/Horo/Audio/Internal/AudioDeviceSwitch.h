#pragma once

/** @file AudioDeviceSwitch.h
 * @brief Control-owner device migration over the equal-peer backend protocol.
 */

#include "Horo/Audio/Internal/AudioBackend.h"

namespace Horo::Audio::Backend {
    /** @brief Voice execution remains owned by playback; device control never rewrites a voice registry. */
    enum class SwitchPlaybackPolicy : std::uint8_t {
        Preserve,
        Restart
    };
    /** @brief One explicitly ordered migration, including a host-owned detached playback preparation boundary. */
    enum class DeviceSwitchStage : std::uint8_t {
        Idle,
        Quiescing,
        Stopping,
        Closing,
        AwaitDiscovery,
        Opening,
        AwaitPlayback,
        Starting,
        AwaitReady,
        QuiescingReplacement,
        StoppingReplacement,
        ClosingReplacement,
        Terminal
    };

    /** @brief Preserved replacement intent and reserved revisions; device identity is resolved only from post-Close discovery. */
    struct DeviceSwitchTarget final {
        AudioDeviceFormatRequest format;
        std::uint64_t formatRevision{}; /**< Reserved control revision, strictly newer than the previous format. */
        std::uint64_t callbackEpoch{};  /**< Reserved callback generation, strictly newer than the retired reader. */
        AccessMode access{AccessMode::Shared};
        LatencyClass latency{LatencyClass::Balanced};
    };

    /** @brief Complete requested migration; backend kind/runtime owner remain fixed. */
    struct DeviceSwitchRequest final {
        AudioDeviceEpoch previous;
        AudioProcessingFormat previousFormat;
        DeviceSwitchTarget replacement;
        SwitchPlaybackPolicy playback{SwitchPlaybackPolicy::Preserve};
        AudioMonotonicTimestamp deadline;
    };

    /**
     * @brief Retained terminal record; failed/retained outcomes never authorize destruction.
     * @details An activation anchor preserves the callback's Horo sample cursor and monotonic timestamp,
     * not a device-consumption measurement. The host still publishes a fresh clock generation,
     * discontinuity and producer correlation before resuming producer admission.
     */
    struct DeviceSwitchCompletion final {
        OperationId operation;
        AudioDeviceEpoch previous;
        std::optional<AudioDeviceEpoch> replacement; /**< Reserved only after a fresh post-Close catalog was admitted. */
        SwitchPlaybackPolicy playback{SwitchPlaybackPolicy::Preserve};
        bool activated{};
        bool previousDetached{};
        ResourceDisposition resources{ResourceDisposition::Retained};
        std::optional<Error> error;
        std::optional<AudioCallbackEvent> activationAnchor{}; /**< First exact replacement Ready fact, only after successful activation. */
    };

    /**
     * @brief Single-owner migration coordinator; backend pumping and lifecycle event dispatch stay host-owned.
     *
     * One object owns one fixed backend and reserves monotonically increasing transaction IDs. Its terminal
     * record remains unchanged until acknowledgement. No callback, wall clock, backend selection or voice
     * mutation runs here. The host must close producer admission/fade before Begin and retain old epoch
     * state through quiescence and native detachment. Timeout or unproven failure retains the ownership island.
     * Events are forwarded by the host; this object does not steal unrelated lifecycle/voice events.
     */
    class AudioDeviceSwitch final {
    public:
        /** @brief Borrow one backend through the entire coordinator lifetime. @param backend Fixed selected peer. */
        explicit AudioDeviceSwitch(AudioBackend &backend) noexcept;
        AudioDeviceSwitch(const AudioDeviceSwitch &) = delete;
        AudioDeviceSwitch &operator=(const AudioDeviceSwitch &) = delete;

        /**
         * @brief Admit migration from an active epoch after host admission/fade closure.
         * @param request Owned replacement selection/format intent and reserved advancing revisions.
         * @param now Current host monotonic time in the deadline domain.
         * @return Transaction identity or rejection with no backend side effects.
         * @details Preserve requires the old actual format to be one complete preferred/allowed tuple.
         * Preferences remain unchanged; the subsequently opened candidate must still have that exact effective format.
         */
        [[nodiscard]] Result<OperationId> Begin(const DeviceSwitchRequest &request, AudioMonotonicTimestamp now);
        /**
         * @brief Cancel replacement activation and request ordered native retirement, without resuming the old output.
         * @param operation Exact admitted transaction.
         * @return Requested or AlreadyTerminal; a foreign identity changes no state.
         * @details The host keeps pumping native/control progress and retains both render contexts until a Closed
         * terminal. An admitted operation is reconciled before the next cleanup step. A started replacement is
         * quiesced/stopped/closed without CommitRendering. At AwaitDiscovery the host still supplies its exact
         * enumeration receipt, even after cancellation, so discovery resources and slots cannot be abandoned.
         * Expiration or failed cleanup retains ownership; cancellation alone never proves detachment or closure.
         */
        [[nodiscard]] Result<CancelDisposition> Cancel(OperationId operation);
        /**
         * @brief Poll at most one operation and submit at most one next step; never waits on native progress.
         * @param now Host monotonic time, also enforcing the transaction's absolute deadline.
         * @return Success, or typed query/admission/terminal failure retained in Completion.
         */
        [[nodiscard]] Result<void> Pump(AudioMonotonicTimestamp now);
        /**
         * @brief Admit the host's completed enumeration after old native Close, without using an earlier catalog.
         * @param operation Exact live migration transaction.
         * @param discovery Backend enumeration operation newer than the acknowledged Close; it must still own its completion slot.
         * @param now Current host monotonic time in the transaction deadline domain.
         * @return Success after catalog admission and acknowledgement, or a typed rejection/failure.
         * @pre At AwaitDiscovery, the host explicitly begins Enumerate and pumps the selected backend to completion.
         * This method polls and acknowledges only that supplied operation; it does not enumerate or select another backend.
         * Wrong, stale, unfinished or unrelated receipts are rejected without consumption. A real enumeration/admission
         * failure creates the one retained transaction terminal, preserving uncertain native discovery resources.
         */
        [[nodiscard]] Result<void> SupplyDiscovery(OperationId operation, OperationId discovery, AudioMonotonicTimestamp now);
        /**
         * @brief Offer an exact-epoch callback fact while preserving host event ownership.
         * @param event Valid callback fact for this runtime; stale/unrelated facts are ignored.
         * @return True only when the first matching replacement Ready fact was accepted; duplicates cannot replace its anchor.
         */
        [[nodiscard]] bool Observe(const AudioCallbackEvent &event) noexcept;
        /**
         * @brief Supply playback's prepared replacement after old native detachment and new-format admission.
         * @param operation Exact live transaction.
         * @param applied Policy actually prepared by playback; must match requested preserve/restart.
         * @param render Retained Horo render port for the admitted replacement, including fade-in state.
         * @return Success or transactional rejection; host retains context/code until later detachment.
         * @pre Playback has preserved/restored its semantic state or restarted exactly as requested, and
         * rebuilt rate/layout-dependent graph/stream state. Preserve currently requires unchanged format.
         */
        [[nodiscard]] Result<void> SupplyPlayback(OperationId operation, SwitchPlaybackPolicy applied, RenderPort render);
        /** @brief Read detached admitted candidate at the playback preparation boundary. @return Candidate or null. */
        [[nodiscard]] const Opened *Candidate() const noexcept;
        /** @brief Read retained terminal evidence. @return Stable record or null while no terminal exists. */
        [[nodiscard]] const DeviceSwitchCompletion *Completion() const noexcept;
        /** @brief Read current ordered stage. @return Control-owned transaction stage. */
        [[nodiscard]] DeviceSwitchStage Stage() const noexcept;
        /**
         * @brief Release a safely reconciled terminal record, never native resources.
         * @param operation Exact completed transaction; Retained failures cannot be acknowledged for reuse.
         * @return Success or identity/lifecycle rejection.
         */
        [[nodiscard]] Result<void> Acknowledge(OperationId operation);

    private:
        [[nodiscard]] Result<void> Fail(const Error &error, ResourceDisposition resources);
        [[nodiscard]] Result<void> Apply(const Backend::Completion &completion);
        /** @brief Reconcile only quiesce/detach/close outcomes for the previous epoch. */
        [[nodiscard]] Result<void> ApplyRetirement(const Backend::Completion &completion);
        /** @brief Admit preserved intent and actual replacement evidence before playback preparation. */
        [[nodiscard]] Result<void> AdmitCandidate(const Opened &candidate);
        /** @brief Resolve preserved selection and reserve the replacement epoch from one complete fresh catalog. */
        [[nodiscard]] Result<void> AdmitDiscovery(const AudioDeviceSnapshot &devices);
        /** @brief Poll and acknowledge at most one backend terminal, retaining uncertain ownership on failure. */
        [[nodiscard]] Result<void> PollPending();
        /** @brief Commit exactly one ready replacement and retain its transaction terminal. */
        [[nodiscard]] Result<void> ActivateReplacement();
        /** @brief Publish cancellation only after actual native Close, retaining any discovery failure as its cause. */
        [[nodiscard]] Result<void> FinishCancelled();
        /** @brief Redirect detached or started replacement stages into ordered cleanup without activation. */
        void AdvanceCancellation() noexcept;
        [[nodiscard]] Result<void> Submit();
        [[nodiscard]] Result<void> PumpImpl(AudioMonotonicTimestamp now);
        AudioBackend &backend_;
        DeviceSwitchStage stage_{DeviceSwitchStage::Idle};
        std::uint64_t sequence_{};
        std::optional<DeviceSwitchRequest> request_;
        std::optional<Open> replacement_;
        std::optional<AudioDeviceSnapshot> devices_;
        std::optional<OperationId> pending_;
        std::optional<Opened> candidate_;
        std::optional<DeviceSwitchCompletion> completion_;
        RenderPort render_;
        bool detached_{};
        std::optional<AudioCallbackEvent> ready_;
        bool cancelled_{};
        std::optional<Error> cancellationCause_;
        std::uint64_t lastBackendSequence_{};
    };
}  // namespace Horo::Audio::Backend
