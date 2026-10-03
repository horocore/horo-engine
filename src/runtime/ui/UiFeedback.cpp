#include "Horo/Runtime/Ui/UiFeedback.h"

#include "Horo/Runtime/Ui/UiErrors.h"

#include <new>
#include <type_traits>
#include <utility>
#include <vector>

namespace Horo::Runtime::Ui {
    namespace {
        template <typename T> [[nodiscard]] Result<T> Failure(const ErrorCodeDescriptor &descriptor) {
            return Result<T>::Failure(MakeError(descriptor));
        }

        template <typename Enum> [[nodiscard]] bool IsKnown(const Enum value, const Enum count) noexcept {
            return static_cast<std::underlying_type_t<Enum>>(value) < static_cast<std::underlying_type_t<Enum>>(count);
        }

        [[nodiscard]] UiActionOwnerContext ActionOwner(const UiFocusOwnerContext &owner) noexcept {
            return {owner.instance, owner.canvas, owner.document, owner.documentRevision, owner.treeRevision, owner.interaction};
        }

        /** @brief Translate a correlated navigation result after fencing its source and target ownership. */
        [[nodiscard]] Result<UiFeedbackIntent> NavigationIntent(const UiActionRequest &request,
                                                                const UiDefaultNavigationResult &navigation) {
            if (UiActionCommandKindOf(request.command) != UiActionCommandKind::Navigation)
                return Failure<UiFeedbackIntent>(UiErrors::FeedbackInvalid);
            if (navigation.from.has_value() && *navigation.from != request.source.element)
                return Failure<UiFeedbackIntent>(UiErrors::FeedbackSourceStale);
            if (navigation.target.has_value() && navigation.target->ownership != request.source.owner.instance.ownership)
                return Failure<UiFeedbackIntent>(UiErrors::FeedbackSourceStale);

            using enum UiDefaultNavigationOutcome;
            if ((navigation.outcome == SubmitDispatched || navigation.outcome == CancelDispatched) &&
                navigation.target != request.source.element)
                return Failure<UiFeedbackIntent>(UiErrors::FeedbackSourceStale);

            UiFeedbackIntent intent{.source = request.source, .target = navigation.target};
            switch (navigation.outcome) {
                case FocusMoved:
                    intent.kind = UiFeedbackKind::Navigate;
                    break;
                case SubmitDispatched:
                    intent.kind = UiFeedbackKind::Confirm;
                    break;
                case CancelDispatched:
                    intent.kind = UiFeedbackKind::Cancel;
                    break;
                case NoTarget:
                    intent.kind = UiFeedbackKind::Boundary;
                    break;
                case Count:
                    return Failure<UiFeedbackIntent>(UiErrors::FeedbackInvalid);
            }
            return Result<UiFeedbackIntent>::Success(std::move(intent));
        }

        /** @brief Translate an admitted action outcome without emitting cues for lifecycle-only results. */
        [[nodiscard]] Result<std::optional<UiFeedbackIntent>> ActionIntent(const UiActionRequest &request, const UiActionResult &result) {
            UiFeedbackIntent intent{.source = request.source};
            if (result.kind == UiActionResultKind::Rejected) {
                using enum UiActionRejectionReason;
                if (result.rejection == Stale || result.rejection == Retiring)
                    return Result<std::optional<UiFeedbackIntent>>::Success(std::nullopt);
                intent.kind = result.rejection == NoTarget ? UiFeedbackKind::Boundary : UiFeedbackKind::Error;
            } else if (result.kind == UiActionResultKind::Cancelled) {
                if (result.cancellation != UiActionCancellationReason::Requested)
                    return Result<std::optional<UiFeedbackIntent>>::Success(std::nullopt);
                intent.kind = UiFeedbackKind::Cancel;
            } else if (result.kind == UiActionResultKind::Completed && result.navigation.has_value()) {
                auto navigation = NavigationIntent(request, *result.navigation);
                if (navigation.HasError())
                    return Result<std::optional<UiFeedbackIntent>>::Failure(navigation.ErrorValue());
                intent = std::move(navigation).Value();
            } else {
                return Result<std::optional<UiFeedbackIntent>>::Success(std::nullopt);
            }
            return Result<std::optional<UiFeedbackIntent>>::Success(std::move(intent));
        }
    }  // namespace

