#include "Horo/XR/XRCharacterLocomotion.h"

#include "Horo/XR/XRErrors.h"

#include <cmath>
#include <new>
#include <utility>

namespace Horo::XR {
    namespace {
        using Submission = Result<XRLocomotionAdmission>;

        /** @brief Projects a typed locomotion admission error into the caller's result. */
        template <typename T> Result<T> Reject(const ErrorCodeDescriptor &error) {
            return Result<T>::Failure(MakeError(error));
        }

        /** @brief Uses double products so enormous finite input cannot overflow the speed or heading comparison. */
        double Square(float value) noexcept {
            const auto wide = static_cast<double>(value);
            return wide * wide;
        }

        /** @brief Tests a destination against the typed finite-coordinate boundary. */
        bool Finite(Math::Vec3 point) noexcept {
            return std::isfinite(point.x) && std::isfinite(point.y) && std::isfinite(point.z);
        }

        /** @brief Applies the Character heading normalization tolerance without normalizing malformed input. */
        bool UnitHeading(Math::Quaternion heading) noexcept {
            const auto length = Square(heading.x) + Square(heading.y) + Square(heading.z) + Square(heading.w);
            return std::abs(length - 1.0) <= 0.001;
        }

        /** @brief Tests the selected reference against the exact tracking session and origin. */
        bool MatchesSpace(const XRCoordinateSpace &space, const XRInteractionCutoff &tracking) noexcept {
            return space.id.IsValid() && space.id.session == tracking.session && space.worldOriginRevision == tracking.origin;
        }

        /** @brief Validates the copied tick fields against the live scheduler issuer before touching any owner. */
        bool MatchesDispatch(const Runtime::RuntimeDispatchSource &source, const Runtime::FixedStepContext &step,
                             std::uint64_t tick) noexcept {
            Runtime::RuntimeDispatchFacts facts;
            return step.dispatchEvidence.Read(source, Runtime::RuntimePhase::FixedUpdate, facts) == Runtime::RuntimeDispatchStatus::Valid &&
                   facts.fixedTick == tick && facts.fixedTick == step.simulationTick && facts.fixedAttempt == step.attemptNumber &&
                   facts.frame == step.frameNumber && facts.fixedDuration == step.fixedDelta;
        }

        /** @brief Copies the Character queue decision without retaining a controller observation. */
        XRLocomotionAdmission MotionAdmission(Character::CharacterCommandAdmissionStatus status) noexcept {
            using enum Character::CharacterCommandAdmissionStatus;
            using enum XRLocomotionAdmission;
            switch (status) {
                case Deferred:
                    return Queued;
                case RejectedBusy:
                    return Busy;
                case RejectedFull:
                    return Full;
            }
            return Full;
        }

        /** @brief Denies recursive admission for the duration of an owner callback. */
        class SubmissionScope final {
        public:
            explicit SubmissionScope(bool &busy) noexcept : busy_(&busy) {
                busy = true;
            }

            SubmissionScope(const SubmissionScope &) = delete;
            SubmissionScope &operator=(const SubmissionScope &) = delete;

            ~SubmissionScope() {
                *busy_ = false;
            }

        private:
            bool *const busy_;
        };
    }  // namespace

    struct XRCharacterLocomotionAdapter::State final {
        State(XRCharacterLocomotionBinding selected, Character::CharacterCapability grant)
            : binding(std::move(selected)), capability(std::move(grant)) {}

        State(const State &) = delete;
        State &operator=(const State &) = delete;

        /** @brief Cancels deferred owner work without recalling frozen Character commands. */
        void Revoke() const noexcept {
            lifetime.RequestCancellation();
            capability.Revoke();
        }

        /** @brief Checks current assignment, focus and tracking provenance without reading an input device. */
        Result<void> ValidateScope(const XRLocomotionScope &scope, const XRTrackingSnapshot &tracking, std::uint64_t assignment) const {
            if (assignment != binding.assignmentRevision || scope.assignmentRevision != assignment || scope.player != binding.player)
                return Reject<void>(XRErrors::IdentityStale);
            if (scope.tracking.session != binding.session || scope.tracking != DescribeXRInteractionCutoff(tracking))
                return Reject<void>(XRErrors::TrackingSnapshotStale);
            if (tracking.Focus() != XRTrackingFocusState::Focused || tracking.Visibility() != XRTrackingVisibilityState::Visible ||
                tracking.State() != XRTrackingState::Active)
                return Reject<void>(XRErrors::OperationUnavailable);
            return capability.Validate();
        }

