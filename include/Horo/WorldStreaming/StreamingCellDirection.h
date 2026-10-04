#pragma once

/**
 * @file StreamingCellDirection.h
 * @brief Revisioned demand reversal and owned asynchronous retirement for one cell operation.
 */

#include "Horo/WorldStreaming/StreamingCellActivation.h"
#include "Horo/WorldStreaming/StreamingDesiredState.h"

#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace Horo::WorldStreaming {
    namespace Detail {
        /** @brief Distinct identity for an authority-issued effective-demand revision. */
        struct StreamingCellDemandRevisionTag;
    }  // namespace Detail

    /** @brief Non-zero effective-demand revision; the authority never wraps or reuses revisions. */
    using StreamingCellDemandRevision =
        Foundation::Detail::NonZeroId64<Detail::StreamingCellDemandRevisionTag, WorldStreamingErrors::IdentityInvalid>;

    /** @brief Exact evidence that one started participant no longer retains attempt resources or readers. */
    struct StreamingCellRetirementAcknowledgement final {
        StreamingCellActivationRequirement participant;
        StreamingCellOperationHandle operation;
    };

    /**
     * @brief Owned retirement adapter for one asset, Scene, provider, job, or lease participant.
     * @details This is a receipt/controller for an already composed attempt, not another feature-provider hierarchy.
     *          Identity and revision are immutable. All calls run on StreamingAuthorityRole and must be bounded and non-blocking;
     *          adapters enqueue native-affinity work. Domain owners retain in-flight resources until real retirement, including on
     *          adapter destruction. The host supplies the validated dependency order and keeps domain services alive through drain.
     */
    class IStreamingCellRetirementParticipant {
    public:
        /** @brief Releases the adapter; domain owners still retain any in-flight work until acknowledged retirement. */
        virtual ~IStreamingCellRetirementParticipant() = default;
        /** @brief Returns immutable participant identity/revision. @return Exact attempt binding. */
        [[nodiscard]] virtual StreamingCellActivationRequirement Requirement() const noexcept = 0;
        /** @brief Returns immutable attempt fence. @return Exact operation identity. */
        [[nodiscard]] virtual StreamingCellOperationHandle Operation() const noexcept = 0;
        /** @brief Revokes new access and publication immediately; does not claim cleanup. */
        virtual void RevokeAccess() noexcept = 0;
        /** @brief Starts idempotent asynchronous cleanup once its dependency predecessors have retired. */
        virtual void BeginRetirement() noexcept = 0;
        /**
         * @brief Polls actual resource retirement without waiting.
         * @return Empty while pending, exact acknowledgement after all jobs/readers/resources retire, or a preserved domain error.
         * @details An error is diagnostic only; it never proves retirement. A later poll may retry observation.
         */
        [[nodiscard]] virtual Result<std::optional<StreamingCellRetirementAcknowledgement>> PollRetirement() = 0;
    };

    /** @brief Bounded, exact construction facts for one admitted direction owner. */
    struct StreamingCellDirectionConfig final {
        StreamingCellOperation operation; /**< Queued operation; normal target cannot change within this identity. */
        StreamingCellDemandRevision revision;
        StreamingDesiredResidency desired{};
        std::size_t maximumParticipants{}; /**< Mandatory positive ceiling, at most 1024. */
        std::uint64_t capacityUnits{};     /**< Required scheduler charge. */
    };

    /**
     * @brief Unique owner that fences publication and drains every participant on demand reversal.
     * @details The borrowed scheduler must outlive this owner and must not be mutated for this owner's reservation by other callers.
     *          The host must drain interrupted work or transfer this owner before destruction; dropping it is not an acknowledgement.
     *          Successful participant ownership must transfer to the resident/active owner. No new residency authority is introduced.
     */
    class StreamingCellDirectionOwner final {
    public:
        StreamingCellDirectionOwner(const StreamingCellDirectionOwner &) = delete;
        StreamingCellDirectionOwner &operator=(const StreamingCellDirectionOwner &) = delete;
        /** @brief Transfers the unique reservation and participant lifetime. @param other Owner to transfer. */
        StreamingCellDirectionOwner(StreamingCellDirectionOwner &&other) noexcept;
        StreamingCellDirectionOwner &operator=(StreamingCellDirectionOwner &&) = delete;

        /**
         * @brief Validates the complete participant set before atomically admitting the operation.
         * @param scheduler Authority-owned scheduler borrowed through terminal release.
         * @param config Exact queued operation, initial demand revision and mandatory limits.
         * @param participants Complete set in validated retirement dependency order; consumed only on success.
         * @pre Controllers are composed before admission. Domain work starts only after successful admission and its matching forward
         *      stage boundary; a controller with no started work acknowledges retirement without inventing resources.
         * @return Unique admitted owner or typed invalid, unsupported, stale, conflict, capacity or lifecycle failure.
         * @post Failure preserves participant ownership and scheduler charges.
         */
        [[nodiscard]] static Result<StreamingCellDirectionOwner> Create(
            StreamingSchedulerAdmissionLedger &scheduler, const StreamingCellDirectionConfig &config,
            std::vector<std::unique_ptr<IStreamingCellRetirementParticipant>> &participants);

        /**
         * @brief Applies a strictly newer effective demand at an owner stage boundary.
         * @param expected Exact operation/fence. @param revision Exact currently retained demand revision.
         * @param successor Strictly greater authority-issued revision. @param desired Supported new effective residency.
         * @return Success or typed stale, invalid, unsupported or lifecycle failure without mutation.
         * @details Load-to-Unloaded and Activate-to-Loaded/Unloaded revoke publication and enter retirement. Returning demand while
         *          Retiring records intent only: cleanup never reverses and a fresh generation must be admitted after acknowledgement.
         */
        [[nodiscard]] Result<void> UpdateDemand(const StreamingCellOperationHandle &expected, StreamingCellDemandRevision revision,
                                                StreamingCellDemandRevision successor, StreamingDesiredResidency desired);
        /**
         * @brief Applies forward progress from an exact current operation snapshot.
         * @param expected Exact current handle, phase, kind and outcome. @param transition BeginPreparation, BeginActivation,
         *          BeginRetirement or Load Complete; interruption and retirement acknowledgement use the dedicated methods.
         * @return Canonical successor or typed stale/unsupported/transition failure. Late completions cannot publish after reversal.
         */
        [[nodiscard]] Result<StreamingCellOperation> Advance(const StreamingCellOperation &expected,
                                                             StreamingCellOperationTransition transition);
        /**
         * @brief Publishes an activation using the current canonical snapshot, then completes the operation once.
         * @param transaction Complete prepared activation receipt set. @param point Current host safe point.
         * @return Success or the preserved activation/scheduler error. Interrupted attempts roll back prepared receipts.
         */
        [[nodiscard]] Result<void> CommitActivation(StreamingCellActivationTransaction &transaction,
                                                    StreamingCellActivationCommitPoint point);
        /**
         * @brief Closes further demand updates and requests cancellation, failure, replacement or shutdown.
         * @param expected Exact operation/fence. @param reason Cancel, Fail, Replace or Shutdown.
         * @return Success or typed invalid/stale/unsupported failure. Repeats retain the first retirement disposition.
         */
        [[nodiscard]] Result<void> Interrupt(const StreamingCellOperationHandle &expected, StreamingCellOperationTransition reason);
        /**
         * @brief Polls retirement in the host-validated dependency order and releases the exact scheduler charge only after all ack.
         * @return Current operation snapshot or a preserved poll/stale acknowledgement error; errors retain every pending owner/charge.
         */
        [[nodiscard]] Result<StreamingCellOperation> PollRetirement();
        /**
         * @brief Transfers successful resources to their resident/active owner.
         * @return Complete participant ownership, or typed lifecycle failure before successful terminal completion.
         */
        [[nodiscard]] Result<std::vector<std::unique_ptr<IStreamingCellRetirementParticipant>>> TakeSucceededParticipants();
        /**
         * @brief Consumes the immutable terminal result exactly once after scheduler release.
         * @return Terminal operation, or typed lifecycle failure while pending or after consumption.
         */
        [[nodiscard]] Result<StreamingCellOperation> TakeTerminalResult();
        /** @brief Returns the canonical snapshot last accepted by the scheduler. @return Owned current operation view. */
        [[nodiscard]] const StreamingCellOperation &Operation() const noexcept;
        /** @brief Returns the latest accepted demand revision. @return Exact demand revision. */
        [[nodiscard]] StreamingCellDemandRevision Revision() const noexcept;
        /** @brief Returns retained demand. @return Effective residency, independent of cleanup disposition. */
        [[nodiscard]] StreamingDesiredResidency Desired() const noexcept;
        /** @brief Reports demand requiring fresh admission after completed reversal. @return True only after interrupted retirement. */
        [[nodiscard]] bool RequiresFreshAttempt() const noexcept;

    private:
        StreamingCellDirectionOwner(StreamingSchedulerAdmissionLedger &scheduler, StreamingCellOperation operation,
                                    StreamingSchedulerReservation reservation, const StreamingCellDirectionConfig &config,
                                    std::vector<std::unique_ptr<IStreamingCellRetirementParticipant>> participants) noexcept;
        /** @brief Retains the first cleanup disposition and fences publication before adapter calls. */
        [[nodiscard]] Result<void> BeginInterruption(StreamingCellOperationTransition reason);
        /** @brief Revokes every participant before ordered cleanup begins. */
        void RevokeParticipants() const noexcept;
        /** @brief Releases canonical terminal capacity exactly once. @return Success or the scheduler error. */
        [[nodiscard]] Result<void> ReleaseTerminal();

        StreamingSchedulerAdmissionLedger *scheduler_{};
        StreamingCellOperation operation_;
        std::optional<StreamingSchedulerReservation> reservation_;
        StreamingCellDemandRevision revision_;
        StreamingDesiredResidency desired_{};
        std::vector<std::unique_ptr<IStreamingCellRetirementParticipant>> participants_;
        std::size_t nextRetirement_{};
        bool retirementStarted_{};
        bool demandClosed_{};
        bool terminalConsumed_{};
        bool participantsConsumed_{};
    };
}  // namespace Horo::WorldStreaming