    /** @copydoc UiFeedbackIntent::IsValid */
    bool UiFeedbackIntent::IsValid() const noexcept {
        if (!IsKnown(kind, UiFeedbackKind::Count) || !source.IsValid())
            return false;
        if (focusScope.has_value() && !focusScope->IsValid(source.owner.instance.ownership))
            return false;
        return !target.has_value() || (target->IsValid() && target->ownership == source.owner.instance.ownership);
    }

    /** @copydoc UiFeedbackRealization::IsValid */
    bool UiFeedbackRealization::IsValid() const noexcept {
        return IsKnown(audio, UiFeedbackModalityResult::Count) && IsKnown(haptic, UiFeedbackModalityResult::Count);
    }

    /** @copydoc UiFeedbackQueueDescriptor::IsValid */
    bool UiFeedbackQueueDescriptor::IsValid() const noexcept {
        return owner.IsValid() && maximumQueuedIntents > 0 && maximumQueuedIntents <= MaximumUiFeedbackIntents;
    }

    struct UiFeedbackQueue::Storage final {
        explicit Storage(const UiFeedbackQueueDescriptor &value) : descriptor(value), intents(value.maximumQueuedIntents) {}

        UiFeedbackQueueDescriptor descriptor;
        std::vector<UiFeedbackIntent> intents;
        std::size_t head{};
        std::size_t count{};
        UiFeedbackQueueState state{UiFeedbackQueueState::Active};
        bool delivering{};
    };

    UiFeedbackQueue::UiFeedbackQueue(std::unique_ptr<Storage> storage) noexcept : storage_(std::move(storage)) {}

    UiFeedbackQueue::~UiFeedbackQueue() = default;
    UiFeedbackQueue::UiFeedbackQueue(UiFeedbackQueue &&) noexcept = default;
    UiFeedbackQueue &UiFeedbackQueue::operator=(UiFeedbackQueue &&) noexcept = default;

    /** @copydoc UiFeedbackQueue::Create */
    Result<UiFeedbackQueue> UiFeedbackQueue::Create(const UiFeedbackQueueDescriptor &descriptor) {
        if (!descriptor.IsValid())
            return Failure<UiFeedbackQueue>(UiErrors::FeedbackInvalid);
        try {
            return Result<UiFeedbackQueue>::Success(UiFeedbackQueue(std::make_unique<Storage>(descriptor)));
        } catch (const std::bad_alloc &) {
            return Failure<UiFeedbackQueue>(UiErrors::FeedbackCapacityExceeded);
        }
    }

    bool UiFeedbackQueue::Matches(const UiActionSource &source) const noexcept {
        return storage_ && source.IsValid() && source.owner == storage_->descriptor.owner;
    }

    Result<bool> UiFeedbackQueue::Enqueue(UiFeedbackIntent intent) {
        if (!storage_ || storage_->state != UiFeedbackQueueState::Active || storage_->delivering)
            return Failure<bool>(UiErrors::FeedbackLifecycleUnavailable);
        if (!intent.IsValid())
            return Failure<bool>(UiErrors::FeedbackInvalid);
        if (!Matches(intent.source))
            return Failure<bool>(UiErrors::FeedbackSourceStale);
        if (storage_->count == storage_->intents.size())
            return Failure<bool>(UiErrors::FeedbackCapacityExceeded);
        const std::size_t tail = (storage_->head + storage_->count) % storage_->intents.size();
        storage_->intents[tail] = std::move(intent);
        ++storage_->count;
        return Result<bool>::Success(true);
    }

