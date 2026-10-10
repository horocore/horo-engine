#include "Horo/Runtime/Ui/UiEventDispatch.h"

#include "Horo/Runtime/Ui/UiErrors.h"

#include <cmath>
#include <new>
#include <optional>
#include <utility>
#include <vector>

namespace Horo::Runtime::Ui {
    namespace {
        /** @brief Creates the owned error used by typed event-dispatch failure results. */
        [[nodiscard]] Error DispatchError(const ErrorCodeDescriptor &descriptor) {
            return MakeError(descriptor);
        }

        /** @brief Checks the closed normalized event vocabulary. */
        [[nodiscard]] bool IsKnown(const UiEventKind kind) noexcept {
            return kind < UiEventKind::Count;
        }

        /** @brief Reports whether one known event carries a logical pointer position. */
        [[nodiscard]] bool IsPointerEvent(const UiEventKind kind) noexcept {
            using enum UiEventKind;
            switch (kind) {
                case PointerMove:
                case PointerPress:
                case PointerRelease:
                    return true;
                case Submit:
                case Cancel:
                case Gesture:
                case Count:
                    return false;
            }
            return false;
        }

        /** @brief Checks the gesture/position payload before route construction. */
        bool ValidPayload(const UiRoutedEvent &event) noexcept {
            return event.kind == UiEventKind::Gesture
                       ? event.gesture.has_value() && event.gesture->IsValid() && event.hasLogicalPosition != event.gesture->accessible
                       : !event.gesture.has_value() && IsPointerEvent(event.kind) == event.hasLogicalPosition;
        }

        /** @brief Restores non-reentrant admission on every handler return or exception path. */
        class DispatchGuard final {
        public:
            explicit DispatchGuard(bool &dispatching) noexcept : dispatching_(dispatching) {
                dispatching_ = true;
            }

            DispatchGuard(const DispatchGuard &) = delete;
            DispatchGuard &operator=(const DispatchGuard &) = delete;
            DispatchGuard(DispatchGuard &&) = delete;
            DispatchGuard &operator=(DispatchGuard &&) = delete;

            ~DispatchGuard() {
                dispatching_ = false;
            }

        private:
            bool &dispatching_;
        };
    }  // namespace

    /** @copydoc UiGestureEvent::IsValid */
    bool UiGestureEvent::IsValid() const noexcept {
        return kind < UiGestureKind::Count && source.IsValid() && (accessible ? pointer == 0 : pointer != 0) && std::isfinite(scale) &&
               scale > 0.0 && std::isfinite(rotation);
    }

    struct UiEventDispatcher::Storage final {
        UiEventDispatcherDescriptor descriptor;
        UiEventDispatcherState state{UiEventDispatcherState::Active};
        std::vector<UiElementHandle> route;
        bool dispatching{};

        explicit Storage(const UiEventDispatcherDescriptor &source) : descriptor(source) {
            route.reserve(source.maximumRouteDepth);
        }

        [[nodiscard]] bool Matches(const UiElementTree &tree, const UiEventRoute &source) const noexcept {
            return source.instance == descriptor.instance && source.canvas == descriptor.canvas && source.document == descriptor.document &&
                   tree.State() == UiElementTreeState::Active && tree.Instance() == descriptor.instance &&
                   tree.Canvas() == descriptor.canvas && tree.SourceDocument() == descriptor.document && source.tree == tree.Revision() &&
                   source.interaction.IsValid();
        }

        [[nodiscard]] Result<void> BuildRoute(const UiElementTree &tree, const UiEventRoute &source) {
            route.clear();
            UiElementHandle current = source.target;
            bool reachedBoundary = !source.modalRoot.has_value();
            while (current.IsValid()) {
                if (route.size() == descriptor.maximumRouteDepth)
                    return Result<void>::Failure(DispatchError(UiErrors::EventDispatchCapacityExceeded));
                const auto record = tree.Get(current);
                if (record.HasError())
                    return Result<void>::Failure(DispatchError(UiErrors::EventDispatchSourceStale));
                route.push_back(current);
                if (source.modalRoot && current == *source.modalRoot) {
                    reachedBoundary = true;
                    break;
                }
                current = record.Value().parent;
            }
            if (!reachedBoundary)
                return Result<void>::Failure(DispatchError(UiErrors::EventDispatchModalBoundaryViolation));
            return Result<void>::Success();
        }