        /** @brief Submits velocity and heading to the sole Character queue after applying the host speed limit. */
        Submission Queue(const XRLocomotionIntent &intent, const XRLocomotionMotion &motion, std::uint64_t characterTick) const {
            if (!motion.velocityMetersPerSecond && !motion.heading)
                return Reject<XRLocomotionAdmission>(XRErrors::OperationInvalid);
            if (motion.heading && !UnitHeading(*motion.heading))
                return Reject<XRLocomotionAdmission>(XRErrors::PoseInvalid);
            if (motion.velocityMetersPerSecond) {
                const auto &velocity = *motion.velocityMetersPerSecond;
                if (!Finite(velocity))
                    return Reject<XRLocomotionAdmission>(XRErrors::PoseInvalid);
                if (Square(velocity.x) + Square(velocity.y) + Square(velocity.z) > Square(binding.maximumSpeedMetersPerSecond))
                    return Reject<XRLocomotionAdmission>(XRErrors::CapacityExceeded);
            }
            const Character::CharacterMovementRequest request{.controller = binding.controller,
                                                              .tick = characterTick,
                                                              .sequence = intent.sequence,
                                                              .desiredVelocityMetersPerSecond = motion.velocityMetersPerSecond,
                                                              .desiredHeading = motion.heading};
            const auto queued = capability.QueueMovementCommand(request);
            return queued.HasValue() ? Submission::Success(MotionAdmission(queued.Value().status))
                                     : Submission::Failure(queued.ErrorValue());
        }

        /** @brief Forwards a scoped destination for Character clearance and placement at its own safe point. */
        Submission Queue(const XRLocomotionIntent &intent, const XRLocomotionTeleport &teleport, std::uint64_t characterTick) const {
            if (teleport.space.kind != XRSpaceKind::World || !MatchesSpace(teleport.space, intent.scope.tracking))
                return Reject<XRLocomotionAdmission>(XRErrors::CoordinateSpaceIncompatible);
            if (teleport.destinationRevision == 0 || !Finite(teleport.position) || !UnitHeading(teleport.heading))
                return Reject<XRLocomotionAdmission>(XRErrors::PoseInvalid);
            const auto owner = binding.teleport.lock();
            if (!owner)
                return Reject<XRLocomotionAdmission>(XRErrors::OperationUnsupported);
            const Character::CharacterTeleportRequest request{binding.controller, characterTick, teleport.position, teleport.heading};
            return owner->QueueTeleport(intent, request, lifetime.Token());
        }

        /** @brief Forwards reference replacement only to the selected application calibration owner. */
        Submission Queue(const XRLocomotionIntent &intent, const XRLocomotionRecenter &recenter, std::uint64_t) const {
            if (const auto kind = recenter.reference.kind;
                (kind != XRSpaceKind::Local && kind != XRSpaceKind::Stage) || !MatchesSpace(recenter.reference, intent.scope.tracking))
                return Reject<XRLocomotionAdmission>(XRErrors::CoordinateSpaceIncompatible);
            const auto owner = binding.recenter.lock();
            return owner ? owner->QueueRecenter(intent, lifetime.Token()) : Reject<XRLocomotionAdmission>(XRErrors::OperationUnsupported);
        }

        /** @brief Contains foreign owners and advances watermarks only after a command was copied. */
        Submission Dispatch(const XRLocomotionIntent &intent, std::uint64_t characterTick) {
            const SubmissionScope scope{busy};
            try {
                auto submitted = std::visit([this, &intent, characterTick](const auto &operation) {
                    return Queue(intent, operation, characterTick);
                }, intent.operation);
                if (submitted.HasValue() && submitted.Value() == XRLocomotionAdmission::Queued) {
                    lastTick = intent.tick;
                    lastSequence = intent.sequence;
                }
                return submitted;
            } catch (const std::bad_alloc &) {
                return Reject<XRLocomotionAdmission>(XRErrors::CapacityExceeded);
            } catch (...) {
                // Foreign owner implementations can throw non-standard exceptions.
                return Reject<XRLocomotionAdmission>(XRErrors::OperationUnavailable);
            }
        }