    /** @copydoc UiFeedbackQueue::ObserveAction */
    Result<bool> UiFeedbackQueue::ObserveAction(const UiActionRequest &request, const UiActionResult &result) {
        if (!storage_ || storage_->state != UiFeedbackQueueState::Active || storage_->delivering)
            return Failure<bool>(UiErrors::FeedbackLifecycleUnavailable);
        if (request.Validate().HasError() || result.Validate().HasError() || request.id != result.request)
            return Failure<bool>(UiErrors::FeedbackInvalid);
        if (!Matches(request.source))
            return Failure<bool>(UiErrors::FeedbackSourceStale);

        auto intent = ActionIntent(request, result);
        if (intent.HasError())
            return Result<bool>::Failure(intent.ErrorValue());
        if (!intent.Value().has_value())
            return Result<bool>::Success(false);
        auto selected = std::move(intent).Value();
        return Enqueue(std::move(*selected));
    }

    /** @copydoc UiFeedbackQueue::ObserveFocus */
    Result<bool> UiFeedbackQueue::ObserveFocus(const UiFocusOwnerContext &owner, const UiFocusChange &change) {
        if (!storage_ || storage_->state != UiFeedbackQueueState::Active || storage_->delivering)
            return Failure<bool>(UiErrors::FeedbackLifecycleUnavailable);
        if (!owner.IsValid() || !change.IsValid())
            return Failure<bool>(UiErrors::FeedbackInvalid);
        if (ActionOwner(owner) != storage_->descriptor.owner)
            return Failure<bool>(UiErrors::FeedbackSourceStale);
        if (change.reason == UiFocusChangeReason::Reload || change.reason == UiFocusChangeReason::Retirement)
            return Result<bool>::Success(false);

        UiFeedbackKind kind = UiFeedbackKind::Count;
        if (change.kind == UiFocusChangeKind::FocusMoved || change.kind == UiFocusChangeKind::FocusRecovered)
            kind = UiFeedbackKind::Focus;
        else if (change.kind == UiFocusChangeKind::NoTarget && change.reason == UiFocusChangeReason::InvalidTarget)
            kind = UiFeedbackKind::Boundary;
        if (kind == UiFeedbackKind::Count)
            return Result<bool>::Success(false);

        const std::optional<UiFocusTarget> &focus = change.current.has_value() ? change.current : change.previous;
        if (!focus.has_value())
            return Result<bool>::Success(false);
        if (focus->element.ownership != owner.instance.ownership)
            return Failure<bool>(UiErrors::FeedbackSourceStale);
        UiFeedbackIntent intent{.kind = kind,
                                .source = {ActionOwner(owner), focus->element},
                                .focusScope = owner.scope,
                                .target =
                                    change.current.has_value() ? std::optional<UiElementHandle>{change.current->element} : std::nullopt};
        return Enqueue(std::move(intent));
    }

    /** @copydoc UiFeedbackQueue::ObserveControl */
    Result<bool> UiFeedbackQueue::ObserveControl(const UiControlDefaultAction &action) {
        if (!storage_ || storage_->state != UiFeedbackQueueState::Active || storage_->delivering)
            return Failure<bool>(UiErrors::FeedbackLifecycleUnavailable);
        if (!action.IsValid())
            return Failure<bool>(UiErrors::FeedbackInvalid);
        if (!Matches(action.source))
            return Failure<bool>(UiErrors::FeedbackSourceStale);
        const UiFeedbackKind kind = action.kind == UiControlActionKind::ValueChanged ? UiFeedbackKind::Navigate : UiFeedbackKind::Confirm;
        return Enqueue(UiFeedbackIntent{.kind = kind, .source = action.source});
    }