        [[nodiscard]] bool RouteStillCurrent(const UiElementTree &tree, const UiEventRoute &source) const {
            return tree.State() == UiElementTreeState::Active && tree.Revision() == source.tree;
        }

        [[nodiscard]] Result<void> Invoke(const UiElementTree &tree, const UiEventRoute &source, const UiRoutedEvent &event,
                                          UiEventHandler &handler, const UiElementHandle element, const UiEventPhase phase,
                                          UiEventDispatchResult &result) const {
            if (!RouteStillCurrent(tree, source))
                return Result<void>::Failure(DispatchError(UiErrors::EventDispatchRouteInvalidated));
            std::optional<Result<UiEventResponse>> response;
            try {
                response.emplace(handler.Handle(element, phase, event));
            } catch (...) {
                return Result<void>::Failure(DispatchError(UiErrors::EventDispatchHandlerFailed));
            }
            if (response->HasError())
                return Result<void>::Failure(response->ErrorValue());
            ++result.phasesVisited;
            result.handled = result.handled || response->Value().handled;
            result.defaultPrevented = result.defaultPrevented || response->Value().preventDefault;
            result.propagationStopped = response->Value().stopPropagation;
            return RouteStillCurrent(tree, source) ? Result<void>::Success()
                                                   : Result<void>::Failure(DispatchError(UiErrors::EventDispatchRouteInvalidated));
        }

        [[nodiscard]] Result<void> ApplyDefault(const UiElementTree &tree, const UiEventRoute &source, const UiRoutedEvent &event,
                                                UiEventHandler &handler) const {
            if (!RouteStillCurrent(tree, source))
                return Result<void>::Failure(DispatchError(UiErrors::EventDispatchRouteInvalidated));
            try {
                return handler.ApplyDefault(source.target, event);
            } catch (...) {
                return Result<void>::Failure(DispatchError(UiErrors::EventDispatchHandlerFailed));
            }
        }
    };

    /** @copydoc UiEventDispatcherDescriptor::IsValid */
    bool UiEventDispatcherDescriptor::IsValid() const noexcept {
        return instance.IsValid() && canvas.IsValid() && instance.ownership == canvas.ownership && document.IsValid() &&
               maximumRouteDepth > 0 && maximumRouteDepth <= MaximumUiTreeDepth;
    }

    /** @copydoc UiEventDispatcher::Create */
    Result<UiEventDispatcher> UiEventDispatcher::Create(const UiEventDispatcherDescriptor &descriptor) {
        if (!descriptor.IsValid())
            return Result<UiEventDispatcher>::Failure(DispatchError(UiErrors::EventDispatchInvalid));
        try {
            return Result<UiEventDispatcher>::Success(UiEventDispatcher{std::make_unique<Storage>(descriptor)});
        } catch (const std::bad_alloc &) {
            return Result<UiEventDispatcher>::Failure(DispatchError(UiErrors::CapacityExceeded));
        }
    }

    /** @copydoc UiEventDispatcher::UiEventDispatcher */
    UiEventDispatcher::UiEventDispatcher(std::unique_ptr<Storage> storage) noexcept : storage_(std::move(storage)) {}

    /** @copydoc UiEventDispatcher::~UiEventDispatcher */
    UiEventDispatcher::~UiEventDispatcher() {
        Shutdown();
    }

    /** @copydoc UiEventDispatcher::UiEventDispatcher */
    UiEventDispatcher::UiEventDispatcher(UiEventDispatcher &&) noexcept = default;

    /** @copydoc UiEventDispatcher::operator= */
    UiEventDispatcher &UiEventDispatcher::operator=(UiEventDispatcher &&) noexcept = default;

