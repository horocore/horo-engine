#include "Horo/CharacterInput/DesiredMotionAdapter.h"

#include "Horo/Physics/CharacterErrors.h"

#include <algorithm>
#include <cmath>
#include <span>
#include <utility>

namespace Horo::CharacterInput {
    namespace {
        /** @brief Validates finite arithmetic before projecting or copying an intent. */
        bool Finite(const Math::Vec3 &value) noexcept {
            return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
        }

        /** @brief Requires a finite usable heading without silently normalizing authored intent. */
        bool ValidIntent(const DesiredMotionIntent &intent) noexcept {
            if (intent.velocityMetersPerSecond && !Finite(*intent.velocityMetersPerSecond))
                return false;
            if (!intent.heading)
                return true;
            const auto &heading = *intent.heading;
            const double squared = static_cast<double>(heading.x) * heading.x + static_cast<double>(heading.y) * heading.y +
                                   static_cast<double>(heading.z) * heading.z + static_cast<double>(heading.w) * heading.w;
            return std::isfinite(squared) && std::abs(squared - 1.0) <= 0.001;
        }

        /** @brief A bounded orthonormal projection cannot amplify input or overflow float arithmetic. */
        bool ValidProjection(const DesiredMotionInputBinding &binding) noexcept {
            if (!Finite(binding.right) || !Finite(binding.forward) || !std::isfinite(binding.speedMetersPerSecond) ||
                binding.speedMetersPerSecond < 0 || binding.speedMetersPerSecond > 10'000)
                return false;
            const auto dot = [](const Math::Vec3 &a, const Math::Vec3 &b) {
                return static_cast<double>(a.x) * b.x + static_cast<double>(a.y) * b.y + static_cast<double>(a.z) * b.z;
            };
            return std::abs(dot(binding.right, binding.right) - 1.0) <= 0.001 &&
                   std::abs(dot(binding.forward, binding.forward) - 1.0) <= 0.001 && std::abs(dot(binding.right, binding.forward)) <= 0.001;
        }

        /** @brief Validates the complete effective binding set independently of semantic action lookup. */
        bool ValidBindings(const std::span<const Input::InputBinding> bindings) noexcept {
            return !bindings.empty() && bindings.size() <= 32 && std::ranges::all_of(bindings, [](const Input::InputBinding &binding) {
                return std::isfinite(binding.scale) && std::isfinite(binding.deadzone) && std::isfinite(binding.digitalThreshold) &&
                       binding.chordSize <= binding.chord.size() && binding.component <= 1;
            });
        }

        /** @brief Admits one exact action and its effective finite binding set before any frame-hot scan. */
        bool ValidAction(const Input::InputRouter &router, const Input::InputContextToken &context, const Input::ActionId &id,
                         const Input::ActionValueType shape) {
            const auto found = std::ranges::find(router.Actions(), id, &Input::ActionDescriptor::id);
            if (found == router.Actions().end() || found->valueType != shape || !router.ContextMatches(context, found->context))
                return false;
            const auto replacement = std::ranges::find(router.Profile().overrides, id, &Input::BindingOverride::action);
            const auto &bindings = replacement == router.Profile().overrides.end() ? found->defaultBindings : replacement->bindings;
            return ValidBindings(bindings);
        }

        /** @brief Load-time bounded map admission follows Input's existing downstream adapter limits. */
        bool ValidInput(const Input::InputRouter &router, const Input::InputContextToken &context,
                        const DesiredMotionInputBinding &binding) {
            if (router.Actions().size() > 512 || router.Profile().overrides.size() > 512 || !ValidProjection(binding))
                return false;
            const auto routing = router.RoutingState(context);
            return routing.contextIdentity != 0 && routing.configurationRevision != 0 && routing.assignmentRevision != 0 &&
                   routing.WithinLimits(64, 16) && binding.move != binding.jump &&
                   ValidAction(router, context, binding.move, Input::ActionValueType::Axis2D) &&
                   ValidAction(router, context, binding.jump, Input::ActionValueType::Digital);
        }

        /** @brief External intent needs no Input owner; Gameplay requires the complete synchronous capture binding. */
        bool ValidSourceBinding(const DesiredMotionSource source, const DesiredMotionInputBinding &binding,
                                const Input::InputRouter *router, const Input::InputContextToken *context) {
            return source != DesiredMotionSource::GameplayInput ||
                   (router != nullptr && context != nullptr && ValidInput(*router, *context, binding));
        }

        /** @brief Compares every ownership fence, not just the producer generation. */
        bool SameGrant(const Character::CharacterCapabilityIdentity &a, const Character::CharacterCapabilityIdentity &b) noexcept {
            return a.sceneGeneration == b.sceneGeneration && a.world == b.world && a.physicsWorld == b.physicsWorld &&
                   a.generation == b.generation;
        }

        /** @brief True absence never becomes an implicit stop command. */
        bool Absent(const DesiredMotionIntent &intent) noexcept {
            return !intent.velocityMetersPerSecond && !intent.heading && !intent.jumpRequested;
        }
    }  // namespace

