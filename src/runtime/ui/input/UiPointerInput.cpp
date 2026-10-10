#include "Horo/Runtime/Ui/UiPointerInput.h"

#include "Horo/Runtime/Ui/UiErrors.h"

#include <algorithm>
#include <cmath>
#include <exception>
#include <limits>
#include <new>
#include <utility>

namespace Horo::Runtime::Ui {
    namespace {
        /** @brief Reserves the stable synchronous adapter until all callback borrows have returned. */
        class PointerInputGuard final {
        public:
            explicit PointerInputGuard(bool &active) noexcept : active_(active) {
                active = true;
            }

            ~PointerInputGuard() {
                active_ = false;
            }

            PointerInputGuard(const PointerInputGuard &) = delete;
            PointerInputGuard &operator=(const PointerInputGuard &) = delete;
            PointerInputGuard(PointerInputGuard &&) = delete;
            PointerInputGuard &operator=(PointerInputGuard &&) = delete;

        private:
            bool &active_;
        };

        /** @brief Caps every effective replacement before any action resolution scans its bindings. */
        bool BoundedProfile(const Input::InputRouter &router) noexcept {
            return router.Actions().size() <= 512 && router.Profile().overrides.size() <= 512 &&
                   std::ranges::all_of(router.Actions(), [](const auto &action) {
                return action.defaultBindings.size() <= 32;
            }) && std::ranges::all_of(router.Profile().overrides, [](const auto &entry) {
                return entry.bindings.size() <= 32;
            });
        }

        /** @brief Frozen synchronous committed-frame lineage plus borrowed local cancellation flags. */
        struct InputRouteFence final {
            const bool &stopped;
            const bool &cleanup;
            Input::FrameNumber frame;
            Input::InputRoutingState generation;
        };

        /** @brief Revalidates borrowed Input authority on both sides of every application callback.
         * @details The aggregate bridge invokes this observer before touching real control defaults. Context cancellation,
         *          reconfiguration and adapter shutdown cannot authorize another contact transition after that callback.
         */
        class InputGestureRoute final : public UiEventHandler {
        public:
            InputGestureRoute(UiEventHandler &observer, const Input::InputRouter &router, const Input::InputContextToken &context,
                              const InputRouteFence &fence)
                : observer_(observer), router_(router), context_(context), fence_(fence) {}

            Result<UiEventResponse> Handle(const UiElementHandle element, const UiEventPhase phase, const UiRoutedEvent &event) override {
                if (!Live())
                    return Result<UiEventResponse>::Failure(MakeError(UiErrors::EventDispatchRouteInvalidated));
                auto response = observer_.Handle(element, phase, event);
                if (!Live())
                    return Result<UiEventResponse>::Failure(MakeError(UiErrors::EventDispatchRouteInvalidated));
                return response;
            }

            Result<void> ApplyDefault(const UiElementHandle element, const UiRoutedEvent &event) override {
                if (!Live())
                    return Result<void>::Failure(MakeError(UiErrors::EventDispatchRouteInvalidated));
                auto applied = observer_.ApplyDefault(element, event);
                if (!Live())
                    return Result<void>::Failure(MakeError(UiErrors::EventDispatchRouteInvalidated));
                return applied;
            }

        private:
            [[nodiscard]] bool Live() const noexcept {
                const auto routing = router_.RoutingState(context_);
                const auto &snapshot = router_.Snapshot();
                return !fence_.stopped && !fence_.cleanup && router_.IsContextActive(context_) &&
                       routing.contextIdentity == fence_.generation.contextIdentity &&
                       routing.configurationRevision == fence_.generation.configurationRevision &&
                       routing.assignmentRevision == fence_.generation.assignmentRevision && snapshot.frame == fence_.frame &&
                       snapshot.window.focused && snapshot.window.pointerDeviceAvailable && !snapshot.touchOverflow;
            }

            UiEventHandler &observer_;
            const Input::InputRouter &router_;
            const Input::InputContextToken &context_;
            const InputRouteFence fence_;
        };
    }  // namespace

