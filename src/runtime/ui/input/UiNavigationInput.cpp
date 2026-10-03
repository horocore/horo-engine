#include "Horo/Runtime/Ui/UiNavigationInput.h"

#include "Horo/Runtime/Ui/UiErrors.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace Horo::Runtime::Ui {
    namespace {
        /** @brief Converts graph owner evidence to the existing action owner contract. */
        UiActionOwnerContext ActionOwner(const UiFocusOwnerContext &owner) noexcept {
            return {owner.instance, owner.canvas, owner.document, owner.documentRevision, owner.treeRevision, owner.interaction};
        }

        /** @brief Adds a finite deadline without wrapping host clock evidence. */
        std::uint64_t Deadline(const std::uint64_t now, const std::uint32_t delay) noexcept {
            return now > std::numeric_limits<std::uint64_t>::max() - delay ? std::numeric_limits<std::uint64_t>::max() : now + delay;
        }

        /** @brief Checks load-time action-map/profile bounds before any linear lookup. */
        bool BoundedConfiguration(const Input::InputRouter &router) noexcept {
            return router.Actions().size() <= 512 && router.Profile().overrides.size() <= 512;
        }

        /** @brief Checks live focus/action composition and non-exhausted Input generations at admission. */
        bool LiveOwners(const UiFocusGraph &focus, const UiActionRouter &actions, const Input::InputRoutingState &routing) noexcept {
            return focus.Owner().IsValid() && focus.State() == UiFocusGraphState::Active &&
                   actions.State() == UiActionRouterState::Active && actions.Owner() == ActionOwner(focus.Owner()) &&
                   routing.contextIdentity != 0 && routing.configurationRevision != 0 && routing.assignmentRevision != 0;
        }

        /** @brief Reloads preserve scope identity and never regress a published revision. */
        bool CompatibleReload(const UiFocusOwnerContext &current, const UiFocusOwnerContext &next) noexcept {
            return next.instance == current.instance && next.canvas == current.canvas && next.document == current.document &&
                   next.scope == current.scope && next.interaction >= current.interaction && next.treeRevision >= current.treeRevision &&
                   next.documentRevision >= current.documentRevision;
        }

        /** @brief Held repeats require a press actually consumed by this adapter after its neutral gate. */
        void GateHeld(Input::ActionEvidence &sample, const bool down, bool &disarmed, bool &owned) noexcept {
            if (!down) {
                disarmed = false;
                owned = false;
            } else if (sample.value.pressed && !disarmed) {
                owned = true;
            }
            if (disarmed || !owned)
                sample = {};
        }
    }  // namespace

    /** @copydoc DefaultUiNavigationActions */
    std::vector<Input::ActionDescriptor> DefaultUiNavigationActions(const Input::InputContextId &context) {
        const std::array names{"ui.next", "ui.previous", "ui.up", "ui.down", "ui.left", "ui.right", "ui.submit", "ui.cancel"};
        constexpr auto keys = [] {
            using enum Input::Key;
            return std::array{Tab, Tab, Up, Down, Left, Right, Enter, Escape};
        }();
        constexpr auto buttons = [] {
            using enum Input::GamepadButton;
            return std::array{RightShoulder, LeftShoulder, DPadUp, DPadDown, DPadLeft, DPadRight, South, East};
        }();
        std::vector<Input::ActionDescriptor> actions;
        actions.reserve(UiNavigationActionCount + 1);
        for (std::size_t index = 0; index < UiNavigationActionCount; ++index) {
            Input::InputBinding key;
            key.key = keys[index];
            key.requiredModifiers.shift = index == 1;
            Input::InputBinding pad;
            pad.kind = Input::BindingControlKind::GamepadButton;
            pad.gamepadButton = buttons[index];
            Input::ActionDescriptor action{Input::ActionId{names[index]}, Input::ActionValueType::Digital, context, true, {key, pad}};
            actions.push_back(std::move(action));
        }
        Input::InputBinding horizontal;
        horizontal.kind = Input::BindingControlKind::GamepadAxis;
        horizontal.gamepadAxis = Input::GamepadAxis::LeftX;
        horizontal.deadzoneKind = Input::DeadzoneKind::Radial;
        horizontal.deadzone = 0.25F;
        auto vertical = horizontal;
        vertical.gamepadAxis = Input::GamepadAxis::LeftY;
        vertical.component = 1;
        actions.push_back({Input::ActionId{"ui.navigate"}, Input::ActionValueType::Axis2D, context, true, {horizontal, vertical}});
        return actions;
    }

    namespace {
        /** @brief Validates finite repeat/hysteresis policy and unique semantic action identities before copying. */
        bool ValidPolicy(const UiNavigationInputDescriptor &descriptor) noexcept {
            if (descriptor.repeatDelayMilliseconds == 0 || descriptor.repeatDelayMilliseconds > 10'000 ||
                descriptor.repeatIntervalMilliseconds == 0 || descriptor.repeatIntervalMilliseconds > 10'000 ||
                descriptor.modalityHysteresisMilliseconds > 10'000 || !descriptor.directionalAxis.IsValid())
                return false;
            for (std::size_t index = 0; index < descriptor.actions.size(); ++index) {
                const auto &id = descriptor.actions[index];
                if (!id.IsValid() ||
                    std::find(descriptor.actions.begin(), descriptor.actions.begin() + index, id) != descriptor.actions.begin() + index)
                    return false;
            }
            return true;
        }

        /** @brief Borrows only the bounded effective binding set for an exact registered context and value shape. */
        Result<std::span<const Input::InputBinding>> EffectiveBindings(const Input::InputRouter &router,
                                                                       const Input::InputContextToken &context, const Input::ActionId &id,
                                                                       const Input::ActionValueType shape) {
            using Bindings = std::span<const Input::InputBinding>;
            const auto found = std::ranges::find(router.Actions(), id, &Input::ActionDescriptor::id);
            if (found == router.Actions().end() || !router.ContextMatches(context, found->context))
                return Result<Bindings>::Failure(MakeError(UiErrors::NavigationInvalid));
            if (const bool scalarAxis = shape == Input::ActionValueType::Digital && found->valueType == Input::ActionValueType::Axis1D;
                found->valueType != shape && !scalarAxis)
                return Result<Bindings>::Failure(MakeError(UiErrors::NavigationCapabilityUnsupported));
            const auto overrides = std::span(router.Profile().overrides);
            const auto replacement = std::ranges::find(overrides, id, &Input::BindingOverride::action);
            const auto &bindings = replacement == overrides.end() ? found->defaultBindings : replacement->bindings;
            if (bindings.size() > 32)
                return Result<Bindings>::Failure(MakeError(UiErrors::CapacityExceeded));
            if (bindings.empty())
                return Result<Bindings>::Failure(MakeError(UiErrors::NavigationCapabilityUnsupported));
            return Result<Bindings>::Success(bindings);
        }

        /** @brief Resolves first supported canonical glyphs for each modality without retaining a router borrow. */
        Result<std::array<std::optional<Input::InputGlyphId>, 2>> DigitalGlyphs(const std::span<const Input::InputBinding> bindings) {
            using Glyphs = std::array<std::optional<Input::InputGlyphId>, 2>;
            Glyphs glyphs{};
            for (const auto &binding : bindings) {
                const auto source = Input::CanonicalActionSource(binding);
                if (Input::CanonicalGlyph(source.glyph).support == Input::InputGlyphSupport::Unsupported || binding.scale <= 0.0F)
                    return Result<Glyphs>::Failure(MakeError(UiErrors::NavigationCapabilityUnsupported));
                const std::size_t modality = source.modality == Input::InputModality::Gamepad ? 1 : 0;
                if (!glyphs[modality])
                    glyphs[modality] = source.glyph;
            }
            return Result<Glyphs>::Success(glyphs);
        }

        /** @brief Resolves the complete effective digital-action glyph capability at composition time. */
        Result<std::array<std::optional<Input::InputGlyphId>, 2>> EffectiveDigitalGlyphs(const Input::InputRouter &router,
                                                                                         const Input::InputContextToken &context,
                                                                                         const Input::ActionId &id) {
            const auto bindings = EffectiveBindings(router, context, id, Input::ActionValueType::Digital);
            if (bindings.HasError())
                return Result<std::array<std::optional<Input::InputGlyphId>, 2>>::Failure(bindings.ErrorValue());
            return DigitalGlyphs(bindings.Value());
        }

        /** @brief Requires post-noise-filter signed canonical evidence for both navigation axis components. */
        Result<void> ValidateAxisBindings(const std::span<const Input::InputBinding> bindings) {
            std::array<bool, 2> components{};
            for (const auto &binding : bindings) {
                if (binding.kind != Input::BindingControlKind::GamepadAxis || binding.deadzone < 0.1F ||
                    binding.deadzoneKind == Input::DeadzoneKind::None || binding.component >= components.size())
                    return Result<void>::Failure(MakeError(UiErrors::NavigationCapabilityUnsupported));
                components[binding.component] = true;
            }
            if (!components[0] || !components[1])
                return Result<void>::Failure(MakeError(UiErrors::NavigationCapabilityUnsupported));
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc UiNavigationInput::Create */
    Result<UiNavigationInput> UiNavigationInput::Create(const UiNavigationInputDescriptor &descriptor, const Input::InputRouter &router,
                                                        const Input::InputContextToken &context, const UiFocusGraph &focus,
                                                        const UiActionRouter &actions) {
        const auto routing = router.RoutingState(context);
        if (!BoundedConfiguration(router) || !routing.WithinLimits(64, 16))
            return Result<UiNavigationInput>::Failure(MakeError(UiErrors::CapacityExceeded));
        if (!ValidPolicy(descriptor))
            return Result<UiNavigationInput>::Failure(MakeError(UiErrors::NavigationInvalid));
        if (!LiveOwners(focus, actions, routing))
            return Result<UiNavigationInput>::Failure(MakeError(UiErrors::FocusSourceStale));
        UiNavigationInput candidate;
        try {
            candidate.descriptor_ = descriptor;
        } catch (const std::bad_alloc &) {
            return Result<UiNavigationInput>::Failure(MakeError(UiErrors::CapacityExceeded));
        }
        candidate.owner_ = focus.Owner();
        candidate.routerIdentity_ = &router;
        candidate.contextIdentity_ = routing.contextIdentity;
        candidate.configurationRevision_ = routing.configurationRevision;
        candidate.assignmentRevision_ = routing.assignmentRevision;
        candidate.modal_ = focus.Snapshot().Value().activeModal;
        candidate.disarmed_.fill(true);
        for (std::size_t index = 0; index < UiNavigationActionCount; ++index) {
            const auto glyphs = EffectiveDigitalGlyphs(router, context, descriptor.actions[index]);
            if (glyphs.HasError())
                return Result<UiNavigationInput>::Failure(glyphs.ErrorValue());
            candidate.glyphs_[index] = glyphs.Value();
        }
        const auto axis = EffectiveBindings(router, context, descriptor.directionalAxis, Input::ActionValueType::Axis2D);
        if (axis.HasError())
            return Result<UiNavigationInput>::Failure(axis.ErrorValue());
        if (const auto valid = ValidateAxisBindings(axis.Value()); valid.HasError())
            return Result<UiNavigationInput>::Failure(valid.ErrorValue());
        return Result<UiNavigationInput>::Success(std::move(candidate));
    }

    /** @copydoc UiNavigationInput::SetPresentation */
    void UiNavigationInput::SetPresentation(const Input::InputModality modality,
                                            const std::optional<Input::GamepadDeviceId> device) noexcept {
        if (presentation_.modality == modality && presentation_.device == device)
            return;
        presentation_.modality = modality;
        presentation_.device = device;
        if (presentation_.revision != std::numeric_limits<std::uint64_t>::max())
            ++presentation_.revision;
        for (std::size_t index = 0; index < UiNavigationActionCount; ++index) {
            const auto glyph = glyphs_[index][modality == Input::InputModality::Gamepad ? 1 : 0];
            presentation_.glyphs[index] =
                modality != Input::InputModality::Unknown && glyph ? Input::CanonicalGlyph(*glyph) : Input::InputGlyphPresentation{};
        }
    }

    /** @copydoc UiNavigationInput::ObserveModality */
    void UiNavigationInput::ObserveModality(const std::optional<Input::ActionSource> &source, const std::uint64_t milliseconds) noexcept {
        using enum Input::InputModality;
        if (source) {
            if (source->modality == presentation_.modality && source->gamepad == presentation_.device) {
                pending_ = {};
                return;
            }
            if (source->modality != pending_.modality || source->gamepad != pending_.device) {
                pending_ = {source->modality, source->gamepad, milliseconds};
            }
        }
        if (pending_.modality != Unknown &&
            (presentation_.modality == Unknown || milliseconds - pending_.since >= descriptor_.modalityHysteresisMilliseconds)) {
            SetPresentation(pending_.modality, pending_.device);
            pending_ = {};
        }
    }

    /** @copydoc UiNavigationInput::ValidateCall */
    Result<void> UiNavigationInput::ValidateCall(const Input::InputRouter &router, const Input::InputContextToken &context,
                                                 const UiFocusGraph &focus, const UiActionRouter &actions) const {
        const auto routing = router.RoutingState(context);
        if (&router != routerIdentity_ || routing.contextIdentity != contextIdentity_ || focus.Owner() != owner_ ||
            actions.Owner() != ActionOwner(owner_))
            return Result<void>::Failure(MakeError(UiErrors::FocusSourceStale));
        if (!routing.WithinLimits(64, 16))
            return Result<void>::Failure(MakeError(UiErrors::CapacityExceeded));
        if (focus.State() != UiFocusGraphState::Active)
            return Result<void>::Failure(MakeError(UiErrors::FocusLifecycleUnavailable));
        if (actions.State() != UiActionRouterState::Active)
            return Result<void>::Failure(MakeError(UiErrors::ActionLifecycleUnavailable));
        return Result<void>::Success();
    }

    /** @copydoc UiNavigationInput::AdmitFrame */
    Result<UiNavigationInputStatus> UiNavigationInput::AdmitFrame(const Input::InputRouter &router, const Input::InputRoutingState &routing,
                                                                  const std::uint64_t milliseconds) {
        using enum UiNavigationInputStatus;
        const auto frame = router.Snapshot().frame;
        if (hasFrame_ && (frame < frame_ || milliseconds < time_))
            return Result<UiNavigationInputStatus>::Failure(MakeError(UiErrors::ControlSequenceInvalid));
        if (hasFrame_ && frame == frame_)
            return Result<UiNavigationInputStatus>::Success(DuplicateFrame);
        frame_ = frame;
        time_ = milliseconds;
        hasFrame_ = true;
        if (routing.configurationRevision != configurationRevision_ || routing.assignmentRevision == 0) {
            Suspend();
            return Result<UiNavigationInputStatus>::Success(NeedsRebind);
        }
        return Result<UiNavigationInputStatus>::Success(Active);
    }

    /** @copydoc UiNavigationInput::ReconcileLifecycle */
    void UiNavigationInput::ReconcileLifecycle(const Input::InputRouter &router, const Input::InputRoutingState &routing,
                                               const UiFocusSnapshot &focus) noexcept {
        if (focus.activeModal != modal_ || routing.assignmentRevision != assignmentRevision_) {
            Suspend();
            modal_ = focus.activeModal;
            assignmentRevision_ = routing.assignmentRevision;
        }
        const auto eligible = [&](const Input::GamepadDeviceId id) {
            return router.Snapshot().FindGamepad(id) != nullptr &&
                   (!descriptor_.player.has_value() || router.PlayerForGamepad(id) == descriptor_.player);
        };
        if (presentation_.device && !eligible(*presentation_.device)) {
            Suspend();
            SetPresentation(Input::InputModality::Unknown, {});
        }
        if (pending_.device && !eligible(*pending_.device))
            pending_ = {};
    }

    namespace {
        /** @brief Chooses only meaningful routed evidence with the canonical simultaneous-device tie order. */
        void PreferSource(const Input::ActionEvidence &sample, std::optional<Input::ActionSource> &source) noexcept {
            if (sample.meaningful && sample.source && (!source || sample.source->modality < source->modality))
                source = sample.source;
        }

        /** @brief Resolves opposing pairs first, then the signed dominant axis with vertical ties. */
        std::optional<UiNavigationDirection> Direction(const std::array<Input::ActionEvidence, UiNavigationActionCount> &digital,
                                                       const Input::ActionValue &axis) noexcept {
            using enum UiNavigationDirection;
            for (std::size_t pair = 0; pair < 3; ++pair) {
                const std::size_t first = pair * 2;
                const bool a = digital[first].value.x >= 0.5F;
                const bool b = digital[first + 1].value.x >= 0.5F;
                if (a != b)
                    return static_cast<UiNavigationDirection>(a ? first : first + 1);
            }
            if (std::abs(axis.y) >= 0.5F && std::abs(axis.y) >= std::abs(axis.x))
                return axis.y < 0.0F ? Up : Down;
            if (std::abs(axis.x) >= 0.5F)
                return axis.x < 0.0F ? Left : Right;
            return {};
        }
    }  // namespace

    /** @copydoc UiNavigationInput::ReadSamples */
    Result<UiNavigationInput::Samples> UiNavigationInput::ReadSamples(Input::InputRouter &router, const Input::InputContextToken &context) {
        Samples samples;
        for (std::size_t index = 0; index < samples.digital.size(); ++index) {
            auto &sample = samples.digital[index];
            sample = router.ReadActionEvidence(context, descriptor_.actions[index], descriptor_.player);
            if (sample.status == Input::ActionReadStatus::CapacityExceeded)
                return Result<Samples>::Failure(MakeError(UiErrors::CapacityExceeded));
            GateHeld(sample, sample.value.x >= 0.5F, disarmed_[index], heldOwned_[index]);
            PreferSource(sample, samples.meaningful);
        }
        samples.axis = router.ReadActionEvidence(context, descriptor_.directionalAxis, descriptor_.player);
        if (samples.axis.status == Input::ActionReadStatus::CapacityExceeded)
            return Result<Samples>::Failure(MakeError(UiErrors::CapacityExceeded));
        GateHeld(samples.axis, samples.axis.value.down, axisDisarmed_, axisOwned_);
        PreferSource(samples.axis, samples.meaningful);
        return Result<Samples>::Success(samples);
    }

    /** @copydoc UiNavigationInput::Navigate */
    Result<std::optional<UiFocusChange>> UiNavigationInput::Navigate(const Samples &samples, UiFocusGraph &focus,
                                                                     const std::uint64_t milliseconds) {
        const auto direction = Direction(samples.digital, samples.axis.value);
        std::optional<UiFocusChange> change;
        if (direction &&
            (repeating_ != direction || (milliseconds >= repeatAt_ && repeatAt_ != std::numeric_limits<std::uint64_t>::max()))) {
            const auto moved = focus.Move(*direction);
            if (moved.HasError())
                return Result<std::optional<UiFocusChange>>::Failure(moved.ErrorValue());
            change = moved.Value();
            repeatAt_ = Deadline(milliseconds,
                                 repeating_ == direction ? descriptor_.repeatIntervalMilliseconds : descriptor_.repeatDelayMilliseconds);
        }
        repeating_ = direction;
        return Result<std::optional<UiFocusChange>>::Success(change);
    }

    /** @copydoc UiNavigationInput::QueueActivation */
    Result<std::array<std::optional<UiActionRequestId>, 2>> UiNavigationInput::QueueActivation(const Samples &samples,
                                                                                               const UiFocusGraph &focus,
                                                                                               UiActionRouter &actions) const {
        using Requests = std::array<std::optional<UiActionRequestId>, 2>;
        Requests requests{};
        const std::size_t index = samples.digital[7].value.pressed && samples.digital[7].value.x >= 0.5F ? 1 : 0;
        if (const auto &sample = samples.digital[6 + index].value; !sample.pressed || sample.x < 0.5F)
            return Result<Requests>::Success(requests);
        const auto target = focus.CurrentFocus();
        if (target.HasError())
            return Result<Requests>::Failure(target.ErrorValue());
        if (target.Value()) {
            const auto queued =
                actions.Enqueue({ActionOwner(owner_), target.Value()->element},
                                UiNavigationCommand{static_cast<UiNavigationDirection>(6 + index), target.Value()->element});
            if (queued.HasError())
                return Result<Requests>::Failure(queued.ErrorValue());
            requests[index] = queued.Value();
        }
        return Result<Requests>::Success(requests);
    }

    /** @copydoc UiNavigationInput::Pump */
    Result<UiNavigationInputFrame> UiNavigationInput::Pump(Input::InputRouter &router, const Input::InputContextToken &context,
                                                           UiFocusGraph &focus, UiActionRouter &actions, const std::uint64_t milliseconds) {
        UiNavigationInputFrame output;
        output.presentation = presentation_;
        if (stopped_) {
            output.status = UiNavigationInputStatus::Stopped;
            return Result<UiNavigationInputFrame>::Success(output);
        }
        if (const auto valid = ValidateCall(router, context, focus, actions); valid.HasError()) {
            Suspend();
            return Result<UiNavigationInputFrame>::Failure(valid.ErrorValue());
        }
        const auto routing = router.RoutingState(context);
        const auto admitted = AdmitFrame(router, routing, milliseconds);
        if (admitted.HasError())
            return Result<UiNavigationInputFrame>::Failure(admitted.ErrorValue());
        output.status = admitted.Value();
        if (output.status != UiNavigationInputStatus::Active)
            return Result<UiNavigationInputFrame>::Success(output);
        ReconcileLifecycle(router, routing, focus.Snapshot().Value());
        if (!router.IsContextActive(context)) {
            Suspend();
            output.status = UiNavigationInputStatus::Blocked;
            output.presentation = presentation_;
            return Result<UiNavigationInputFrame>::Success(output);
        }
        const auto samples = ReadSamples(router, context);
        if (samples.HasError()) {
            Suspend();
            return Result<UiNavigationInputFrame>::Failure(samples.ErrorValue());
        }
        ObserveModality(samples.Value().meaningful, milliseconds);
        output.presentation = presentation_;
        const auto changed = Navigate(samples.Value(), focus, milliseconds);
        if (changed.HasError())
            return Result<UiNavigationInputFrame>::Failure(changed.ErrorValue());
        output.focus = changed.Value();
        const auto queued = QueueActivation(samples.Value(), focus, actions);
        if (queued.HasError())
            return Result<UiNavigationInputFrame>::Failure(queued.ErrorValue());
        output.requests = queued.Value();
        return Result<UiNavigationInputFrame>::Success(output);
    }

    /** @copydoc UiNavigationInput::Rebind */
    Result<void> UiNavigationInput::Rebind(const Input::InputRouter &router, const Input::InputContextToken &context,
                                           const UiFocusGraph &focus, const UiActionRouter &actions) {
        if (stopped_ || &router != routerIdentity_ || router.RoutingState(context).contextIdentity != contextIdentity_ ||
            !CompatibleReload(owner_, focus.Owner()))
            return Result<void>::Failure(MakeError(UiErrors::FocusSourceStale));
        auto replacement = Create(descriptor_, router, context, focus, actions);
        if (replacement.HasError())
            return Result<void>::Failure(replacement.ErrorValue());
        auto candidate = std::move(replacement).Value();
        candidate.frame_ = frame_;
        candidate.time_ = time_;
        candidate.hasFrame_ = hasFrame_;
        const auto modality = presentation_.modality;
        const auto device = presentation_.device;
        candidate.presentation_.revision = presentation_.revision;
        candidate.SetPresentation(modality, device);
        *this = std::move(candidate);
        return Result<void>::Success();
    }

    /** @copydoc UiNavigationInput::Suspend */
    void UiNavigationInput::Suspend() noexcept {
        disarmed_.fill(true);
        axisDisarmed_ = true;
        heldOwned_.fill(false);
        axisOwned_ = false;
        repeating_.reset();
        pending_ = {};
    }

    /** @copydoc UiNavigationInput::Shutdown */
    void UiNavigationInput::Shutdown() noexcept {
        stopped_ = true;
        Suspend();
        SetPresentation(Input::InputModality::Unknown, {});
    }

    /** @copydoc UiNavigationInput::Presentation */
    UiInputPresentation UiNavigationInput::Presentation() const noexcept {
        return presentation_;
    }
}  // namespace Horo::Runtime::Ui
