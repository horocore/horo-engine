#include "Horo/Runtime/Ui/UiPointerCapture.h"

#include "Horo/Runtime/Ui/UiErrors.h"

#include <limits>
#include <new>
#include <utility>

namespace Horo::Runtime::Ui {
    namespace {
        template <typename T> [[nodiscard]] Result<T> Failure(const ErrorCodeDescriptor &descriptor) {
            return Result<T>::Failure(MakeError(descriptor));
        }

        [[nodiscard]] Result<void> Failure(const ErrorCodeDescriptor &descriptor) {
            return Result<void>::Failure(MakeError(descriptor));
        }

        [[nodiscard]] bool IsKnownButton(const UiPointerButton button) noexcept {
            return static_cast<std::uint8_t>(button) < static_cast<std::uint8_t>(UiPointerButton::Count);
        }

        [[nodiscard]] bool IsKnownCancellationReason(const UiPointerCaptureCancellationReason reason) noexcept {
            return static_cast<std::uint8_t>(reason) < static_cast<std::uint8_t>(UiPointerCaptureCancellationReason::Count);
        }

        [[nodiscard]] bool SameOwner(const RuntimeUiInstanceId instance, const UiCanvasInstanceId canvas, const UiRenderViewId view,
                                     const RuntimeUiInputContextId context) noexcept {
            return instance.IsValid() && canvas.IsValid() && view.IsValid() && context.IsValid() &&
                   instance.ownership == canvas.ownership && instance.ownership == view.ownership &&
                   instance.ownership == context.ownership;
        }

        [[nodiscard]] bool IsValidRoute(const UiEventRoute &route) noexcept {
            if (!route.instance.IsValid() || !route.canvas.IsValid() || !route.document.IsValid() || !route.tree.IsValid() ||
                !route.interaction.IsValid() || !route.target.IsValid() || route.instance.ownership != route.canvas.ownership ||
                route.instance.ownership != route.target.ownership)
                return false;
            if (route.modalRoot.has_value() && (!route.modalRoot->IsValid() || route.modalRoot->ownership != route.instance.ownership))
                return false;
            return true;
        }

        [[nodiscard]] bool IsValidCaptureOwner(const UiPointerCaptureRequest &request) noexcept {
            return request.context.IsValid() && request.view.IsValid() && IsValidRoute(request.route) &&
                   SameOwner(request.route.instance, request.route.canvas, request.view, request.context);
        }

        [[nodiscard]] bool IsWithinModalRoot(const UiElementTree &tree, const UiEventRoute &route) {
            if (!route.modalRoot.has_value())
                return true;

            UiElementHandle current = route.target;
            for (std::uint32_t depth = 0; depth < MaximumUiTreeDepth && current.IsValid(); ++depth) {
                const auto record = tree.Get(current);
                if (record.HasError())
                    return false;
                if (current == *route.modalRoot)
                    return true;
                current = record.Value().parent;
            }
            return false;
        }
    }  // namespace

    namespace Detail {
        enum class CaptureSlotState : std::uint8_t {
            Free,
            Active,
            Cancelled,
            Retired,
        };

        struct CaptureSlot final {
            UiPointerCaptureRequest request;
            std::uint64_t generation{1};
            UiPointerCaptureCancellationReason cancellation{UiPointerCaptureCancellationReason::Count};
            CaptureSlotState state{CaptureSlotState::Free};
        };

        struct UiPointerCaptureStorage final {
            explicit UiPointerCaptureStorage(const UiPointerCaptureStoreDescriptor &source)
                : ownership(source.ownership), slots(source.maximumCaptures) {}

            UiOwnershipGeneration ownership;
            UiPointerCaptureStoreState lifecycle{UiPointerCaptureStoreState::Active};
            std::vector<CaptureSlot> slots;

            [[nodiscard]] bool Matches(const std::uint32_t slot, const std::uint64_t generation) const noexcept {
                return slot < slots.size() && slots[slot].generation == generation && slots[slot].state != CaptureSlotState::Free &&
                       slots[slot].state != CaptureSlotState::Retired;
            }