    /** @copydoc UiFeedbackQueue::ObserveControlTransition */
    Result<bool> UiFeedbackQueue::ObserveControlTransition(const UiControlInput &input, const UiControlTransitionKind transition) {
        if (!storage_ || storage_->state != UiFeedbackQueueState::Active || storage_->delivering)
            return Failure<bool>(UiErrors::FeedbackLifecycleUnavailable);
        if (!input.IsValid() || !IsKnown(transition, UiControlTransitionKind::Count))
            return Failure<bool>(UiErrors::FeedbackInvalid);
        if (!Matches(input.source))
            return Failure<bool>(UiErrors::FeedbackSourceStale);
        if (transition != UiControlTransitionKind::Cancelled)
            return Result<bool>::Success(false);
        return Enqueue(UiFeedbackIntent{.kind = UiFeedbackKind::Cancel, .source = input.source});
    }

    /** @copydoc UiFeedbackQueue::TryDequeue */
    Result<std::optional<UiFeedbackIntent>> UiFeedbackQueue::TryDequeue() {
        if (!storage_ || storage_->state == UiFeedbackQueueState::Stopped || storage_->delivering)
            return Failure<std::optional<UiFeedbackIntent>>(UiErrors::FeedbackLifecycleUnavailable);
        if (storage_->count == 0)
            return Result<std::optional<UiFeedbackIntent>>::Success(std::nullopt);
        UiFeedbackIntent intent = std::move(storage_->intents[storage_->head]);
        storage_->intents[storage_->head] = {};
        storage_->head = (storage_->head + 1) % storage_->intents.size();
        --storage_->count;
        return Result<std::optional<UiFeedbackIntent>>::Success(std::move(intent));
    }

    /** @copydoc UiFeedbackQueue::DeliverNext */
    Result<std::optional<UiFeedbackRealization>> UiFeedbackQueue::DeliverNext(UiFeedbackRealizer &realizer) {
        auto next = TryDequeue();
        if (next.HasError())
            return Result<std::optional<UiFeedbackRealization>>::Failure(next.ErrorValue());
        if (!next.Value().has_value())
            return Result<std::optional<UiFeedbackRealization>>::Success(std::nullopt);
        storage_->delivering = true;
        try {
            auto realized = realizer.Realize(*next.Value());
            storage_->delivering = false;
            if (realized.HasError())
                return Result<std::optional<UiFeedbackRealization>>::Failure(realized.ErrorValue());
            if (!realized.Value().IsValid())
                return Failure<std::optional<UiFeedbackRealization>>(UiErrors::FeedbackConsumerFailed);
            return Result<std::optional<UiFeedbackRealization>>::Success(std::optional<UiFeedbackRealization>{std::move(realized).Value()});
        } catch (...) {  // NOSONAR(cpp:S2738) The optional host callback boundary must contain arbitrary provider exceptions.
            storage_->delivering = false;
            return Failure<std::optional<UiFeedbackRealization>>(UiErrors::FeedbackConsumerFailed);
        }
    }

    /** @copydoc UiFeedbackQueue::BeginRetirement */
    Result<void> UiFeedbackQueue::BeginRetirement() {
        if (!storage_ || storage_->state == UiFeedbackQueueState::Stopped || storage_->delivering)
            return Result<void>::Failure(MakeError(UiErrors::FeedbackLifecycleUnavailable));
        storage_->state = UiFeedbackQueueState::Retiring;
        storage_->count = 0;
        storage_->head = 0;
        return Result<void>::Success();
    }

    /** @copydoc UiFeedbackQueue::Shutdown */
    void UiFeedbackQueue::Shutdown() noexcept {
        if (!storage_)
            return;
        storage_->state = UiFeedbackQueueState::Stopped;
        storage_->count = 0;
        storage_->head = 0;
    }

    /** @copydoc UiFeedbackQueue::State */
    UiFeedbackQueueState UiFeedbackQueue::State() const noexcept {
        return storage_ ? storage_->state : UiFeedbackQueueState::Stopped;
    }

    /** @copydoc UiFeedbackQueue::QueuedCount */
    std::size_t UiFeedbackQueue::QueuedCount() const noexcept {
        return storage_ ? storage_->count : 0;
    }
}  // namespace Horo::Runtime::Ui
