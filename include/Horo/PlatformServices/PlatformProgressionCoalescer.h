#pragma once

/** @file PlatformProgressionCoalescer.h
 * @brief Bounded volatile progression aggregation with immutable dispatch and individual outcomes.
 */

#include "Horo/PlatformServices/PlatformProgressionIdempotency.h"
#include "Horo/PlatformServices/PlatformServiceErrors.h"

#include <memory>

namespace Horo::PlatformServices {
    /** @brief Logical outcome, independent of any ADR-130 attempt's terminal record. */
    enum class PlatformProgressionOutcome : std::uint8_t {
        Pending,
        InFlight,
        Succeeded,
        Failed,
        Cancelled,
        RemoteOutcomeUnknown
    };

    /** @brief Atomic logical observation; a failure category and its outcome always describe the same receipt state. */
    struct PlatformProgressionReceiptSnapshot final {
        PlatformProgressionOutcome outcome{PlatformProgressionOutcome::Pending};
        std::optional<PlatformProviderFailureCategory> failure;
    };

    /** @brief Immutable dispatch token; the strongest original envelope carries its original mutation ID. */
    struct PlatformProgressionDispatch final {
        std::uint64_t sequence{};
        PlatformProgressionMutationEnvelope envelope;
        [[nodiscard]] bool operator==(const PlatformProgressionDispatch &) const noexcept = default;
    };

    /** @brief Finite logical retention and a fixed window measured in caller-owned monotonic ticks. */
    struct PlatformProgressionCoalescerConfig final {
        std::size_t maximumReceipts{256}; /**< Includes terminal receipts; exhaustion never evicts dedupe evidence. */
        std::uint64_t windowTicks{1};     /**< Fixed from first arrival; later arrivals never extend the deadline. */
    };

    /**
     * @brief Thread-safe semantic aggregation of already authorized, provider-qualified envelopes.
     * @details This tooling/background admission path never invokes callbacks, providers, or durable storage. The composition owner
     * must validate definition, authority, session and atomic provider semantics before Admit. One mutex serializes admission,
     * dispatch and completion; no borrowed state escapes it. Close cancels undispatched work and marks in-flight effects unknown.
     * Terminal receipts remain observable until destruction; callers must quiesce threads before destroying the owner.
     */
    class PlatformProgressionCoalescer final {
    public:
        /** @brief Creates a bounded owner. @param config Finite retention/window.
         * @return Owner, InvalidConfiguration, or CapacityExceeded when bounded storage cannot be allocated. */
        [[nodiscard]] static Result<std::unique_ptr<PlatformProgressionCoalescer>> Create(PlatformProgressionCoalescerConfig config = {});
        ~PlatformProgressionCoalescer();
        PlatformProgressionCoalescer(const PlatformProgressionCoalescer &) = delete;
        PlatformProgressionCoalescer &operator=(const PlatformProgressionCoalescer &) = delete;

        /**
         * @brief Admits an immutable logical intent or joins its exact retained receipt.
         * @param envelope Authorized and qualified original intent.
         * @param ordering Immutable registered ordering (used only for SubmitBestScore).
         * @param tick Nondecreasing monotonic tick; backwards clocks fail closed.
         * @return Stable receipt/disposition, conflict, invalid input, capacity or Closed.
         */
        [[nodiscard]] Result<PlatformProgressionAdmission> Admit(const PlatformProgressionMutationEnvelope &envelope,
                                                                 LeaderboardOrdering ordering, std::uint64_t tick);
        /** @brief Takes one ready aggregate with no in-flight operation for the same subject/definition.
         * @param tick Nondecreasing monotonic tick. @return Immutable token, empty, or typed failure. */
        [[nodiscard]] Result<std::optional<PlatformProgressionDispatch>> TakeReady(std::uint64_t tick);
        /** @brief Publishes the same conclusive/unknown outcome to every member, exactly once.
         * @param dispatch Exact issued token. @param outcome Succeeded, Failed or RemoteOutcomeUnknown.
         * @param failure Normalized provider category for Failed, retained for every member; no provider text is accepted.
         * @return Success or InvalidMutation/Closed; late/conflicting completions cannot rewrite terminal outcomes.
         * RemoteOutcomeUnknown retains the exact token for later conclusive reconciliation evidence. */
        [[nodiscard]] Result<void> Complete(const PlatformProgressionDispatch &dispatch, PlatformProgressionOutcome outcome,
                                            std::optional<PlatformProviderFailureCategory> failure = std::nullopt);
        /** @brief Cancels only an undispatched logical receipt; dispatched effects require owner reconciliation.
         * @param mutation Original receipt identity. @return Success or InvalidMutation/Closed. */
        [[nodiscard]] Result<void> Cancel(PlatformProgressionMutationId mutation);
        /** @brief Observes an individual receipt without changing retention.
         * @param mutation Original receipt identity. @return Copied outcome, or empty for unknown identity. */
        [[nodiscard]] std::optional<PlatformProgressionOutcome> Observe(PlatformProgressionMutationId mutation) const;
        /** @brief Observes retained normalized failure evidence without exposing provider strings.
         * @param mutation Original receipt identity. @return Failure category for Failed, otherwise empty. */
        [[nodiscard]] std::optional<PlatformProviderFailureCategory> FailureCategory(PlatformProgressionMutationId mutation) const;
        /** @brief Observes one receipt only within its captured subject/session/access scope.
         * @param mutation Original receipt identity. @param scope Exact captured scope.
         * @return One atomic outcome/category pair, or empty for an unknown or differently scoped identity. */
        [[nodiscard]] std::optional<PlatformProgressionReceiptSnapshot> Query(PlatformProgressionMutationId mutation,
                                                                              const PlatformProgressionSessionScope &scope) const;
        /** @brief Closes admission and preserves each terminal/unknown outcome; idempotent. */
        void Close();

    private:
        struct Impl;
        explicit PlatformProgressionCoalescer(PlatformProgressionCoalescerConfig config);
        std::unique_ptr<Impl> impl_;
    };
}  // namespace Horo::PlatformServices