            [[nodiscard]] UiPointerCaptureState State(const std::uint32_t slot, const std::uint64_t generation) const noexcept {
                if (!Matches(slot, generation))
                    return UiPointerCaptureState::Inactive;
                return slots[slot].state == CaptureSlotState::Active ? UiPointerCaptureState::Active : UiPointerCaptureState::Cancelled;
            }

            [[nodiscard]] std::optional<UiPointerCaptureCancellationReason> Cancellation(const std::uint32_t slot,
                                                                                         const std::uint64_t generation) const noexcept {
                if (State(slot, generation) != UiPointerCaptureState::Cancelled)
                    return std::nullopt;
                return slots[slot].cancellation;
            }

            void Release(const std::uint32_t slot, const std::uint64_t generation) noexcept {
                if (!Matches(slot, generation))
                    return;
                auto &entry = slots[slot];
                entry.request = {};
                entry.cancellation = UiPointerCaptureCancellationReason::Count;
                if (entry.generation == std::numeric_limits<std::uint64_t>::max()) {
                    entry.state = CaptureSlotState::Retired;
                    return;
                }
                ++entry.generation;
                entry.state = CaptureSlotState::Free;
            }

            void Cancel(CaptureSlot &entry, const UiPointerCaptureCancellationReason reason) noexcept {
                if (entry.state != CaptureSlotState::Active)
                    return;
                entry.cancellation = reason;
                entry.state = CaptureSlotState::Cancelled;
            }

            template <typename Predicate>
            [[nodiscard]] std::uint32_t CancelMatching(Predicate predicate, const UiPointerCaptureCancellationReason reason) noexcept {
                std::uint32_t cancelled{};
                for (CaptureSlot &entry : slots) {
                    if (entry.state == CaptureSlotState::Active && predicate(entry)) {
                        Cancel(entry, reason);
                        ++cancelled;
                    }
                }
                return cancelled;
            }

            [[nodiscard]] std::uint32_t ActiveCount() const noexcept {
                std::uint32_t count{};
                for (const CaptureSlot &entry : slots)
                    if (entry.state == CaptureSlotState::Active)
                        ++count;
                return count;
            }

            [[nodiscard]] bool IsDrained() const noexcept {
                for (const CaptureSlot &entry : slots)
                    if (entry.state == CaptureSlotState::Active || entry.state == CaptureSlotState::Cancelled)
                        return false;
                return true;
            }
        };
    }  // namespace Detail

    /** @copydoc UiPointerId::Create */
    Result<UiPointerId> UiPointerId::Create(const std::uint32_t value) {
        if (value == 0)
            return Failure<UiPointerId>(UiErrors::PointerCaptureInvalid);
        return Result<UiPointerId>::Success(UiPointerId{value});
    }

    /** @copydoc UiPointerCaptureContext::IsValid */
    bool UiPointerCaptureContext::IsValid() const noexcept {
        return context.IsValid() && view.IsValid() && instance.IsValid() && canvas.IsValid() && document.IsValid() && tree.IsValid() &&
               interaction.IsValid() && SameOwner(instance, canvas, view, context);
    }

    /** @copydoc UiPointerCaptureRequest::IsValid */
    bool UiPointerCaptureRequest::IsValid() const noexcept {
        return pointer.IsValid() && IsKnownButton(button) && IsValidCaptureOwner(*this);
    }

    /** @copydoc UiPointerCaptureStoreDescriptor::IsValid */
    bool UiPointerCaptureStoreDescriptor::IsValid() const noexcept {
        return ownership.IsValid() && maximumCaptures > 0 && maximumCaptures <= MaximumUiPointerCaptures;
    }

    namespace {
        [[nodiscard]] Result<void> ValidateCancellation(const UiPointerCaptureCancellationReason reason) {
            return IsKnownCancellationReason(reason) ? Result<void>::Success() : Failure(UiErrors::PointerCaptureInvalid);
        }