    /** @copydoc DefaultUiPointerActions */
    std::vector<Input::ActionDescriptor> DefaultUiPointerActions(const Input::InputContextId &context) {
        const std::array names{"ui.pointer.activate", "ui.pointer.context", "ui.pointer.pick", "ui.pointer.cancel"};
        using enum Input::Key;
        const std::array keys{Enter, F10, Space, Escape};
        std::vector<Input::ActionDescriptor> result;
        result.reserve(UiPointerActionCount);
        for (std::size_t index = 0; index < names.size(); ++index) {
            Input::InputBinding binding;
            binding.key = keys[index];
            if (index == 1)
                binding.requiredModifiers.shift = true;
            result.push_back({Input::ActionId{names[index]}, Input::ActionValueType::Digital, context, true, {binding}});
        }
        return result;
    }

    /** @copydoc UiPointerInput::Create */
    Result<std::unique_ptr<UiPointerInput>> UiPointerInput::Create(const Input::InputRouter &router,
                                                                   const Input::InputContextToken &context,
                                                                   const UiPointerInteractionDescriptor &descriptor,
                                                                   const std::span<const UiPointerTargetPolicy> targets,
                                                                   const std::array<Input::ActionId, UiPointerActionCount> &actions) {
        const auto routing = router.RoutingState(context);
        if (!routing.contextIdentity || !routing.configurationRevision || !routing.assignmentRevision || !routing.WithinLimits(64, 16) ||
            !BoundedProfile(router))
            return Result<std::unique_ptr<UiPointerInput>>::Failure(MakeError(UiErrors::EventDispatchCapacityExceeded));
        for (std::size_t index = 0; index < actions.size(); ++index) {
            if (const auto action = std::ranges::find(router.Actions(), actions[index], &Input::ActionDescriptor::id);
                action == router.Actions().end() || action->valueType != Input::ActionValueType::Digital ||
                !router.ContextMatches(context, action->context) || action->defaultBindings.size() > 32 ||
                std::find(actions.begin(), actions.begin() + index, actions[index]) != actions.begin() + index)
                return Result<std::unique_ptr<UiPointerInput>>::Failure(MakeError(UiErrors::EventDispatchInvalid));
        }
        auto interaction = UiPointerInteraction::Create(descriptor, targets);
        if (interaction.HasError())
            return Result<std::unique_ptr<UiPointerInput>>::Failure(interaction.ErrorValue());
        auto dispatcher =
            UiEventDispatcher::Create({descriptor.owner.instance, descriptor.owner.canvas, descriptor.owner.document, MaximumUiTreeDepth});
        if (dispatcher.HasError())
            return Result<std::unique_ptr<UiPointerInput>>::Failure(dispatcher.ErrorValue());
        try {
            auto result = std::make_unique<UiPointerInput>(ConstructionKey{}, std::move(interaction).Value());
            result->router_ = &router;
            result->context_ = routing.contextIdentity;
            result->configurationRevision_ = routing.configurationRevision;
            result->assignmentRevision_ = routing.assignmentRevision;
            result->actions_ = actions;
            result->dispatcher_.emplace(std::move(dispatcher).Value());
            return Result<std::unique_ptr<UiPointerInput>>::Success(std::move(result));
        } catch (const std::bad_alloc &) {
            return Result<std::unique_ptr<UiPointerInput>>::Failure(MakeError(UiErrors::EventDispatchCapacityExceeded));
        }
    }

    /** @copydoc UiPointerInput::UiPointerInput */
    UiPointerInput::UiPointerInput(ConstructionKey, UiPointerInteraction interaction) : interaction_(std::move(interaction)) {}

    /** @copydoc UiPointerInput::~UiPointerInput */
    UiPointerInput::~UiPointerInput() {
        if (pumping_)
            std::terminate();
        Shutdown();
    }

    /** @copydoc UiPointerInput::OnInputCaptureCancelled */
    void UiPointerInput::OnInputCaptureCancelled(const Input::CaptureCancellationReason) noexcept {
        mouseCapture_.Release();
        // Even Released means the router observed a terminal edge before this adapter delivered it to the UI owner.
        cleanupPending_ = true;
        mouseButton_.reset();
    }

    /** @copydoc UiPointerInput::Defaults */
    std::span<const UiControlDefaultAction> UiPointerInput::Defaults() const noexcept {
        return std::span{defaults_}.first(defaultCount_);
    }