    /** @copydoc UiEventDispatcher::Dispatch */
    Result<UiEventDispatchResult> UiEventDispatcher::Dispatch(const UiElementTree &tree, const UiEventRoute &route,
                                                              const UiRoutedEvent &event, UiEventHandler &handler) {
        if (!storage_ || storage_->state != UiEventDispatcherState::Active)
            return Result<UiEventDispatchResult>::Failure(DispatchError(UiErrors::EventDispatchLifecycleUnavailable));
        if (storage_->dispatching)
            return Result<UiEventDispatchResult>::Failure(DispatchError(UiErrors::EventDispatchReentrant));
        const bool eventValid = IsKnown(event.kind) && event.sequence != 0 && ValidPayload(event);
        if (const bool routeValid = route.target.IsValid() && (!route.modalRoot || route.modalRoot->IsValid()); !eventValid || !routeValid)
            return Result<UiEventDispatchResult>::Failure(DispatchError(UiErrors::EventDispatchInvalid));
        if (!storage_->Matches(tree, route))
            return Result<UiEventDispatchResult>::Failure(DispatchError(UiErrors::EventDispatchSourceStale));
        if (event.gesture && (event.gesture->source.ownership != route.instance.ownership || tree.Get(event.gesture->source).HasError()))
            return Result<UiEventDispatchResult>::Failure(DispatchError(UiErrors::EventDispatchSourceStale));
        if (const auto built = storage_->BuildRoute(tree, route); built.HasError())
            return Result<UiEventDispatchResult>::Failure(built.ErrorValue());

        DispatchGuard guard{storage_->dispatching};
        UiEventDispatchResult result;
        for (std::size_t index = storage_->route.size(); index > 1 && !result.propagationStopped; --index)
            if (const auto invoked =
                    storage_->Invoke(tree, route, event, handler, storage_->route[index - 1], UiEventPhase::Capture, result);
                invoked.HasError())
                return Result<UiEventDispatchResult>::Failure(invoked.ErrorValue());
        if (!result.propagationStopped)
            if (const auto invoked = storage_->Invoke(tree, route, event, handler, storage_->route.front(), UiEventPhase::Target, result);
                invoked.HasError())
                return Result<UiEventDispatchResult>::Failure(invoked.ErrorValue());
        for (std::size_t index = 1; index < storage_->route.size() && !result.propagationStopped; ++index)
            if (const auto invoked = storage_->Invoke(tree, route, event, handler, storage_->route[index], UiEventPhase::Bubble, result);
                invoked.HasError())
                return Result<UiEventDispatchResult>::Failure(invoked.ErrorValue());

        if (!result.defaultPrevented) {
            if (const auto applied = storage_->ApplyDefault(tree, route, event, handler); applied.HasError())
                return Result<UiEventDispatchResult>::Failure(applied.ErrorValue());
            result.defaultApplied = true;
        }
        return Result<UiEventDispatchResult>::Success(result);
    }

    /** @copydoc UiEventDispatcher::BeginRetirement */
    Result<void> UiEventDispatcher::BeginRetirement() {
        if (!storage_ || storage_->state != UiEventDispatcherState::Active || storage_->dispatching)
            return Result<void>::Failure(DispatchError(UiErrors::EventDispatchLifecycleUnavailable));
        storage_->state = UiEventDispatcherState::Retiring;
        return Result<void>::Success();
    }

    /** @copydoc UiEventDispatcher::Shutdown */
    void UiEventDispatcher::Shutdown() noexcept {
        if (!storage_ || storage_->dispatching)
            return;
        storage_->state = UiEventDispatcherState::Stopped;
        storage_->route.clear();
    }

    /** @copydoc UiEventDispatcher::State */
    UiEventDispatcherState UiEventDispatcher::State() const noexcept {
        return storage_ ? storage_->state : UiEventDispatcherState::Stopped;
    }
}  // namespace Horo::Runtime::Ui