        [[nodiscard]] Result<void> ValidateContextForStore(const RuntimeUiInputContextId context, const UiOwnershipGeneration ownership) {
            if (!context.IsValid())
                return Failure(UiErrors::PointerCaptureInvalid);
            if (context.ownership != ownership)
                return Failure(UiErrors::PointerCaptureSourceStale);
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ValidateInstanceForStore(const RuntimeUiInstanceId instance, const UiOwnershipGeneration ownership) {
            if (!instance.IsValid())
                return Failure(UiErrors::PointerCaptureInvalid);
            if (instance.ownership != ownership)
                return Failure(UiErrors::PointerCaptureSourceStale);
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ValidateCanvasForStore(const UiCanvasInstanceId canvas, const UiOwnershipGeneration ownership) {
            if (!canvas.IsValid())
                return Failure(UiErrors::PointerCaptureInvalid);
            if (canvas.ownership != ownership)
                return Failure(UiErrors::PointerCaptureSourceStale);
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ValidateViewForStore(const UiRenderViewId view, const UiOwnershipGeneration ownership) {
            if (!view.IsValid())
                return Failure(UiErrors::PointerCaptureInvalid);
            if (view.ownership != ownership)
                return Failure(UiErrors::PointerCaptureSourceStale);
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ValidateStoreOperation(const std::shared_ptr<Detail::UiPointerCaptureStorage> &storage) {
            if (!storage || storage->lifecycle == UiPointerCaptureStoreState::Stopped)
                return Failure(UiErrors::PointerCaptureLifecycleUnavailable);
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ValidateCaptureSource(const UiPointerCaptureStoreDescriptor &descriptor,
                                                         const UiPointerCaptureRequest &request, const UiElementTree &tree,
                                                         const UiPresentedInteractionState &presented) {
            if (request.route.instance.ownership != descriptor.ownership || request.context.ownership != descriptor.ownership)
                return Failure(UiErrors::PointerCaptureSourceStale);
            if (tree.State() != UiElementTreeState::Active || tree.Instance() != request.route.instance ||
                tree.Canvas() != request.route.canvas || tree.SourceDocument() != request.route.document ||
                tree.Revision() != request.route.tree)
                return Failure(UiErrors::PointerCaptureSourceStale);
            if (tree.Get(request.route.target).HasError() || !IsWithinModalRoot(tree, request.route))
                return Failure(UiErrors::PointerCaptureSourceStale);
            if (presented.View() != request.view || presented.Canvas() != request.route.canvas)
                return Failure(UiErrors::PointerCaptureSourceStale);
            if (presented.LastPresentedInteraction() != request.route.interaction)
                return Failure(UiErrors::PointerCaptureInteractionStale);
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc UiPointerCaptureToken::UiPointerCaptureToken */
    UiPointerCaptureToken::UiPointerCaptureToken(std::shared_ptr<Detail::UiPointerCaptureStorage> storage, const std::uint32_t slot,
                                                 const std::uint64_t generation, UiPointerCaptureRequest request) noexcept
        : storage_(std::move(storage)), slot_(slot), generation_(generation), request_(std::move(request)) {}

    /** @copydoc UiPointerCaptureToken::~UiPointerCaptureToken */
    UiPointerCaptureToken::~UiPointerCaptureToken() {
        Release();
    }

    /** @copydoc UiPointerCaptureToken::UiPointerCaptureToken */
    UiPointerCaptureToken::UiPointerCaptureToken(UiPointerCaptureToken &&other) noexcept
        : storage_(std::exchange(other.storage_, nullptr)), slot_(std::exchange(other.slot_, 0)),
          generation_(std::exchange(other.generation_, 0)), request_(std::exchange(other.request_, {})) {}

    /** @copydoc UiPointerCaptureToken::operator= */
    UiPointerCaptureToken &UiPointerCaptureToken::operator=(UiPointerCaptureToken &&other) noexcept {
        if (this != &other) {
            Release();
            storage_ = std::exchange(other.storage_, nullptr);
            slot_ = std::exchange(other.slot_, 0);
            generation_ = std::exchange(other.generation_, 0);
            request_ = std::exchange(other.request_, {});
        }
        return *this;
    }

    /** @copydoc UiPointerCaptureToken::Release */
    void UiPointerCaptureToken::Release() noexcept {
        if (storage_)
            storage_->Release(slot_, generation_);
        storage_.reset();
        slot_ = 0;
        generation_ = 0;
        request_ = {};
    }

    /** @copydoc UiPointerCaptureToken::IsActive */
    bool UiPointerCaptureToken::IsActive() const noexcept {
        return State() == UiPointerCaptureState::Active;
    }

    /** @copydoc UiPointerCaptureToken::State */
    UiPointerCaptureState UiPointerCaptureToken::State() const noexcept {
        return storage_ ? storage_->State(slot_, generation_) : UiPointerCaptureState::Inactive;
    }

    /** @copydoc UiPointerCaptureToken::CancellationReason */
    std::optional<UiPointerCaptureCancellationReason> UiPointerCaptureToken::CancellationReason() const noexcept {
        return storage_ ? storage_->Cancellation(slot_, generation_) : std::nullopt;
    }

    /** @copydoc UiPointerCaptureToken::Request */
    const UiPointerCaptureRequest &UiPointerCaptureToken::Request() const noexcept {
        return request_;
    }

    /** @copydoc UiPointerCaptureStore::Create */
    Result<UiPointerCaptureStore> UiPointerCaptureStore::Create(const UiPointerCaptureStoreDescriptor &descriptor) {
        if (!descriptor.IsValid())
            return Failure<UiPointerCaptureStore>(UiErrors::PointerCaptureInvalid);
        try {
            return Result<UiPointerCaptureStore>::Success(
                UiPointerCaptureStore{std::make_shared<Detail::UiPointerCaptureStorage>(descriptor)});
        } catch (const std::bad_alloc &) {
            return Failure<UiPointerCaptureStore>(UiErrors::PointerCaptureCapacityExceeded);
        }
    }

    /** @copydoc UiPointerCaptureStore::UiPointerCaptureStore */
    UiPointerCaptureStore::UiPointerCaptureStore(std::shared_ptr<Detail::UiPointerCaptureStorage> storage) noexcept
        : storage_(std::move(storage)) {}

    /** @copydoc UiPointerCaptureStore::~UiPointerCaptureStore */
    UiPointerCaptureStore::~UiPointerCaptureStore() {
        Shutdown();
    }

    /** @copydoc UiPointerCaptureStore::UiPointerCaptureStore */
    UiPointerCaptureStore::UiPointerCaptureStore(UiPointerCaptureStore &&) noexcept = default;

    /** @copydoc UiPointerCaptureStore::operator= */
    UiPointerCaptureStore &UiPointerCaptureStore::operator=(UiPointerCaptureStore &&other) noexcept {
        if (this != &other) {
            Shutdown();
            storage_ = std::exchange(other.storage_, nullptr);
        }
        return *this;
    }

    /** @copydoc UiPointerCaptureStore::Capture */
    Result<UiPointerCaptureToken> UiPointerCaptureStore::Capture(const UiPointerCaptureRequest &request, const UiElementTree &tree,
                                                                 const UiPresentedInteractionState &presented) {
        if (const auto available = ValidateStoreOperation(storage_); available.HasError())
            return Result<UiPointerCaptureToken>::Failure(available.ErrorValue());
        if (storage_->lifecycle != UiPointerCaptureStoreState::Active)
            return Failure<UiPointerCaptureToken>(UiErrors::PointerCaptureLifecycleUnavailable);
        if (!request.IsValid())
            return Failure<UiPointerCaptureToken>(UiErrors::PointerCaptureInvalid);
        const UiPointerCaptureStoreDescriptor descriptor{storage_->ownership, static_cast<std::uint32_t>(storage_->slots.size())};
        if (const auto source = ValidateCaptureSource(descriptor, request, tree, presented); source.HasError())
            return Result<UiPointerCaptureToken>::Failure(source.ErrorValue());

        const std::uint32_t availableSlot = static_cast<std::uint32_t>(storage_->slots.size());
        std::uint32_t freeSlot = availableSlot;
        for (std::uint32_t index = 0; index < storage_->slots.size(); ++index) {
            const Detail::CaptureSlot &entry = storage_->slots[index];
            if (entry.state == Detail::CaptureSlotState::Active && entry.request.context == request.context &&
                entry.request.pointer == request.pointer)
                return Failure<UiPointerCaptureToken>(UiErrors::PointerCaptureBusy);
            if (freeSlot == availableSlot && entry.state == Detail::CaptureSlotState::Free)
                freeSlot = index;
        }

        if (freeSlot == availableSlot)
            return Failure<UiPointerCaptureToken>(UiErrors::PointerCaptureCapacityExceeded);
        Detail::CaptureSlot &entry = storage_->slots[freeSlot];
        entry.request = request;
        entry.cancellation = UiPointerCaptureCancellationReason::Count;
        entry.state = Detail::CaptureSlotState::Active;
        return Result<UiPointerCaptureToken>::Success(UiPointerCaptureToken{storage_, freeSlot, entry.generation, request});
    }

    /** @copydoc UiPointerCaptureStore::ReleasePointer */
    Result<bool> UiPointerCaptureStore::ReleasePointer(const RuntimeUiInputContextId context, const UiPointerId pointer,
                                                       const UiPointerButton button) {
        if (const auto available = ValidateStoreOperation(storage_); available.HasError())
            return Failure<bool>(UiErrors::PointerCaptureLifecycleUnavailable);
        if (const auto valid = ValidateContextForStore(context, storage_->ownership); valid.HasError())
            return Result<bool>::Failure(valid.ErrorValue());
        if (!pointer.IsValid() || !IsKnownButton(button))
            return Failure<bool>(UiErrors::PointerCaptureInvalid);
        for (std::uint32_t index = 0; index < storage_->slots.size(); ++index) {
            auto &entry = storage_->slots[index];
            if (entry.state == Detail::CaptureSlotState::Active && entry.request.context == context && entry.request.pointer == pointer &&
                entry.request.button == button) {
                storage_->Release(index, entry.generation);
                return Result<bool>::Success(true);
            }
        }
        return Result<bool>::Success(false);
    }

    /** @copydoc UiPointerCaptureStore::CancelPointer */
    Result<std::uint32_t> UiPointerCaptureStore::CancelPointer(const RuntimeUiInputContextId context, const UiPointerId pointer,
                                                               const UiPointerCaptureCancellationReason reason) {
        if (const auto available = ValidateStoreOperation(storage_); available.HasError())
            return Failure<std::uint32_t>(UiErrors::PointerCaptureLifecycleUnavailable);
        if (const auto valid = ValidateContextForStore(context, storage_->ownership); valid.HasError())
            return Result<std::uint32_t>::Failure(valid.ErrorValue());
        if (!pointer.IsValid())
            return Failure<std::uint32_t>(UiErrors::PointerCaptureInvalid);
        if (const auto valid = ValidateCancellation(reason); valid.HasError())
            return Result<std::uint32_t>::Failure(valid.ErrorValue());
        return Result<std::uint32_t>::Success(storage_->CancelMatching([context, pointer](const Detail::CaptureSlot &entry) {
            return entry.request.context == context && entry.request.pointer == pointer;
        }, reason));
    }

    /** @copydoc UiPointerCaptureStore::CancelContext */
    Result<std::uint32_t> UiPointerCaptureStore::CancelContext(const RuntimeUiInputContextId context,
                                                               const UiPointerCaptureCancellationReason reason) {
        if (const auto available = ValidateStoreOperation(storage_); available.HasError())
            return Failure<std::uint32_t>(UiErrors::PointerCaptureLifecycleUnavailable);
        if (const auto valid = ValidateContextForStore(context, storage_->ownership); valid.HasError())
            return Result<std::uint32_t>::Failure(valid.ErrorValue());
        if (const auto valid = ValidateCancellation(reason); valid.HasError())
            return Result<std::uint32_t>::Failure(valid.ErrorValue());
        return Result<std::uint32_t>::Success(storage_->CancelMatching([context](const Detail::CaptureSlot &entry) {
            return entry.request.context == context;
        }, reason));
    }

    /** @copydoc UiPointerCaptureStore::CancelInstance */
    Result<std::uint32_t> UiPointerCaptureStore::CancelInstance(const RuntimeUiInstanceId instance,
                                                                const UiPointerCaptureCancellationReason reason) {
        if (const auto available = ValidateStoreOperation(storage_); available.HasError())
            return Failure<std::uint32_t>(UiErrors::PointerCaptureLifecycleUnavailable);
        if (const auto valid = ValidateInstanceForStore(instance, storage_->ownership); valid.HasError())
            return Result<std::uint32_t>::Failure(valid.ErrorValue());
        if (const auto valid = ValidateCancellation(reason); valid.HasError())
            return Result<std::uint32_t>::Failure(valid.ErrorValue());
        return Result<std::uint32_t>::Success(storage_->CancelMatching([instance](const Detail::CaptureSlot &entry) {
            return entry.request.route.instance == instance;
        }, reason));
    }

    /** @copydoc UiPointerCaptureStore::CancelCanvas */
    Result<std::uint32_t> UiPointerCaptureStore::CancelCanvas(const UiCanvasInstanceId canvas,
                                                              const UiPointerCaptureCancellationReason reason) {
        if (const auto available = ValidateStoreOperation(storage_); available.HasError())
            return Failure<std::uint32_t>(UiErrors::PointerCaptureLifecycleUnavailable);
        if (const auto valid = ValidateCanvasForStore(canvas, storage_->ownership); valid.HasError())
            return Result<std::uint32_t>::Failure(valid.ErrorValue());
        if (const auto valid = ValidateCancellation(reason); valid.HasError())
            return Result<std::uint32_t>::Failure(valid.ErrorValue());
        return Result<std::uint32_t>::Success(storage_->CancelMatching([canvas](const Detail::CaptureSlot &entry) {
            return entry.request.route.canvas == canvas;
        }, reason));
    }

    /** @copydoc UiPointerCaptureStore::CancelView */
    Result<std::uint32_t> UiPointerCaptureStore::CancelView(const UiRenderViewId view, const UiPointerCaptureCancellationReason reason) {
        if (const auto available = ValidateStoreOperation(storage_); available.HasError())
            return Failure<std::uint32_t>(UiErrors::PointerCaptureLifecycleUnavailable);
        if (const auto valid = ValidateViewForStore(view, storage_->ownership); valid.HasError())
            return Result<std::uint32_t>::Failure(valid.ErrorValue());
        if (const auto valid = ValidateCancellation(reason); valid.HasError())
            return Result<std::uint32_t>::Failure(valid.ErrorValue());
        return Result<std::uint32_t>::Success(storage_->CancelMatching([view](const Detail::CaptureSlot &entry) {
            return entry.request.view == view;
        }, reason));
    }

    /** @copydoc UiPointerCaptureStore::CancelAll */
    Result<std::uint32_t> UiPointerCaptureStore::CancelAll(const UiPointerCaptureCancellationReason reason) {
        if (const auto available = ValidateStoreOperation(storage_); available.HasError())
            return Failure<std::uint32_t>(UiErrors::PointerCaptureLifecycleUnavailable);
        if (const auto valid = ValidateCancellation(reason); valid.HasError())
            return Result<std::uint32_t>::Failure(valid.ErrorValue());
        return Result<std::uint32_t>::Success(storage_->CancelMatching([](const Detail::CaptureSlot &) {
            return true;
        }, reason));
    }

    /** @copydoc UiPointerCaptureStore::Reconcile */
    Result<std::uint32_t> UiPointerCaptureStore::Reconcile(const UiPointerCaptureContext &current, const UiElementTree &tree,
                                                           const UiPresentedInteractionState &presented) {
        if (const auto available = ValidateStoreOperation(storage_); available.HasError())
            return Failure<std::uint32_t>(UiErrors::PointerCaptureLifecycleUnavailable);
        if (!current.IsValid())
            return Failure<std::uint32_t>(UiErrors::PointerCaptureInvalid);
        if (current.context.ownership != storage_->ownership)
            return Failure<std::uint32_t>(UiErrors::PointerCaptureSourceStale);
        if (tree.State() != UiElementTreeState::Active || tree.Instance() != current.instance || tree.Canvas() != current.canvas ||
            tree.SourceDocument() != current.document || tree.Revision() != current.tree)
            return Failure<std::uint32_t>(UiErrors::PointerCaptureSourceStale);
        if (presented.View() != current.view || presented.Canvas() != current.canvas)
            return Failure<std::uint32_t>(UiErrors::PointerCaptureSourceStale);
        if (presented.LastPresentedInteraction() != current.interaction)
            return Failure<std::uint32_t>(UiErrors::PointerCaptureInteractionStale);

        std::uint32_t cancelled{};
        for (Detail::CaptureSlot &entry : storage_->slots) {
            if (entry.state != Detail::CaptureSlotState::Active || entry.request.context != current.context)
                continue;
            if (entry.request.view != current.view || entry.request.route.instance != current.instance ||
                entry.request.route.canvas != current.canvas || entry.request.route.document != current.document) {
                storage_->Cancel(entry, UiPointerCaptureCancellationReason::RouteReplaced);
            } else if (entry.request.route.tree != current.tree) {
                storage_->Cancel(entry, UiPointerCaptureCancellationReason::Reload);
            } else if (entry.request.route.interaction != current.interaction) {
                storage_->Cancel(entry, UiPointerCaptureCancellationReason::InteractionRevisionLost);
            } else if (tree.Get(entry.request.route.target).HasError()) {
                storage_->Cancel(entry, UiPointerCaptureCancellationReason::TargetDestroyed);
            } else {
                continue;
            }
            ++cancelled;
        }
        return Result<std::uint32_t>::Success(cancelled);
    }

    /** @copydoc UiPointerCaptureStore::BeginRetirement */
    Result<void> UiPointerCaptureStore::BeginRetirement() {
        if (!storage_ || storage_->lifecycle != UiPointerCaptureStoreState::Active)
            return Failure(UiErrors::PointerCaptureLifecycleUnavailable);
        storage_->lifecycle = UiPointerCaptureStoreState::Retiring;
        return Result<void>::Success();
    }

    /** @copydoc UiPointerCaptureStore::Shutdown */
    void UiPointerCaptureStore::Shutdown() noexcept {
        if (!storage_ || storage_->lifecycle == UiPointerCaptureStoreState::Stopped)
            return;
        static_cast<void>(storage_->CancelMatching([](const Detail::CaptureSlot &) {
            return true;
        }, UiPointerCaptureCancellationReason::Shutdown));
        storage_->lifecycle = UiPointerCaptureStoreState::Stopped;
    }

    /** @copydoc UiPointerCaptureStore::State */
    UiPointerCaptureStoreState UiPointerCaptureStore::State() const noexcept {
        return storage_ ? storage_->lifecycle : UiPointerCaptureStoreState::Stopped;
    }

    /** @copydoc UiPointerCaptureStore::IsDrained */
    bool UiPointerCaptureStore::IsDrained() const noexcept {
        return !storage_ || storage_->IsDrained();
    }

    /** @copydoc UiPointerCaptureStore::ActiveCount */
    std::uint32_t UiPointerCaptureStore::ActiveCount() const noexcept {
        return storage_ ? storage_->ActiveCount() : 0;
    }
}  // namespace Horo::Runtime::Ui