    /** @copydoc UiPointerInput::Suspend */
    Result<void> UiPointerInput::Suspend(UiPointerInteractionHost &owner, std::uint64_t &nextSequence) {
        mouseCapture_.Release();
        mouseButton_.reset();
        contacts_.fill({});
        cleanupPending_ = false;
        return owner.CancelPointers(interaction_, nextSequence);
    }

    /** @copydoc UiPointerInput::Shutdown */
    void UiPointerInput::Shutdown() noexcept {
        stopped_ = true;
        mouseCapture_.Release();
        mouseButton_.reset();
        contacts_.fill({});
        interaction_.Shutdown();
        if (dispatcher_)
            dispatcher_->Shutdown();
        defaultCount_ = 0;
    }

    /** @copydoc UiPointerInput::Rebind */
    Result<void> UiPointerInput::Rebind(UiPointerInteractionHost &owner, std::uint64_t &nextSequence,
                                        const UiPointerInteractionDescriptor &descriptor,
                                        const std::span<const UiPointerTargetPolicy> targets) {
        if (stopped_)
            return Result<void>::Failure(MakeError(UiErrors::EventDispatchLifecycleUnavailable));
        if (pumping_)
            return Result<void>::Failure(MakeError(UiErrors::EventDispatchReentrant));
        auto next = UiPointerInteraction::Create(descriptor, targets);
        if (next.HasError())
            return Result<void>::Failure(next.ErrorValue());
        auto dispatcher =
            UiEventDispatcher::Create({descriptor.owner.instance, descriptor.owner.canvas, descriptor.owner.document, MaximumUiTreeDepth});
        if (dispatcher.HasError())
            return Result<void>::Failure(dispatcher.ErrorValue());
        if (const auto suspended = Suspend(owner, nextSequence); suspended.HasError())
            return suspended;
        interaction_ = std::move(next).Value();
        dispatcher_.emplace(std::move(dispatcher).Value());
        defaultCount_ = 0;
        return Result<void>::Success();
    }

    /** @copydoc UiPointerInput::AddSample */
    void UiPointerInput::AddSample(const UiPointerInputSurface &surface, std::size_t &count, const UiPointerId pointer,
                                   const UiPointerEdge edge, const UiPointerModality modality, const UiPointerButton button, const float x,
                                   const float y) {
        samples_[count++] = {pointer,
                             edge,
                             modality,
                             button,
                             (x - static_cast<float>(surface.inputViewport.x)) * static_cast<float>(surface.canvasSpace.pixelExtent.width) /
                                 static_cast<float>(surface.inputViewport.width),
                             (y - static_cast<float>(surface.inputViewport.y)) *
                                 static_cast<float>(surface.canvasSpace.pixelExtent.height) /
                                 static_cast<float>(surface.inputViewport.height)};
    }

    /** @copydoc UiPointerInput::CollectMouse */
    Result<void> UiPointerInput::CollectMouse(Input::InputRouter &router, const Input::InputContextToken &context,
                                              const UiPointerInputSurface &surface, std::size_t &count) {
        const auto &snapshot = router.Snapshot();
        const auto mouse = UiPointerId::Create(1).Value();
        if (!mouseButton_) {
            for (std::size_t index = 0; index < snapshot.pointer.buttons.size(); ++index) {
                const auto button = static_cast<Input::PointerButton>(index);
                if (!router.ConsumePointerButton(context, button))
                    continue;
                auto captured = router.CapturePointer(context, button, *this);
                if (captured.HasError())
                    return Result<void>::Failure(captured.ErrorValue());
                mouseCapture_ = std::move(captured).Value();
                mouseButton_ = button;
                AddSample(surface, count, mouse, UiPointerEdge::Press, UiPointerModality::Mouse, static_cast<UiPointerButton>(index),
                          snapshot.pointer.x, snapshot.pointer.y);
                break;
            }
        }
        if (mouseButton_) {
            const auto state = snapshot.State(*mouseButton_);
            const auto button = static_cast<UiPointerButton>(*mouseButton_);
            if (state.released || !state.down) {
                AddSample(surface, count, mouse, UiPointerEdge::Release, UiPointerModality::Mouse, button, snapshot.pointer.x,
                          snapshot.pointer.y);
                mouseCapture_.Release();
                mouseButton_.reset();
            } else if (!state.pressed)
                AddSample(surface, count, mouse, UiPointerEdge::Move, UiPointerModality::Mouse, button, snapshot.pointer.x,
                          snapshot.pointer.y);
        } else if (router.ConsumePointerMotion(context)) {
            AddSample(surface, count, mouse, snapshot.window.pointerInside ? UiPointerEdge::Move : UiPointerEdge::Cancel,
                      UiPointerModality::Mouse, UiPointerButton::Primary, snapshot.pointer.x, snapshot.pointer.y);
        }
        return Result<void>::Success();
    }

