#pragma once

/**
 * @file EventDispatcher.h
 * @brief Session-owned, commit-gated cinematic gameplay callback dispatch.
 */

#include "Horo/Cinematic/EventTrack.h"
#include "Horo/Cinematic/SequenceEvaluation.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace Horo::Cinematic {
    /** @brief Typed destination outcome; accepted commands commit under their gameplay owner. */
    enum class EventDispatchOutcome : std::uint8_t {
        Accepted,
        OperationStarted,
        SuppressedByAuthority,
        CapabilityUnavailable,
        InvalidTarget,
        Backpressured,
        HandlerFailed,
        BindingUnavailable,
        StaleBinding,
        Count
    };

    /** @brief Immutable owned payload view valid only during a synchronous owner-safe-point callback. */
    struct EventDispatchRequest final {
        SequenceFrameEventOccurrence occurrence;
        EventBindingId binding;
        EventPayloadSchemaId schema;
        std::span<const std::byte> payload;
        std::uint64_t committedTick{};
    };

    using EventHandlerFunction = EventDispatchOutcome (*)(const BorrowedCallbackContext &context, const EventDispatchRequest &request);

    /** @brief Host-owned registration with a lease that keeps callback context alive until revocation. */
    struct EventHandlerRegistration final {
        EventBindingId binding;
        EventPayloadSchemaId schema;
        EventRuntimeContext context{EventRuntimeContext::Runtime};
        std::uint64_t generation{};
        BorrowedCallbackContext callbackContext;
        EventHandlerFunction callback{};
        std::shared_ptr<void> lease;
    };

    /** @brief Bounded terminal evidence for one exact occurrence identity. */
    struct EventDispatchRecord final {
        SequenceFrameEventOccurrence occurrence;
        EventBindingId binding;
        EventDispatchOutcome outcome{EventDispatchOutcome::BindingUnavailable};
        std::uint64_t committedTick{};
    };

    /**
     * @brief One owner-thread dispatcher for a runtime, PIE, preview, or headless session.
     * @note Host composition owns registrations and calls BeginTick/CommitTick or AbortTick around committed simulation.
     * No gameplay callback runs during SequenceFrameEvaluationPlan::Evaluate.
     */
    class CinematicEventDispatcher final {
    public:
        /**
         * @brief Creates finite queue/result storage for one exact session and context.
         * @param session Current runtime session identity.
         * @param context Context to which every registered adapter must be admitted.
         * @param maximumPending Maximum committed plus staged occurrences.
         * @param maximumResults Maximum retained terminal results until ClearResults.
         * @return Session dispatcher or typed invalid/capacity failure.
         */
        [[nodiscard]] static Result<CinematicEventDispatcher> Create(CinematicRuntimeSessionId session, EventRuntimeContext context,
                                                                     std::size_t maximumPending, std::size_t maximumResults);

        /** @brief Registers one host-authorized adapter and its explicit lifetime lease. @return Success or collision/state error. */
        [[nodiscard]] Result<void> Register(EventHandlerRegistration registration);
        /** @brief Revokes admission before module unload; no callback may be active. @return Success or state error. */
        [[nodiscard]] Result<void> Revoke(EventBindingId binding, std::uint64_t generation);
        /**
         * @brief Admits all required cooked bindings against the current adapter generation.
         * @param player Session-owned player with a unique generation.
         * @param plan Immutable cooked payload lease retained until player cancellation and queue retirement.
         * @param evaluation Exact compiled event key set for this player.
         * @param priority Canonical player batch priority.
         * @return Success or typed required-binding/state failure without partial activation.
         */
        [[nodiscard]] Result<void> Activate(const SequencePlayerHandle &player, std::shared_ptr<const CookedEventPlan> plan,
                                            const SequenceFrameEvaluationPlan &evaluation, std::int32_t priority);
        /** @brief Stops new events while committed occurrences remain drainable. @return Success or stale player. */
        [[nodiscard]] Result<void> StopPlayer(const SequencePlayerHandle &player);
        /** @brief Cancels undispatched occurrences and retires the player's payload lease. @return Success or stale player. */
        [[nodiscard]] Result<void> CancelPlayer(const SequencePlayerHandle &player);

        /** @brief Opens one source-tick transaction. @return Success or invalid boundary state. */
        [[nodiscard]] Result<void> BeginTick(std::uint64_t tick);
        /**
         * @brief Atomically stages one complete crossed-event batch before the player cursor commits.
         * @note Traversal ordinals advance monotonically for each player generation. Delivered ordinals and older
         * ordinals for the same key/direction are terminal even after ClearResults; cancellation retires that history.
         */
        [[nodiscard]] Result<void> Stage(std::span<const SequenceFrameEventOccurrence> occurrences);
        /** @brief Publishes staged occurrences only after the source tick commits. */
        [[nodiscard]] Result<void> CommitTick();
        /** @brief Drops a failed tick's staged occurrences without invoking any handler. */
        [[nodiscard]] Result<void> AbortTick();
        /** @brief Invokes at most maximumToDrain committed occurrences in canonical order at the owner safe point. */
        [[nodiscard]] Result<std::size_t> Drain(std::size_t maximumToDrain);
        /** @brief Returns bounded immutable terminal evidence until ClearResults. */
        [[nodiscard]] std::span<const EventDispatchRecord> Results() const noexcept;
        /** @brief Releases acknowledged diagnostic records, retaining per-key traversal watermarks until player cancellation. */
        void ClearResults() noexcept;
        /** @brief Closes admission and retires undelivered work and adapter leases at session shutdown. */
        [[nodiscard]] Result<void> Close();

        /** @brief Typed SequenceFrameHooks admission adapter; context must point to this dispatcher. */
        [[nodiscard]] static Result<void> StageHook(const BorrowedCallbackContext &context,
                                                    std::span<const SequenceFrameEventOccurrence> occurrences);

    private:
        struct Registration final {
            EventHandlerRegistration handler;
            bool active{true};
        };

        struct BindingFence final {
            EventBindingId binding;
            std::uint64_t generation{};
        };

        struct Player final {
            struct DeliveryWatermark {
                TrackId track;
                KeyframeId key;
                std::uint64_t forward{};
                std::uint64_t reverse{};
            };

            SequencePlayerHandle handle;
            std::shared_ptr<const CookedEventPlan> plan;
            std::vector<BindingFence> bindings;
            std::int32_t priority{};
            bool accepting{true};
            std::vector<TrackId> failedTracks;
            std::vector<DeliveryWatermark> delivered;
        };

        struct Pending final {
            SequenceFrameEventOccurrence occurrence;
            std::shared_ptr<const CookedEventPlan> plan;
            std::uint64_t handlerGeneration{};
            std::uint64_t tick{};
            std::int32_t priority{};
            bool committed{};
        };

        CinematicEventDispatcher(CinematicRuntimeSessionId session, EventRuntimeContext context, std::size_t maximumPending,
                                 std::size_t maximumResults);
        [[nodiscard]] const Registration *FindRegistration(EventBindingId binding) const noexcept;
        [[nodiscard]] Registration *FindRegistration(EventBindingId binding) noexcept;
        [[nodiscard]] const Player *FindPlayer(const SequencePlayerHandle &handle) const noexcept;
        [[nodiscard]] Player *FindPlayer(const SequencePlayerHandle &handle) noexcept;
        /** @brief Builds immutable activation fences and bounded delivery history before publishing the player. */
        [[nodiscard]] Result<Player> BindPlayer(const SequencePlayerHandle &player, std::shared_ptr<const CookedEventPlan> plan,
                                                std::int32_t priority) const;
        /** @brief Checks bounded per-key/direction traversal history, independent of acknowledged result storage. */
        [[nodiscard]] bool Delivered(const SequenceFrameEventOccurrence &occurrence) const noexcept;
        /** @brief Validates one occurrence before any queue mutation. */
        [[nodiscard]] Result<void> ValidateOccurrence(const SequenceFrameEventOccurrence &occurrence) const;
        /** @brief Checks queued and terminal identities without allocating. */
        [[nodiscard]] bool PreviouslySeen(const SequenceFrameEventOccurrence &occurrence) const noexcept;
        /** @brief Defines canonical committed occurrence order independent of submission order. */
        [[nodiscard]] static bool PendingLess(const Pending &left, const Pending &right) noexcept;
        /** @brief Invokes one fenced callback, translating provider exceptions and invalid outcomes. */
        [[nodiscard]] EventDispatchOutcome InvokePending(const Pending &entry, const CookedEventKey &key) const noexcept;
        /** @brief Retains terminal evidence and bounded replay/failure history. */
        void RecordTerminal(const Pending &entry, const CookedEventKey &key, EventDispatchOutcome outcome);

        CinematicRuntimeSessionId session_;
        EventRuntimeContext context_;
        std::size_t maximumPending_{};
        std::size_t maximumResults_{};
        std::uint64_t openTick_{};
        std::uint64_t lastCommittedTick_{};
        std::size_t tickStart_{};
        bool closed_{};
        bool draining_{};
        bool tickFailed_{};
        std::vector<Registration> registrations_;
        std::vector<Player> players_;
        std::vector<Pending> pending_;
        std::vector<EventDispatchRecord> results_;
    };
}  // namespace Horo::Cinematic
