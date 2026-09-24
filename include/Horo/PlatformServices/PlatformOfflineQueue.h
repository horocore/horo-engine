#pragma once

/**
 * @file PlatformOfflineQueue.h
 * @brief Provider-neutral bounded ordering, coalescing, expiry, and retention for offline intents.
 */

#include "Horo/Foundation/Result.h"
#include "Horo/PlatformServices/PlatformServiceInterfaces.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace Horo::PlatformServices {
    inline constexpr std::size_t PlatformOfflineQueueMaximumActiveIntents = 4096;
    inline constexpr std::size_t PlatformOfflineQueueMaximumActiveIntentsPerSubject = 1024;
    inline constexpr std::size_t PlatformOfflineQueueMaximumRetainedIntents = 16384;
    inline constexpr std::size_t PlatformOfflineQueueMaximumPresenceDetailBytes = 512;
    inline constexpr auto PlatformOfflineQueueMaximumProgressionAge = std::chrono::days{30};
    inline constexpr auto PlatformOfflineQueueMaximumPresenceAge = std::chrono::minutes{15};
    inline constexpr auto PlatformOfflineQueueMaximumTerminalRetention = std::chrono::days{30};

    namespace detail {
        /** @brief Checks whether a fixed byte array contains a nonzero identity value. */
        template <std::size_t Size> [[nodiscard]] constexpr bool HasNonZeroByte(const std::array<std::byte, Size> &bytes) noexcept {
            return std::ranges::any_of(bytes, [](const std::byte byte) {
                return byte != std::byte{};
            });
        }
    }  // namespace detail

    /** @brief Opaque protected partition for one stable subject binding; never a provider account identifier. */
    struct PlatformOfflineSubjectPartition final {
        std::array<std::byte, 16> bytes{}; /**< Host-issued pseudonymous binding value; all-zero is invalid. */

        /** @brief Checks that the protected partition is nonzero. @return True when valid. */
        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return detail::HasNonZeroByte(bytes);
        }

        [[nodiscard]] constexpr auto operator<=>(const PlatformOfflineSubjectPartition &) const noexcept = default;
    };

    /** @brief Caller-owned nonzero identity preserved across queue coalescing and replay. */
    struct PlatformOfflineIntentId final {
        std::array<std::byte, 16> bytes{}; /**< Stable Horo intent identity; all-zero is invalid. */

        /** @brief Checks that the identity is nonzero. @return True when valid. */
        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return detail::HasNonZeroByte(bytes);
        }

        [[nodiscard]] constexpr auto operator<=>(const PlatformOfflineIntentId &) const noexcept = default;
    };

    /** @brief Nonzero semantic policy generation fencing coalescing compatibility. */
    struct PlatformOfflinePolicyGeneration final {
        std::uint64_t value{}; /**< Zero is reserved for an invalid generation. */

        /** @brief Checks representation. @return True when valid. */
        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return value != 0;
        }

        [[nodiscard]] constexpr auto operator<=>(const PlatformOfflinePolicyGeneration &) const noexcept = default;
    };

    /** @brief Nonzero generation fencing operation handles to one queue lifetime. */
    struct PlatformOfflineQueueGeneration final {
        std::uint64_t value{}; /**< Zero is reserved for an invalid generation. */

        /** @brief Checks representation. @return True when valid. */
        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return value != 0;
        }

        [[nodiscard]] constexpr auto operator<=>(const PlatformOfflineQueueGeneration &) const noexcept = default;
    };

    /** @brief Target identity participating in one progression ordering lane. */
    using PlatformOfflineProgressionTarget = std::variant<AchievementId, LeaderboardId, StatId>;

    /** @brief Exact per-subject progression lane; unrelated definitions never share an ordering queue. */
    struct PlatformOfflineProgressionLane final {
        PlatformOfflineSubjectPartition subject;
        PlatformOfflineProgressionTarget target;
        PlatformOfflinePolicyGeneration policy;
        PlatformAccessPolicyRevision accessPolicy;

        [[nodiscard]] constexpr bool operator==(const PlatformOfflineProgressionLane &) const noexcept = default;
    };

    /** @brief Purpose for the sole presence category eligible for opt-in durable intent. */
    enum class PlatformOfflinePresencePurpose : std::uint8_t {
        PresencePublish
    };

    /** @brief Exact per-subject presence desired-state lane. */
    struct PlatformOfflinePresenceLane final {
        PlatformOfflineSubjectPartition subject;
        PlatformOfflinePresencePurpose purpose{PlatformOfflinePresencePurpose::PresencePublish};
        PlatformOfflinePolicyGeneration policy;
        PlatformAccessPolicyRevision accessPolicy;

        [[nodiscard]] constexpr bool operator==(const PlatformOfflinePresenceLane &) const noexcept = default;
    };

    /** @brief Disjoint progression and presence ordering lanes. */
    using PlatformOfflineLaneKey = std::variant<PlatformOfflineProgressionLane, PlatformOfflinePresenceLane>;

    /** @brief Declared score ordering used by best-score coalescing. */
    enum class PlatformOfflineScoreOrder : std::uint8_t {
        LowerIsBetter,
        HigherIsBetter
    };

    /** @brief Idempotent unlock receipt; distinct IDs remain independently observable. */
    struct PlatformOfflineUnlockOnce final {
        AchievementId achievement;
        [[nodiscard]] constexpr bool operator==(const PlatformOfflineUnlockOnce &) const noexcept = default;
    };

    /** @brief Monotonic achievement progress intent reduced by maximum value. */
    struct PlatformOfflineSetProgressMaximum final {
        AchievementId achievement;
        std::uint64_t progress{};
        std::uint64_t total{};
        [[nodiscard]] constexpr bool operator==(const PlatformOfflineSetProgressMaximum &) const noexcept = default;
    };

    /** @brief Monotonic statistic intent reduced by maximum value. */
    struct PlatformOfflineSetStatMaximum final {
        StatId stat;
        std::int64_t value{};
        [[nodiscard]] constexpr bool operator==(const PlatformOfflineSetStatMaximum &) const noexcept = default;
    };

    /** @brief Monotonic statistic intent reduced by minimum value. */
    struct PlatformOfflineSetStatMinimum final {
        StatId stat;
        std::int64_t value{};
        [[nodiscard]] constexpr bool operator==(const PlatformOfflineSetStatMinimum &) const noexcept = default;
    };

    /** @brief Best-score intent reduced using the definition's declared score ordering. */
    struct PlatformOfflineSubmitBestScore final {
        LeaderboardId leaderboard;
        std::int64_t score{};
        PlatformOfflineScoreOrder order{PlatformOfflineScoreOrder::HigherIsBetter};
        [[nodiscard]] constexpr bool operator==(const PlatformOfflineSubmitBestScore &) const noexcept = default;
    };

    /** @brief Conditional statistic replacement that is never coalesced. */
    struct PlatformOfflineReplaceStatAtRevision final {
        StatId stat;
        std::int64_t value{};
        std::uint64_t expectedRevision{};
        [[nodiscard]] constexpr bool operator==(const PlatformOfflineReplaceStatAtRevision &) const noexcept = default;
    };

    /** @brief Conditional score replacement that is never coalesced. */
    struct PlatformOfflineReplaceScoreAtRevision final {
        LeaderboardId leaderboard;
        std::int64_t score{};
        std::uint64_t expectedRevision{};
        [[nodiscard]] constexpr bool operator==(const PlatformOfflineReplaceScoreAtRevision &) const noexcept = default;
    };

    /** @brief Exactly-once statistic delta; it is never coalesced. */
    struct PlatformOfflineAddStatOnce final {
        StatId stat;
        std::int64_t delta{};
        [[nodiscard]] constexpr bool operator==(const PlatformOfflineAddStatOnce &) const noexcept = default;
    };

    /** @brief Latest desired presence state; old unsent values become explicitly Superseded. */
    struct PlatformOfflinePresenceDesiredState final {
        enum class Action : std::uint8_t {
            Set,
            Clear
        };

        Action action{Action::Clear};
        PresenceStatusId status;
        std::string detail;
        [[nodiscard]] bool operator==(const PlatformOfflinePresenceDesiredState &) const noexcept = default;
    };

    /** @brief Admissible logical operations; cloud writes and all reads are intentionally unrepresentable. */
    using PlatformOfflineOperation =
        std::variant<PlatformOfflineUnlockOnce, PlatformOfflineSetProgressMaximum, PlatformOfflineSetStatMaximum,
                     PlatformOfflineSetStatMinimum, PlatformOfflineSubmitBestScore, PlatformOfflineReplaceStatAtRevision,
                     PlatformOfflineReplaceScoreAtRevision, PlatformOfflineAddStatOnce, PlatformOfflinePresenceDesiredState>;

    /** @brief One immutable Horo logical intent; identity allocation and replay eligibility belong to semantic owners. */
    struct PlatformOfflineIntent final {
        PlatformOfflineIntentId id;
        PlatformOfflineLaneKey lane;
        PlatformOfflineOperation operation;
        [[nodiscard]] bool operator==(const PlatformOfflineIntent &) const noexcept = default;
    };

    /** @brief Queue execution state for one aggregate operation. */
    enum class PlatformOfflineOperationState : std::uint8_t {
        Pending,
        Dispatching,
        Reconciling,
        Suspended,
        Terminal
    };

    /** @brief Per-intent outcome; expiry, supersession and failure can never be reported as success. */
    enum class PlatformOfflineIntentState : std::uint8_t {
        Pending,
        Dispatching,
        Reconciling,
        Suspended,
        Succeeded,
        PermanentlyFailed,
        Expired,
        Superseded,
        Abandoned
    };

    /** @brief Nonzero generation and insertion sequence for one queue-owned aggregate operation. */
    struct PlatformOfflineOperationHandle final {
        PlatformOfflineQueueGeneration generation;
        std::uint64_t sequence{};

        /** @brief Checks that both handle components are valid. @return True when valid. */
        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return generation.IsValid() && sequence != 0;
        }

        [[nodiscard]] constexpr auto operator<=>(const PlatformOfflineOperationHandle &) const noexcept = default;
    };

    /** @brief Individual producer receipt retained within an aggregate operation. */
    struct PlatformOfflineReceiptSnapshot final {
        PlatformOfflineIntentId id;
        PlatformOfflineIntentState state{PlatformOfflineIntentState::Pending};
        std::chrono::steady_clock::time_point admittedAt;
        std::chrono::steady_clock::time_point expiresAt;
        std::optional<std::chrono::steady_clock::time_point> terminalAt;
    };

    /** @brief Immutable view of one ordered operation and all preserved producer receipts. */
    struct PlatformOfflineOperationSnapshot final {
        PlatformOfflineOperationHandle handle;
        PlatformOfflineLaneKey lane;
        PlatformOfflineOperation operation;
        PlatformOfflineOperationState state{PlatformOfflineOperationState::Pending};
        std::vector<PlatformOfflineReceiptSnapshot> receipts;
    };

    /** @brief Admission disposition after exact-identity joining, legal coalescing, or presence replacement. */
    enum class PlatformOfflineAdmissionDisposition : std::uint8_t {
        Accepted,
        JoinedExisting,
        Coalesced,
        PresenceSuperseded
    };

    /** @brief Admission receipt; Superseded and Expired IDs are reported for semantic-owner notification. */
    struct PlatformOfflineAdmission final {
        PlatformOfflineAdmissionDisposition disposition{PlatformOfflineAdmissionDisposition::Accepted};
        PlatformOfflineOperationHandle operation;
        std::vector<PlatformOfflineIntentId> superseded;
    };

    /** @brief Explicit finite limits for the in-memory queue policy core. */
    struct PlatformOfflineQueueConfig final {
        std::size_t activeCapacity{256};                                    /**< Maximum active receipts across all protected subjects. */
        std::size_t perSubjectActiveCapacity{64};                           /**< Maximum active receipts for one protected subject. */
        std::size_t retainedCapacity{1024};                                 /**< Maximum receipts retained before compaction. */
        std::chrono::seconds progressionMaximumAge{std::chrono::hours{24}}; /**< TTL for progression intents. */
        std::chrono::seconds presenceMaximumAge{std::chrono::minutes{5}};   /**< Short TTL for presence desired state. */
        std::chrono::seconds terminalRetention{std::chrono::hours{24}};     /**< Minimum time terminal outcomes remain queryable. */
        std::chrono::seconds producerRedeliveryHorizon{std::chrono::hours{24}}; /**< Duplicate-delivery window covered by retention. */
        std::size_t maximumPresenceDetailBytes{256};                            /**< Maximum UTF-8 detail payload size. */
        PlatformOfflineQueueGeneration generation{1};                           /**< Queue-lifetime fence for operation handles. */
    };

    /**
     * @brief Bounded owner-lane policy state for ordered offline intents.
     * @details This in-memory core does not claim durable admission. A persistent owner must atomically commit each returned
     * transition before scheduling provider work or publishing a durable receipt. Callers advance expiry explicitly;
     * compaction removes terminal operations only after terminal retention, which is validated to cover producer redelivery.
     * Operations in one lane retain sequence order, while independent lanes have no cross-lane FIFO guarantee. Only declared
     * monotonic/best/unlock operations coalesce. The semantic owner must qualify replay safety before admission.
     */
    class PlatformOfflineQueue final {
    public:
        using Clock = std::chrono::steady_clock;
        using TimePoint = Clock::time_point;

        /**
         * @brief Creates one finite queue generation.
         * @param config Product limits validated before the first mutation.
         */
        explicit PlatformOfflineQueue(const PlatformOfflineQueueConfig &config = {});
        ~PlatformOfflineQueue();
        PlatformOfflineQueue(const PlatformOfflineQueue &) = delete;
        PlatformOfflineQueue &operator=(const PlatformOfflineQueue &) = delete;
        PlatformOfflineQueue(PlatformOfflineQueue &&) = delete;
        PlatformOfflineQueue &operator=(PlatformOfflineQueue &&) = delete;

        /**
         * @brief Admits or joins one caller-owned logical intent.
         * @param intent Typed intent with an already allocated Horo identity.
         * @param now Monotonic time supplied by the queue owner.
         * @return Admission receipt, or typed invalid/capacity/identity/clock failure.
         */
        [[nodiscard]] Result<PlatformOfflineAdmission> Admit(PlatformOfflineIntent intent, TimePoint now);

        /**
         * @brief Returns the aggregate operation for a live or retained producer receipt.
         * @param id Exact producer intent identity.
         * @return Immutable operation snapshot, or Expired after retention/compaction.
         */
        [[nodiscard]] Result<PlatformOfflineOperationSnapshot> Query(PlatformOfflineIntentId id) const;

        /**
         * @brief Lists pending operations in one exact semantic lane, in insertion sequence.
         * @param lane Exact subject/definition-or-purpose/policy lane.
         * @return Pending aggregates in deterministic lane order; callers should call Expire first.
         */
        [[nodiscard]] std::vector<PlatformOfflineOperationSnapshot> PendingInLane(const PlatformOfflineLaneKey &lane) const;

        /**
         * @brief Marks expired pending or suspended receipts as terminal Expired outcomes.
         * @param now Monotonic time supplied by the queue owner.
         * @return Every newly expired receipt ID in stable sequence order.
         */
        [[nodiscard]] Result<std::vector<PlatformOfflineIntentId>> Expire(TimePoint now);

        /**
         * @brief Begins an operation before its earliest live receipt expires.
         * @param operation Aggregate operation handle.
         * @param now Monotonic time supplied by the queue owner.
         * @return Applied/unchanged, or expired/stale/invalid-transition failure.
         */
        [[nodiscard]] Result<PlatformRequestMutation> MarkDispatching(PlatformOfflineOperationHandle operation, TimePoint now);

        /**
         * @brief Records unresolved remote ambiguity; this state is immune to ordinary TTL expiry and compaction.
         * @param operation Aggregate operation handle.
         * @return Applied/unchanged, or stale/invalid-transition failure.
         */
        [[nodiscard]] Result<PlatformRequestMutation> MarkReconciling(PlatformOfflineOperationHandle operation);

        /**
         * @brief Suspends only work that has not crossed the dispatch boundary.
         * @param operation Aggregate operation handle.
         * @return Applied/unchanged, or stale/invalid-transition failure.
         */
        [[nodiscard]] Result<PlatformRequestMutation> SuspendPending(PlatformOfflineOperationHandle operation);

        /**
         * @brief Abandons only unsent pending or suspended work; it never requests provider cancellation.
         * @param operation Aggregate operation handle.
         * @param now Monotonic time supplied by the queue owner.
         * @return Applied/unchanged, or expired/stale/invalid-transition failure.
         */
        [[nodiscard]] Result<PlatformRequestMutation> CancelPending(PlatformOfflineOperationHandle operation, TimePoint now);

        /**
         * @brief Resumes suspended work without resetting its original expiry.
         * @param operation Aggregate operation handle.
         * @param now Monotonic time supplied by the queue owner.
         * @return Applied/unchanged, or expired/stale/invalid-transition failure.
         */
        [[nodiscard]] Result<PlatformRequestMutation> Resume(PlatformOfflineOperationHandle operation, TimePoint now);

        /**
         * @brief Publishes a confirmed remote success for dispatched or reconciled work.
         * @param operation Aggregate operation handle.
         * @param now Monotonic time supplied by the queue owner.
         * @return Applied/unchanged, or stale/invalid-transition failure.
         */
        [[nodiscard]] Result<PlatformRequestMutation> CompleteSuccess(PlatformOfflineOperationHandle operation, TimePoint now);

        /**
         * @brief Publishes a semantic-owner-confirmed permanent failure.
         * @param operation Aggregate operation handle.
         * @param now Monotonic time supplied by the queue owner.
         * @return Applied/unchanged, or stale/invalid-transition failure.
         */
        [[nodiscard]] Result<PlatformRequestMutation> CompletePermanentlyFailed(PlatformOfflineOperationHandle operation, TimePoint now);

        /**
         * @brief Closes admission, suspends unsent work, and retains dispatched work for reconciliation.
         * @param now Monotonic time supplied by the queue owner.
         * @return Newly expired receipt IDs; unresolved work is never deleted or marked successful.
         */
        [[nodiscard]] Result<std::vector<PlatformOfflineIntentId>> Shutdown(TimePoint now);

        /**
         * @brief Removes only fully terminal operations older than terminalRetention.
         * @param now Monotonic time supplied by the queue owner.
         * @return Every compacted receipt ID in stable sequence order; later queries return Expired.
         */
        [[nodiscard]] Result<std::vector<PlatformOfflineIntentId>> Compact(TimePoint now);

        /** @brief Returns the immutable queue generation. */
        [[nodiscard]] PlatformOfflineQueueGeneration Generation() const noexcept;
        /** @brief Returns active producer receipt count. */
        [[nodiscard]] std::size_t ActiveIntentCount() const noexcept;
        /** @brief Returns all retained producer receipts, including terminal outcomes. */
        [[nodiscard]] std::size_t RetainedIntentCount() const noexcept;
        /** @brief Returns aggregate operation count, including terminal records awaiting compaction. */
        [[nodiscard]] std::size_t OperationCount() const noexcept;

    private:
        struct State;
        PlatformOfflineQueueConfig config_;
        std::vector<State> operations_;
        std::size_t activeIntentCount_{};
        std::size_t retainedIntentCount_{};
        std::uint64_t nextSequence_{1};
        std::optional<TimePoint> lastObservedAt_;
        bool closed_{};

        /** @brief Checks configured capacities and lifetimes against engine hard maxima. @return True when valid. */
        [[nodiscard]] bool ConfigurationIsValid() const noexcept;
        /** @brief Advances the monotonic high-water mark. @param now Candidate timestamp. @return Success or invalid-time error. */
        [[nodiscard]] Result<void> ObserveTime(TimePoint now);
        /** @brief Adds a bounded lifetime without overflowing the monotonic clock. @param time Base time. @param age Lifetime. @return Safe
         * deadline. */
        [[nodiscard]] static std::optional<TimePoint> AddAge(TimePoint time, std::chrono::seconds age) noexcept;
        /** @brief Finds the newest non-terminal operation in one lane. @param lane Semantic lane. @return Tail or null. */
        [[nodiscard]] State *FindTail(const PlatformOfflineLaneKey &lane) noexcept;
        /** @brief Counts active receipts for one protected subject. @param subject Subject partition. @return Active count. */
        [[nodiscard]] std::size_t ActiveForSubject(const PlatformOfflineSubjectPartition &subject) const noexcept;
        /** @brief Attempts bounded coalescing into a pending lane tail. @return Admission, no-op, or typed capacity failure. */
        [[nodiscard]] Result<std::optional<PlatformOfflineAdmission>> TryCoalesceTail(State *tail, PlatformOfflineIntent &intent,
                                                                                      TimePoint now);
        /** @brief Admits a new aggregate after joining/coalescing was ruled out. @return New admission or typed capacity failure. */
        [[nodiscard]] Result<PlatformOfflineAdmission> AdmitFresh(PlatformOfflineIntent intent, TimePoint now, TimePoint expiresAt,
                                                                  State *tail);
        /** @brief Marks active presence receipts as superseded. @return Superseded producer identities. */
        [[nodiscard]] std::vector<PlatformOfflineIntentId> SupersedePresence(State &tail, TimePoint now);
        /** @brief Checks whether all active receipts in a state remain before their deadlines. @return True when live. */
        [[nodiscard]] static bool ReceiptsRemainLive(const State &state, TimePoint now);
        /** @brief Copies immutable operation and receipt state. @param state Queue-owned record. @return Detached snapshot. */
        [[nodiscard]] PlatformOfflineOperationSnapshot Snapshot(const State &state) const;
        /** @brief Expires safe unsent receipts. @param state Queue-owned record. @param now Current time. @param expired Optional event
         * sink. */
        void ExpireDue(State &state, TimePoint now, std::vector<PlatformOfflineIntentId> *expired);
        /** @brief Commits one terminal receipt outcome. @param state Queue-owned record. @param terminal Final outcome. @param now Commit
         * time. */
        [[nodiscard]] Result<PlatformRequestMutation> CompleteOperation(State &state, PlatformOfflineIntentState terminal, TimePoint now);
    };
}  // namespace Horo::PlatformServices