        const XRCharacterLocomotionBinding binding;
        Character::CharacterCapability capability;
        CancellationSource lifetime;
        std::uint64_t lastTick{};
        std::uint64_t lastSequence{};
        bool busy{};
    };

    XRCharacterLocomotionAdapter::XRCharacterLocomotionAdapter(std::shared_ptr<State> state) noexcept : state_(std::move(state)) {}

    XRCharacterLocomotionAdapter::~XRCharacterLocomotionAdapter() {
        Shutdown();
    }

    XRCharacterLocomotionAdapter &XRCharacterLocomotionAdapter::operator=(XRCharacterLocomotionAdapter &&other) noexcept {
        if (this != &other) {
            Shutdown();
            state_ = std::move(other.state_);
        }
        return *this;
    }

    /** @copydoc XRCharacterLocomotionAdapter::Create */
    Result<XRCharacterLocomotionAdapter> XRCharacterLocomotionAdapter::Create(XRCharacterLocomotionBinding binding,
                                                                              const Character::CharacterCapability &authority) {
        if (binding.scheduler.BindingStatus() != Runtime::RuntimeDispatchStatus::Valid)
            return Reject<XRCharacterLocomotionAdapter>(XRErrors::IdentityStale);
        return CreateValidated(std::move(binding), authority);
    }

    /** @copydoc XRCharacterLocomotionAdapter::CreateForFixedStep */
    Result<XRCharacterLocomotionAdapter> XRCharacterLocomotionAdapter::CreateForFixedStep(XRCharacterLocomotionBinding binding,
                                                                                          const Character::CharacterCapability &authority,
                                                                                          const Runtime::FixedStepContext &step) {
        if (!MatchesDispatch(binding.scheduler, step, step.simulationTick) || step.cancellation.IsCancellationRequested())
            return Reject<XRCharacterLocomotionAdapter>(XRErrors::IdentityStale);
        return CreateValidated(std::move(binding), authority);
    }

    Result<XRCharacterLocomotionAdapter> XRCharacterLocomotionAdapter::CreateValidated(XRCharacterLocomotionBinding binding,
                                                                                       const Character::CharacterCapability &authority) {
        using Outcome = Result<XRCharacterLocomotionAdapter>;
        if (!binding.session.IsValid() || binding.assignmentRevision == 0 || !std::isfinite(binding.maximumSpeedMetersPerSecond) ||
            binding.maximumSpeedMetersPerSecond < 0.0F)
            return Reject<XRCharacterLocomotionAdapter>(XRErrors::OperationInvalid);
        if (auto controller = authority.ControllerDescriptor(binding.controller); controller.HasError())
            return Outcome::Failure(controller.ErrorValue());
        auto grant = authority.Derive();
        if (grant.HasError())
            return Outcome::Failure(grant.ErrorValue());
        try {
            return Outcome::Success(XRCharacterLocomotionAdapter{std::make_shared<State>(std::move(binding), grant.Value())});
        } catch (const std::bad_alloc &) {
            grant.Value().Revoke();
            return Reject<XRCharacterLocomotionAdapter>(XRErrors::CapacityExceeded);
        }
    }

    /** @copydoc XRCharacterLocomotionAdapter::Submit */
    Submission XRCharacterLocomotionAdapter::Submit(const XRLocomotionIntent &intent, const XRTrackingSnapshot &tracking,
                                                    std::uint64_t assignmentRevision, const Runtime::FixedStepContext &step,
                                                    std::optional<std::uint64_t> characterTick) {
        const auto state = state_;
        if (!state || !MatchesDispatch(state->binding.scheduler, step, intent.tick))
            return Reject<XRLocomotionAdmission>(XRErrors::IdentityStale);
        if (step.cancellation.IsCancellationRequested() || state->lifetime.Token().IsCancellationRequested())
            return Reject<XRLocomotionAdmission>(XRErrors::OperationUnavailable);
        if (state->busy)
            return Submission::Success(XRLocomotionAdmission::Busy);
        if (intent.tick <= state->lastTick || intent.sequence <= state->lastSequence)
            return Reject<XRLocomotionAdmission>(XRErrors::IdentityStale);
        if (auto live = state->ValidateScope(intent.scope, tracking, assignmentRevision); live.HasError())
            return Submission::Failure(live.ErrorValue());
        const auto target = characterTick.value_or(intent.tick);
        if (target == 0)
            return Reject<XRLocomotionAdmission>(XRErrors::OperationInvalid);
        return state->Dispatch(intent, target);
    }