    /** @copydoc DesiredMotionFrame::IssuedFor */
    bool DesiredMotionFrame::IssuedFor(const DesiredMotionPrincipal &principal, const Character::CharacterCapabilityIdentity &grant,
                                       const Character::CharacterControllerHandle &controller) const noexcept {
        return principal_ == principal.identity && player_ == principal.player && source_ == principal.source &&
               controller_ == controller && SameGrant(grant_, grant);
    }

    struct DesiredMotionAdapter::State final {
        DesiredMotionPrincipal principal;
        Character::CharacterCapability capability;
        Character::CharacterControllerHandle controller;
        DesiredMotionInputBinding binding;
        const Input::InputRouter *captureOwner{}; /**< Identity only; never dereferenced outside CaptureInput. */
        Input::InputRoutingState routing;
        Input::FrameNumber capturedFrame{};
        bool hasCapture{};
        bool routingValid{true};
        std::optional<Math::Vec3> heldVelocity;
        bool pendingJump{};
        std::uint64_t producedTick{};
        std::uint64_t producedCorrelation{};
        std::uint64_t submittedTick{};
        std::uint64_t submittedCorrelation{};

        /** @brief Clears all accumulated input atomically on loss of eligibility or ownership. */
        void ClearInput() noexcept {
            heldVelocity.reset();
            pendingJump = false;
        }

        /** @brief Admits the original capture owner, then permanently closes changed routing generations. */
        Result<void> ValidateRouting(const Input::InputRouter &router, const Input::InputContextToken &context) {
            if (captureOwner != &router)
                return Result<void>::Failure(MakeError(Character::CharacterErrors::CapabilityStale));
            if (const auto current = router.RoutingState(context); !routingValid || current.contextIdentity != routing.contextIdentity ||
                                                                   current.configurationRevision != routing.configurationRevision ||
                                                                   current.assignmentRevision != routing.assignmentRevision ||
                                                                   !current.WithinLimits(64, 16)) {
                ClearInput();
                routingValid = false;
                capability.Revoke();
                return Result<void>::Failure(MakeError(Character::CharacterErrors::CapabilityStale));
            }
            return Result<void>::Success();
        }

        /** @brief Copies eligible action evidence and projects it through admitted finite host policy. */
        Result<void> CaptureSample(Input::InputRouter &router, const Input::InputContextToken &context) {
            const auto movement = router.ReadActionEvidence(context, binding.move, principal.player);
            const auto jump = router.ReadActionEvidence(context, binding.jump, principal.player);
            if (movement.status != Input::ActionReadStatus::Resolved || jump.status != Input::ActionReadStatus::Resolved) {
                ClearInput();
                return Result<void>::Failure(MakeError(Character::CharacterErrors::CapacityExceeded));
            }
            if (!std::isfinite(movement.value.x) || !std::isfinite(movement.value.y)) {
                ClearInput();
                return Result<void>::Failure(MakeError(Character::CharacterErrors::RequestInvalid));
            }
            const float length = std::hypot(movement.value.x, movement.value.y);
            const float scale = binding.speedMetersPerSecond / std::max(1.0F, length);
            heldVelocity = Math::Vec3{(binding.right.x * movement.value.x + binding.forward.x * movement.value.y) * scale,
                                      (binding.right.y * movement.value.x + binding.forward.y * movement.value.y) * scale,
                                      (binding.right.z * movement.value.x + binding.forward.z * movement.value.y) * scale};
            pendingJump = pendingJump || jump.value.pressed;
            return Result<void>::Success();
        }
    };

    /** @copydoc DesiredMotionAdapter::DesiredMotionAdapter */
    DesiredMotionAdapter::DesiredMotionAdapter() noexcept = default;