    /** @copydoc UiPointerInput::CollectTouch */
    Result<void> UiPointerInput::CollectTouch(Input::InputRouter &router, const Input::InputContextToken &context,
                                              const UiPointerInputSurface &surface, const Input::TouchContactState &touch,
                                              std::size_t &count) {
        if (!touch.id.IsValid())
            return Result<void>::Success();
        auto owned = std::ranges::find(contacts_, touch.id, &Contact::source);
        if (owned == contacts_.end() && router.ConsumeTouchContact(context, touch.id)) {
            owned = std::ranges::find_if(contacts_, [](const Contact &entry) {
                return !entry.source.IsValid();
            });
            if (owned == contacts_.end() || nextPointer_ == std::numeric_limits<std::uint32_t>::max())
                return Result<void>::Failure(MakeError(UiErrors::PointerCaptureCapacityExceeded));
            *owned = {touch.id, UiPointerId::Create(nextPointer_++).Value(), touch.x, touch.y};
            AddSample(surface, count, owned->pointer, UiPointerEdge::Press, UiPointerModality::Touch, UiPointerButton::Primary, touch.x,
                      touch.y);
        }
        if (owned == contacts_.end())
            return Result<void>::Success();
        owned->x = touch.x;
        owned->y = touch.y;
        if (touch.cancelled || touch.contact.released || !touch.contact.down) {
            AddSample(surface, count, owned->pointer, touch.cancelled ? UiPointerEdge::Cancel : UiPointerEdge::Release,
                      UiPointerModality::Touch, UiPointerButton::Primary, touch.x, touch.y);
            *owned = {};
        } else if (!touch.contact.pressed)
            AddSample(surface, count, owned->pointer, UiPointerEdge::Move, UiPointerModality::Touch, UiPointerButton::Primary, touch.x,
                      touch.y);
        return Result<void>::Success();
    }

    /** @copydoc UiPointerInput::CollectTouches */
    Result<void> UiPointerInput::CollectTouches(Input::InputRouter &router, const Input::InputContextToken &context,
                                                const UiPointerInputSurface &surface, std::size_t &count) {
        const auto &snapshot = router.Snapshot();
        for (const auto &touch : snapshot.touches)
            if (const auto collected = CollectTouch(router, context, surface, touch, count); collected.HasError())
                return collected;
        for (auto &owned : contacts_) {
            if (owned.source.IsValid() &&
                std::ranges::find(snapshot.touches, owned.source, &Input::TouchContactState::id) == snapshot.touches.end()) {
                AddSample(surface, count, owned.pointer, UiPointerEdge::Cancel, UiPointerModality::Touch, UiPointerButton::Primary, owned.x,
                          owned.y);
                owned = {};
            }
        }
        return Result<void>::Success();
    }

    /** @copydoc UiPointerInput::Collect */
    Result<std::size_t> UiPointerInput::Collect(Input::InputRouter &router, const Input::InputContextToken &context,
                                                const UiPointerInputSurface &surface) {
        std::size_t count = 0;
        if (const auto mouse = CollectMouse(router, context, surface, count); mouse.HasError())
            return Result<std::size_t>::Failure(mouse.ErrorValue());
        if (const auto touches = CollectTouches(router, context, surface, count); touches.HasError())
            return Result<std::size_t>::Failure(touches.ErrorValue());
        return Result<std::size_t>::Success(count);
    }