    XRLocomotionComfortHint::XRLocomotionComfortHint(XRLocomotionScope scope, Runtime::RuntimeDispatchSource scheduler,
                                                     CancellationToken lifetime, const Runtime::FixedStepContext &step,
                                                     float opacity) noexcept
        : scope_(scope), scheduler_(std::move(scheduler)), lifetime_(std::move(lifetime)), tick_(step.simulationTick),
          attempt_(step.attemptNumber), dispatchFrame_(step.frameNumber), duration_(step.fixedDelta), opacity_(opacity) {}

    /** @copydoc XRLocomotionComfortHint::ReadForPresentation */
    std::optional<float> XRLocomotionComfortHint::ReadForPresentation(const XRLocomotionScope &current,
                                                                      const Runtime::FrameContext &frame) const noexcept {
        if (scope_ != current || lifetime_.IsCancellationRequested() || frame.cancellation.IsCancellationRequested())
            return std::nullopt;
        Runtime::RuntimeDispatchFacts facts;
        if (frame.dispatchEvidence.Read(scheduler_, Runtime::RuntimePhase::RenderExecution, facts) != Runtime::RuntimeDispatchStatus::Valid)
            return std::nullopt;
        if (facts.frame != frame.frameNumber || facts.committedTick != tick_ || facts.committedAttempt != attempt_ ||
            facts.committedFrame != dispatchFrame_ || facts.committedDuration != duration_)
            return std::nullopt;
        return opacity_;
    }

    /** @copydoc XRCharacterLocomotionAdapter::SubmitFrame */
    Result<XRLocomotionFrameAdmission> XRCharacterLocomotionAdapter::SubmitFrame(const XRLocomotionFrame &frame,
                                                                                 const XRTrackingSnapshot &tracking,
                                                                                 std::uint64_t assignmentRevision,
                                                                                 const Runtime::FixedStepContext &step,
                                                                                 std::optional<std::uint64_t> characterTick) {
        using Outcome = Result<XRLocomotionFrameAdmission>;
        const auto opacity = frame.vignetteOpacity;
        if (!std::isfinite(opacity) || opacity < 0.0F || opacity > 1.0F)
            return Reject<XRLocomotionFrameAdmission>(XRErrors::OperationInvalid);
        if (!frame.intent)
            return opacity == 0.0F ? Outcome::Success({}) : Reject<XRLocomotionFrameAdmission>(XRErrors::OperationInvalid);
        if (opacity > 0.0F) {
            const auto *motion = std::get_if<XRLocomotionMotion>(&frame.intent->operation);
            if (!motion || !motion->velocityMetersPerSecond || *motion->velocityMetersPerSecond == Math::Vec3{})
                return Reject<XRLocomotionFrameAdmission>(XRErrors::OperationInvalid);
        }
        const auto state = state_;
        const auto admitted = Submit(*frame.intent, tracking, assignmentRevision, step, characterTick);
        if (admitted.HasError())
            return Outcome::Failure(admitted.ErrorValue());
        XRLocomotionFrameAdmission admission{.command = admitted.Value()};
        if (opacity > 0.0F && admitted.Value() == XRLocomotionAdmission::Queued) {
            admission.comfort =
                XRLocomotionComfortHint{frame.intent->scope, state->binding.scheduler, state->lifetime.Token(), step, opacity};
        }
        return Outcome::Success(std::move(admission));
    }

    /** @copydoc XRCharacterLocomotionAdapter::Shutdown */
    void XRCharacterLocomotionAdapter::Shutdown() noexcept {
        if (auto state = std::exchange(state_, {}))
            state->Revoke();
    }
}  // namespace Horo::XR