    /** @copydoc DesiredMotionAdapter::~DesiredMotionAdapter */
    DesiredMotionAdapter::~DesiredMotionAdapter() {
        Shutdown();
    }

    /** @copydoc DesiredMotionAdapter::DesiredMotionAdapter */
    DesiredMotionAdapter::DesiredMotionAdapter(DesiredMotionAdapter &&other) noexcept = default;

    /** @copydoc DesiredMotionAdapter::operator= */
    DesiredMotionAdapter &DesiredMotionAdapter::operator=(DesiredMotionAdapter &&other) noexcept {
        if (this != &other) {
            Shutdown();
            state_ = std::move(other.state_);
        }
        return *this;
    }

    /** @copydoc DesiredMotionAdapter::Create */
    Result<DesiredMotionAdapter> DesiredMotionAdapter::Create(const DesiredMotionPrincipal principal,
                                                              Character::CharacterCapability capability,
                                                              const Character::CharacterControllerHandle controller,
                                                              DesiredMotionInputBinding binding, const Input::InputRouter *router,
                                                              const Input::InputContextToken *context) {
        if (!principal.permissionGranted || principal.identity == 0)
            return Result<DesiredMotionAdapter>::Failure(MakeError(Character::CharacterErrors::CapabilityUnavailable));
        if (principal.source != DesiredMotionSource::GameplayInput && principal.source != DesiredMotionSource::ExternalIntent)
            return Result<DesiredMotionAdapter>::Failure(MakeError(Character::CharacterErrors::DescriptorInvalid));
        if (const auto descriptor = capability.ControllerDescriptor(controller); descriptor.HasError())
            return Result<DesiredMotionAdapter>::Failure(descriptor.ErrorValue());
        if (!ValidSourceBinding(principal.source, binding, router, context))
            return Result<DesiredMotionAdapter>::Failure(MakeError(Character::CharacterErrors::DescriptorInvalid));
        DesiredMotionAdapter result;
        result.state_ = std::make_unique<State>();
        auto &state = *result.state_;
        state.principal = principal;
        state.capability = std::move(capability);
        state.controller = controller;
        state.binding = std::move(binding);
        if (principal.source == DesiredMotionSource::GameplayInput) {
            state.captureOwner = router;
            state.routing = router->RoutingState(*context);
        }
        return Result<DesiredMotionAdapter>::Success(std::move(result));
    }

    /** @copydoc DesiredMotionAdapter::CaptureInput */
    Result<void> DesiredMotionAdapter::CaptureInput(Input::InputRouter &router, const Input::InputContextToken &context) {
        if (!state_ || state_->principal.source != DesiredMotionSource::GameplayInput)
            return Result<void>::Failure(MakeError(Character::CharacterErrors::CapabilityUnavailable));
        auto &state = *state_;
        if (const auto admitted = state.ValidateRouting(router, context); admitted.HasError())
            return admitted;
        if (!router.Snapshot().window.focused || !router.IsContextActive(context)) {
            state.ClearInput();
            state.hasCapture = true;
            state.capturedFrame = router.Snapshot().frame;
            return Result<void>::Success();
        }
        if (state.hasCapture && router.Snapshot().frame <= state.capturedFrame)
            return Result<void>::Success();
        state.hasCapture = true;
        state.capturedFrame = router.Snapshot().frame;
        return state.CaptureSample(router, context);
    }

    /** @copydoc DesiredMotionAdapter::MakeFrame */
    Result<DesiredMotionFrame> DesiredMotionAdapter::MakeFrame(const std::uint64_t tick, const std::uint64_t correlation,
                                                               const DesiredMotionIntent &intent) {
        if (!state_ || !state_->routingValid)
            return Result<DesiredMotionFrame>::Failure(MakeError(Character::CharacterErrors::CapabilityUnavailable));
        if (tick == 0 || correlation == 0 || tick <= state_->producedTick || correlation <= state_->producedCorrelation)
            return Result<DesiredMotionFrame>::Failure(MakeError(Character::CharacterErrors::CommandOrderInvalid));
        if (!ValidIntent(intent))
            return Result<DesiredMotionFrame>::Failure(MakeError(Character::CharacterErrors::RequestInvalid));
        if (const auto live = state_->capability.ControllerDescriptor(state_->controller); live.HasError())
            return Result<DesiredMotionFrame>::Failure(live.ErrorValue());
        DesiredMotionFrame frame;
        frame.tick_ = tick;
        frame.correlation_ = correlation;
        frame.principal_ = state_->principal.identity;
        frame.player_ = state_->principal.player;
        frame.source_ = state_->principal.source;
        frame.grant_ = state_->capability.Identity();
        frame.controller_ = state_->controller;
        frame.intent_ = intent;
        state_->producedTick = tick;
        state_->producedCorrelation = correlation;
        return Result<DesiredMotionFrame>::Success(frame);
    }