    /** @copydoc UiPointerInput::ValidateFrame */
    Result<UiPointerInputStatus> UiPointerInput::ValidateFrame(Input::InputRouter &router, const Input::InputContextToken &context,
                                                               const UiPointerInputSurface &surface,
                                                               const std::uint64_t milliseconds) const {
        const auto routing = router.RoutingState(context);
        if (&router != router_ || routing.contextIdentity != context_)
            return Result<UiPointerInputStatus>::Failure(MakeError(UiErrors::EventDispatchSourceStale));
        const auto &snapshot = router.Snapshot();
        if (!routing.WithinLimits(64, 16) || !BoundedProfile(router) || !surface.inputViewport.IsValid() ||
            surface.inputViewport.width == 0 || surface.inputViewport.height == 0 || !surface.canvasSpace.IsValid() ||
            snapshot.frame == 0 || milliseconds < time_ || (hasFrame_ && snapshot.frame < frame_))
            return Result<UiPointerInputStatus>::Failure(MakeError(UiErrors::EventDispatchInvalid));
        if (hasFrame_ && snapshot.frame == frame_)
            return Result<UiPointerInputStatus>::Success(UiPointerInputStatus::DuplicateFrame);
        if (!std::isfinite(snapshot.pointer.x) || !std::isfinite(snapshot.pointer.y) ||
            std::ranges::any_of(snapshot.touches, [](const auto &touch) {
            return touch.id.IsValid() && (!std::isfinite(touch.x) || !std::isfinite(touch.y));
        }))
            return Result<UiPointerInputStatus>::Failure(MakeError(UiErrors::HitTestInvalid));
        return Result<UiPointerInputStatus>::Success(UiPointerInputStatus::Active);
    }

    /** @copydoc UiPointerInput::AdmitFrame */
    Result<UiPointerInputStatus> UiPointerInput::AdmitFrame(Input::InputRouter &router, const Input::InputContextToken &context,
                                                            const UiPointerInputSurface &surface, const std::uint64_t milliseconds) {
        const auto routing = router.RoutingState(context);
        const auto &snapshot = router.Snapshot();
        frame_ = snapshot.frame;
        time_ = milliseconds;
        hasFrame_ = true;
        if (routing.configurationRevision != configurationRevision_ || routing.assignmentRevision != assignmentRevision_) {
            if (const auto cancelled = Suspend(surface.owner, surface.nextSequence); cancelled.HasError())
                return Result<UiPointerInputStatus>::Failure(cancelled.ErrorValue());
            if (!routing.configurationRevision || !routing.assignmentRevision)
                return Result<UiPointerInputStatus>::Failure(MakeError(UiErrors::EventDispatchLifecycleUnavailable));
            configurationRevision_ = routing.configurationRevision;
            assignmentRevision_ = routing.assignmentRevision;
            return Result<UiPointerInputStatus>::Success(UiPointerInputStatus::Blocked);
        }
        if (cleanupPending_ || !router.IsContextActive(context) || snapshot.touchOverflow || !snapshot.window.pointerDeviceAvailable) {
            if (const auto cancelled = Suspend(surface.owner, surface.nextSequence); cancelled.HasError())
                return Result<UiPointerInputStatus>::Failure(cancelled.ErrorValue());
            return Result<UiPointerInputStatus>::Success(UiPointerInputStatus::Blocked);
        }
        if (const auto &hit = surface.hitTesting.Descriptor(); hit.tree != interaction_.Owner().tree ||
                                                               hit.interaction != interaction_.Owner().interaction ||
                                                               !surface.owner.PointerInputEligible(interaction_.Owner())) {
            if (const auto cancelled = Suspend(surface.owner, surface.nextSequence); cancelled.HasError())
                return Result<UiPointerInputStatus>::Failure(cancelled.ErrorValue());
            return Result<UiPointerInputStatus>::Success(UiPointerInputStatus::NeedsRebind);
        }
        return Result<UiPointerInputStatus>::Success(UiPointerInputStatus::Active);
    }

