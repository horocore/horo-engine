#pragma once

/**
 * @file UiFeedback.h
 * @brief Bounded semantic Runtime UI feedback intents and optional host realization.
 */

#include "Horo/Runtime/Ui/UiControls.h"
#include "Horo/Runtime/Ui/UiFocusGraph.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>

namespace Horo::Runtime::Ui {
    inline constexpr std::uint32_t MaximumUiFeedbackIntents = 1'024;

    /** @brief Closed semantic feedback vocabulary, independent of audio clips or haptic devices. */
    enum class UiFeedbackKind : std::uint8_t {
        Navigate,
        Focus,
        Confirm,
        Cancel,
        Error,
        Boundary,
        Count
    };

    /** @brief Owned, generation-fenced intent emitted after a semantic UI outcome. */
    struct UiFeedbackIntent final {
        UiFeedbackKind kind{UiFeedbackKind::Count}; /**< Semantic cue; no backend resource identity. */
        UiActionSource source;                      /**< Exact presented owner and originating element. */
        std::optional<UiFocusScope> focusScope;     /**< Audience when produced by a focus graph. */
        std::optional<UiElementHandle> target;      /**< Resolved destination, when distinct from the source. */

        /** @brief Validates kind, source, audience and destination ownership. @return Whether the intent is well formed. */
        [[nodiscard]] bool IsValid() const noexcept;
    };

    /** @brief Outcome for one optional host modality; neither modality is required for UI success. */
    enum class UiFeedbackModalityResult : std::uint8_t {
        Skipped,
        Realized,
        Unavailable,
        Failed,
        Count
    };

    /** @brief Explicit result of an out-of-control host realization attempt. */
    struct UiFeedbackRealization final {
        UiFeedbackModalityResult audio{UiFeedbackModalityResult::Skipped};  /**< Audio outcome. */
        UiFeedbackModalityResult haptic{UiFeedbackModalityResult::Skipped}; /**< Haptic outcome. */

        /** @brief Validates both closed modality results. @return Whether the result is well formed. */
        [[nodiscard]] bool IsValid() const noexcept;
    };

    /** @brief Borrowed host boundary for optional audio/haptic realization outside controls. */
    class UiFeedbackRealizer {
    public:
        virtual ~UiFeedbackRealizer() = default;

        /**
         * @brief Attempts optional realization at a host safe point.
         * @param intent Borrowed semantic intent; the realizer may not retain references to it.
         * @return Per-modality outcome or a typed host failure; UI behavior never depends on realization.
         */
        [[nodiscard]] virtual Result<UiFeedbackRealization> Realize(const UiFeedbackIntent &intent) = 0;
    };

    /** @brief Explicit owner-thread lifecycle of one feedback queue generation. */
    enum class UiFeedbackQueueState : std::uint8_t {
        Active,
        Retiring,
        Stopped
    };

    /** @brief Exact owner evidence and preallocated queue bound. */
    struct UiFeedbackQueueDescriptor final {
        UiActionOwnerContext owner;           /**< Exact active runtime/canvas/document/presentation generation. */
        std::uint32_t maximumQueuedIntents{}; /**< Finite capacity reserved at creation. */

        /** @brief Validates owner and capacity. @return Whether creation can proceed. */
        [[nodiscard]] bool IsValid() const noexcept;
    };

    /**
     * @brief Owner-thread semantic feedback producer with bounded FIFO storage.
     * @details Observe and TryDequeue never allocate or invoke a host provider. Retire on reload, discard stale
     *          intents at shutdown, and create a new queue for the new presented owner generation. A host may use
     *          TryDequeue or DeliverNext at a non-frame-hot safe point; no realizer is retained.
     */
    class UiFeedbackQueue final {
    public:
        /**
         * @brief Allocates complete storage before any frame work.
         * @param descriptor Exact owner and finite queue capacity.
         * @return Active queue or typed validation/allocation failure.
         */
        [[nodiscard]] static Result<UiFeedbackQueue> Create(const UiFeedbackQueueDescriptor &descriptor);

        ~UiFeedbackQueue();
        UiFeedbackQueue(UiFeedbackQueue &&) noexcept;
        UiFeedbackQueue &operator=(UiFeedbackQueue &&) noexcept;
        UiFeedbackQueue(const UiFeedbackQueue &) = delete;
        UiFeedbackQueue &operator=(const UiFeedbackQueue &) = delete;

        /**
         * @brief Emits feedback from a correlated action result, including navigation and rejection outcomes.
         * @param request Original owner-admitted request.
         * @param result Validated result for exactly that request.
         * @return Whether an intent was emitted, or typed stale/capacity/lifecycle failure.
         */
        [[nodiscard]] Result<bool> ObserveAction(const UiActionRequest &request, const UiActionResult &result);

        /**
         * @brief Emits focus or boundary feedback from an owner-scoped focus outcome.
         * @param owner Exact focus graph context and audience.
         * @param change Validated focus operation result.
         * @return Whether an intent was emitted; reload/retirement recovery is deliberately silent.
         */
        [[nodiscard]] Result<bool> ObserveFocus(const UiFocusOwnerContext &owner, const UiFocusChange &change);

        /**
         * @brief Emits confirm or navigate feedback only after a routed control default was applied.
         * @param action Applied typed default action, not a mere press or pending transition.
         * @return Whether an intent was emitted or a typed failure.
         */
        [[nodiscard]] Result<bool> ObserveControl(const UiControlDefaultAction &action);

        /**
         * @brief Emits cancel feedback for a genuine control cancellation, not an ignored/no-op input.
         * @param input Owner-addressed input that produced the transition.
         * @param transition Resulting control transition category.
         * @return Whether a cancel intent was emitted or a typed failure.
         */
        [[nodiscard]] Result<bool> ObserveControlTransition(const UiControlInput &input, UiControlTransitionKind transition);

        /**
         * @brief Removes at most one pending intent without invoking audio or haptics.
         * @return Empty when no intent remains, otherwise an owned value; shutdown rejects delivery.
         */
        [[nodiscard]] Result<std::optional<UiFeedbackIntent>> TryDequeue();

        /**
         * @brief Consumes at most one intent through a borrowed host realizer at a non-frame-hot safe point.
         * @param realizer Optional host audio/haptic adapter; never retained by this queue.
         * @return Empty when no intent remains, otherwise validated modality outcomes or a typed consumer failure.
         * @note A failed or throwing realizer does not retry the consumed intent or change UI action outcomes.
         */
        [[nodiscard]] Result<std::optional<UiFeedbackRealization>> DeliverNext(UiFeedbackRealizer &realizer);

        /** @brief Closes emission and discards old-generation cues. @return Success or lifecycle failure. */
        [[nodiscard]] Result<void> BeginRetirement();
        /** @brief Idempotently closes the queue and discards all pending intents. */
        void Shutdown() noexcept;
        /** @brief Returns the current lifecycle state. @return Active, Retiring or Stopped. */
        [[nodiscard]] UiFeedbackQueueState State() const noexcept;
        /** @brief Returns the pending bounded count. @return Number of queued intents. */
        [[nodiscard]] std::size_t QueuedCount() const noexcept;

    private:
        struct Storage;
        explicit UiFeedbackQueue(std::unique_ptr<Storage> storage) noexcept;
        [[nodiscard]] Result<bool> Enqueue(UiFeedbackIntent intent);
        [[nodiscard]] bool Matches(const UiActionSource &source) const noexcept;

        std::unique_ptr<Storage> storage_;
    };
}  // namespace Horo::Runtime::Ui