    /** @copydoc DesiredMotionAdapter::ConsumeInput */
    Result<DesiredMotionFrame> DesiredMotionAdapter::ConsumeInput(const std::uint64_t tick, const std::uint64_t correlation) {
        if (!state_ || state_->principal.source != DesiredMotionSource::GameplayInput)
            return Result<DesiredMotionFrame>::Failure(MakeError(Character::CharacterErrors::CapabilityUnavailable));
        auto frame = MakeFrame(tick, correlation, {state_->heldVelocity, {}, state_->pendingJump});
        if (frame.HasValue())
            state_->pendingJump = false;
        return frame;
    }

    /** @copydoc DesiredMotionAdapter::CaptureIntent */
    Result<DesiredMotionFrame> DesiredMotionAdapter::CaptureIntent(const std::uint64_t tick, const std::uint64_t correlation,
                                                                   const DesiredMotionIntent &intent) {
        if (!state_ || state_->principal.source != DesiredMotionSource::ExternalIntent)
            return Result<DesiredMotionFrame>::Failure(MakeError(Character::CharacterErrors::CapabilityUnavailable));
        return MakeFrame(tick, correlation, intent);
    }

    /** @copydoc DesiredMotionAdapter::ValidateSubmissionFrame */
    Result<void> DesiredMotionAdapter::ValidateSubmissionFrame(const DesiredMotionFrame &frame) const {
        if (!state_ || !state_->routingValid)
            return Result<void>::Failure(MakeError(Character::CharacterErrors::CapabilityUnavailable));
        const auto &state = *state_;
        if (!frame.IssuedFor(state.principal, state.capability.Identity(), state.controller))
            return Result<void>::Failure(MakeError(Character::CharacterErrors::CapabilityStale));
        if (frame.tick_ == 0 || frame.correlation_ == 0 || frame.tick_ <= state.submittedTick ||
            frame.correlation_ <= state.submittedCorrelation)
            return Result<void>::Failure(MakeError(Character::CharacterErrors::CommandOrderInvalid));
        return Result<void>::Success();
    }

    /** @copydoc DesiredMotionAdapter::Submit */
    Result<DesiredMotionSubmission> DesiredMotionAdapter::Submit(const DesiredMotionFrame &frame) {
        if (const auto valid = ValidateSubmissionFrame(frame); valid.HasError())
            return Result<DesiredMotionSubmission>::Failure(valid.ErrorValue());
        auto &state = *state_;
        DesiredMotionSubmission submission;
        submission.absent = Absent(frame.intent_);
        if (submission.absent) {
            const auto live = state.capability.ControllerDescriptor(state.controller);
            if (live.HasError())
                return Result<DesiredMotionSubmission>::Failure(live.ErrorValue());
        } else {
            Character::CharacterMovementRequest request{.controller = frame.controller_,
                                                        .tick = frame.tick_,
                                                        .sequence = frame.correlation_,
                                                        .desiredVelocityMetersPerSecond = frame.intent_.velocityMetersPerSecond,
                                                        .desiredHeading = frame.intent_.heading,
                                                        .jumpRequested = frame.intent_.jumpRequested};
            const auto admitted = state.capability.QueueMovementCommand(request);
            if (admitted.HasError())
                return Result<DesiredMotionSubmission>::Failure(admitted.ErrorValue());
            submission.admission = admitted.Value();
            if (submission.admission.status != Character::CharacterCommandAdmissionStatus::Deferred)
                return Result<DesiredMotionSubmission>::Success(submission);
        }
        state.submittedTick = frame.tick_;
        state.submittedCorrelation = frame.correlation_;
        return Result<DesiredMotionSubmission>::Success(submission);
    }

    /** @copydoc DesiredMotionAdapter::Shutdown */
    void DesiredMotionAdapter::Shutdown() noexcept {
        if (state_)
            state_->capability.Revoke();
        state_.reset();
    }
}  // namespace Horo::CharacterInput