    /** @copydoc UiPointerInput::ReadAlternative */
    Result<std::optional<UiAccessibleGesture>> UiPointerInput::ReadAlternative(Input::InputRouter &router,
                                                                               const Input::InputContextToken &context,
                                                                               const UiPointerInputSurface &surface) {
        std::array<Input::ActionEvidence, UiPointerActionCount> actions{};
        for (std::size_t index = 0; index < actions.size(); ++index) {
            actions[index] = router.ReadActionEvidence(context, actions_[index]);
            if (router.LastActionStatus() == Input::ActionReadStatus::CapacityExceeded)
                return Result<std::optional<UiAccessibleGesture>>::Failure(MakeError(UiErrors::EventDispatchCapacityExceeded));
        }
        std::optional<UiAccessibleGesture> alternative;
        if (actions[3].value.pressed) {
            if (const auto cancelled = Suspend(surface.owner, surface.nextSequence); cancelled.HasError())
                return Result<std::optional<UiAccessibleGesture>>::Failure(cancelled.ErrorValue());
            alternative = UiAccessibleGesture::Cancel;
        } else if (actions[1].value.pressed)
            alternative = UiAccessibleGesture::ContextAction;
        else if (actions[2].value.pressed)
            alternative = interaction_.HasAccessibleDrag() ? UiAccessibleGesture::Drop : UiAccessibleGesture::PickUp;
        else if (actions[0].value.pressed)
            alternative = interaction_.HasAccessibleDrag() ? UiAccessibleGesture::Drop : UiAccessibleGesture::Activate;
        return Result<std::optional<UiAccessibleGesture>>::Success(alternative);
    }

    /** @copydoc UiPointerInput::DeliverFrame */
    Result<UiPointerInputFrame> UiPointerInput::DeliverFrame(Input::InputRouter &router, const Input::InputContextToken &context,
                                                             const UiPointerInputSurface &surface, const std::uint64_t milliseconds,
                                                             const std::optional<UiAccessibleGesture> alternative) {
        const auto routing = router.RoutingState(context);
        const auto collected =
            alternative == UiAccessibleGesture::Cancel ? Result<std::size_t>::Success(0) : Collect(router, context, surface);
        if (collected.HasError()) {
            (void)Suspend(surface.owner, surface.nextSequence);
            return Result<UiPointerInputFrame>::Failure(collected.ErrorValue());
        }
        const UiAnimationPointerInput input{interaction_.Owner().view,
                                            interaction_,
                                            *dispatcher_,
                                            surface.hitTesting,
                                            surface.canvasSpace,
                                            std::span{samples_}.first(collected.Value()),
                                            milliseconds,
                                            surface.nextSequence,
                                            defaults_,
                                            defaultCount_,
                                            alternative};
        InputGestureRoute route{surface.routeHandler, router, context, {stopped_, cleanupPending_, frame_, routing}};
        auto pumped = surface.owner.PumpPointers(input, route);
        if (pumped.HasError()) {
            (void)Suspend(surface.owner, surface.nextSequence);
            return Result<UiPointerInputFrame>::Failure(pumped.ErrorValue());
        }
        return Result<UiPointerInputFrame>::Success({UiPointerInputStatus::Active, pumped.Value(), Defaults()});
    }

    /** @copydoc UiPointerInput::Pump */
    Result<UiPointerInputFrame> UiPointerInput::Pump(Input::InputRouter &router, const Input::InputContextToken &context,
                                                     const UiPointerInputSurface &surface, const std::uint64_t milliseconds) {
        if (pumping_)
            return Result<UiPointerInputFrame>::Failure(MakeError(UiErrors::EventDispatchReentrant));
        PointerInputGuard guard{pumping_};
        defaultCount_ = 0;
        if (stopped_)
            return Result<UiPointerInputFrame>::Success({UiPointerInputStatus::Stopped});
        const auto validated = ValidateFrame(router, context, surface, milliseconds);
        if (validated.HasError())
            return Result<UiPointerInputFrame>::Failure(validated.ErrorValue());
        if (validated.Value() != UiPointerInputStatus::Active)
            return Result<UiPointerInputFrame>::Success({validated.Value()});
        const auto admitted = AdmitFrame(router, context, surface, milliseconds);
        if (admitted.HasError())
            return Result<UiPointerInputFrame>::Failure(admitted.ErrorValue());
        if (admitted.Value() != UiPointerInputStatus::Active)
            return Result<UiPointerInputFrame>::Success({admitted.Value()});
        const auto alternative = ReadAlternative(router, context, surface);
        if (alternative.HasError())
            return Result<UiPointerInputFrame>::Failure(alternative.ErrorValue());
        return DeliverFrame(router, context, surface, milliseconds, alternative.Value());
    }

}  // namespace Horo::Runtime::Ui
