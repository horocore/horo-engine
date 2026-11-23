#include "Horo/Navigation/NavigationVelocityPipeline.h"

#include "NavigationCrowdOutcomeValidation.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <new>

namespace Horo::Navigation {
    namespace {
        /** @brief Compute length without overflowing finite float intermediates. */
        double ComputeMotionMagnitude(const Math::Vec3 value) noexcept {
            return std::hypot(static_cast<double>(value.x), static_cast<double>(value.y), static_cast<double>(value.z));
        }

        /** @brief Reduces route speed and the solver speed budget, retaining room to brake within this tick's envelope. */
        void ApplyDensityPolicy(NavigationAvoidanceRequest &request, const NavigationCrowdAgentFact &agent) noexcept {
            if (agent.densitySpeedScale == 1.0F)
                return;
            request.preferredVelocity = request.preferredVelocity * agent.densitySpeedScale;
            const auto maximum = static_cast<double>(request.maximumSpeedMetersPerSecond);
            const auto speed = std::hypot(static_cast<double>(agent.velocity.x), static_cast<double>(agent.velocity.z));
            const auto braking = static_cast<double>(request.maximumAccelerationMetersPerSecondSquared) * request.stepSeconds;
            const auto limit = std::clamp(speed - braking, maximum * agent.densitySpeedScale, maximum);
            request.maximumSpeedMetersPerSecond = std::max(std::numeric_limits<float>::denorm_min(), static_cast<float>(limit));
        }

    }  // namespace

    /** @copydoc Detail::ValidCrowdEnvelope */
    bool Detail::ValidCrowdEnvelope(const NavigationAvoidanceRequest &request, const float step) noexcept {
        return std::isfinite(request.maximumSpeedMetersPerSecond) && request.maximumSpeedMetersPerSecond > 0 &&
               std::isfinite(request.maximumAccelerationMetersPerSecondSquared) && request.maximumAccelerationMetersPerSecondSquared > 0 &&
               std::isfinite(request.horizonSeconds) && request.horizonSeconds > 0 && std::isfinite(step) && step > 0 &&
               request.stepSeconds == step;
    }

    /** @copydoc Detail::ValidCrowdOutcome */
    bool Detail::ValidCrowdOutcome(const NavigationAvoidanceOutcome &outcome, const NavigationAvoidanceRequest &request,
                                   const Math::Vec3 capturedVelocity) noexcept {
        using enum NavigationAvoidanceDisposition;
        using enum NavigationAvoidanceStopReason;
        if (!Math::IsFinite(outcome.desiredVelocity) || outcome.desiredVelocity.y != 0 ||
            ComputeMotionMagnitude(outcome.desiredVelocity) > static_cast<double>(request.maximumSpeedMetersPerSecond) + 0.00001 ||
            (outcome.disposition != Sampled && outcome.disposition != CollisionSafeStop) || outcome.stopReason > CadenceHold ||
            (outcome.disposition == Sampled && outcome.stopReason != None) ||
            (outcome.disposition == CollisionSafeStop && (outcome.desiredVelocity != Math::Vec3{} || outcome.stopReason == None)))
            return false;
        if (const auto planarDelta = std::hypot(static_cast<double>(outcome.desiredVelocity.x) - capturedVelocity.x,
                                                static_cast<double>(outcome.desiredVelocity.z) - capturedVelocity.z);
            outcome.disposition == Sampled &&
            planarDelta > static_cast<double>(request.maximumAccelerationMetersPerSecondSquared) * request.stepSeconds + 0.00001)
            return false;
        return true;
    }

    /** @copydoc NavigationVelocityPipeline::Create */
    Result<std::unique_ptr<NavigationVelocityPipeline>> NavigationVelocityPipeline::Create(NavigationMovementController &movement,
                                                                                           const NavigationAgentSceneBinding binding,
                                                                                           const CrowdAgentHandle agent,
                                                                                           const std::uint64_t characterRevision,
                                                                                           const NavigationVelocityPolicy policy) {
        using Return = Result<std::unique_ptr<NavigationVelocityPipeline>>;
        if (!binding.IsValid() || !agent.IsValid() || agent.world != binding.world || !characterRevision || movement.Agent() != agent ||
            movement.Binding() != binding || !policy.blockedTicksBeforeRepath || policy.blockedTicksBeforeRepath > 1024 ||
            !policy.repathCooldownTicks || policy.repathCooldownTicks > 1'000'000 || !std::isfinite(policy.minimumProgressMeters) ||
            policy.minimumProgressMeters < 0)
            return Return::Failure(MakeError(NavigationErrors::AgentDescriptorInvalid));
        try {
            return Return::Success(
                std::make_unique<NavigationVelocityPipeline>(ConstructionKey{}, movement, binding, agent, characterRevision, policy));
        } catch (const std::bad_alloc &) {
            return Return::Failure(MakeError(NavigationErrors::CapacityExceeded));
        }
    }

    /** @copydoc NavigationVelocityPipeline::NavigationVelocityPipeline */
    NavigationVelocityPipeline::NavigationVelocityPipeline(ConstructionKey &&, NavigationMovementController &movement,
                                                           const NavigationAgentSceneBinding binding, const CrowdAgentHandle agent,
                                                           const std::uint64_t revision, const NavigationVelocityPolicy policy) noexcept
        : movement_(&movement), binding_(binding), agent_(agent), characterRevision_(revision), policy_(policy),
          ownerThread_(std::this_thread::get_id()) {}

    /** @copydoc NavigationVelocityPipeline::RetirePreviousOperation */
    void NavigationVelocityPipeline::RetirePreviousOperation() noexcept {
        const auto operation = movement_->Status().id;
        if ((pending_ && pending_->preferred.operation != operation) || (publication_ && publication_->intent.operation != operation)) {
            // A new goal retires old operation authority without inventing a new Character incarnation.
            pending_.reset();
            pendingCapture_.reset();
            publication_.reset();
            feedback_.reset();
            blockedTicks_ = 0;
            lastRepathTick_.reset();
        }
    }

    /** @copydoc NavigationVelocityPipeline::PrepareFeedback */
    Result<NavigationVelocityPipeline::PreparedFeedback> NavigationVelocityPipeline::PrepareFeedback(
        const NavigationMovementObservation &current) const {
        using Return = Result<PreparedFeedback>;
        auto observation = current;
        auto nextBlocked = feedback_ ? blockedTicks_ : 0U;
        bool repath = false;
        if (feedback_) {
            if (!current.path.tick || feedback_->tick != current.path.tick - 1 || feedback_->operation != movement_->Status().id)
                return Return::Failure(MakeError(NavigationErrors::StaleSnapshot));
            const auto progress = ComputeMotionMagnitude(feedback_->achievedDisplacement);
            if (!publication_ || publication_->stopReason != NavigationAvoidanceStopReason::CadenceHold) {
                const bool stalled = feedback_->blocked && progress <= policy_.minimumProgressMeters && publication_ &&
                                     publication_->intent.state == NavigationMovementState::Following &&
                                     publication_->preferredVelocity != Math::Vec3{};
                nextBlocked = stalled ? std::min(blockedTicks_ + 1, policy_.blockedTicksBeforeRepath) : 0;
                repath = nextBlocked >= policy_.blockedTicksBeforeRepath &&
                         (!lastRepathTick_.has_value() || current.path.tick - *lastRepathTick_ >= policy_.repathCooldownTicks);
            }
            observation.feedback = NavigationMovementFeedback{feedback_->operation,
                                                              feedback_->path,
                                                              feedback_->tick,
                                                              static_cast<float>(progress / publishedStepSeconds_),
                                                              feedback_->achievedDisplacement,
                                                              repath};
        }
        return Return::Success(PreparedFeedback{observation, nextBlocked, repath});
    }

    /** @copydoc NavigationVelocityPipeline::Prepare */
    Result<NavigationVelocityWork> NavigationVelocityPipeline::Prepare(const NavigationMovementObservation &current,
                                                                       const NavigationCrowdSnapshot &snapshot,
                                                                       NavigationAvoidanceRequest request) {
        using Return = Result<NavigationVelocityWork>;
        if (std::this_thread::get_id() != ownerThread_)
            return Return::Failure(MakeError(NavigationErrors::AgentDescriptorInvalid));
        if (retired_)
            return Return::Failure(MakeError(NavigationErrors::AdmissionRejected));
        RetirePreviousOperation();
        if (pending_)
            return Return::Failure(MakeError(NavigationErrors::AdmissionRejected));
        if (!snapshot.IsValid() || snapshot.Binding() != binding_ || current.binding != binding_ ||
            snapshot.CaptureTick() != current.path.tick || (lastTick_.has_value() && current.path.tick <= *lastTick_) ||
            request.agentIndex >= snapshot.Agents().size() || snapshot.Agents()[request.agentIndex].handle != agent_)
            return Return::Failure(MakeError(NavigationErrors::StaleSnapshot));
        if (current.feedback || !Detail::ValidCrowdEnvelope(request, current.stepSeconds))
            return Return::Failure(MakeError(NavigationErrors::OutcomeDescriptorInvalid));
        if (current.position != snapshot.Agents()[request.agentIndex].position)
            return Return::Failure(MakeError(NavigationErrors::StaleSnapshot));
        if (sequence_ == std::numeric_limits<std::uint64_t>::max())
            return Return::Failure(MakeError(NavigationErrors::GenerationExhausted));
        const auto preparedFeedback = PrepareFeedback(current);
        if (preparedFeedback.HasError())
            return Return::Failure(preparedFeedback.ErrorValue());
        const auto &feedback = preparedFeedback.Value();
        auto preferred = movement_->Update(feedback.observation);
        if (preferred.HasError())
            return Return::Failure(preferred.ErrorValue());
        if (preferred.Value().operation.agent != agent_ && preferred.Value().operation.sequence != 0)
            return Return::Failure(MakeError(NavigationErrors::InvalidHandle));
        // Avoidance is planar. A vertical corridor request is explicit unsupported capability, never flattened silently.
        if (preferred.Value().desiredVelocity.y != 0)
            return Return::Failure(MakeError(NavigationErrors::OperationUnsupported));
        request.preferredVelocity = preferred.Value().desiredVelocity;
        if (const auto speed = ComputeMotionMagnitude(request.preferredVelocity); speed > request.maximumSpeedMetersPerSecond)
            request.preferredVelocity = request.preferredVelocity * static_cast<float>(request.maximumSpeedMetersPerSecond / speed);
        ApplyDensityPolicy(request, snapshot.Agents()[request.agentIndex]);
        using enum NavigationCrowdRecovery;
        const auto progress = feedback.blockedTicks != 0 ? Stalled : Clear;
        const auto recovery = feedback.repath ? RepathRequested : progress;
        pending_ = NavigationVelocityWork{++sequence_,        preferred.Value(), request, binding_, snapshot.DynamicRevision(),
                                          characterRevision_, recovery};
        pendingCapture_ = snapshot;
        lastTick_ = current.path.tick;
        blockedTicks_ = feedback.blockedTicks;
        if (feedback.repath) {
            lastRepathTick_ = current.path.tick;
            blockedTicks_ = 0;
        }
        feedback_.reset();
        publication_.reset();
        return Return::Success(*pending_);
    }

    /** @copydoc NavigationVelocityPipeline::Commit */
    Result<NavigationVelocityPublication> NavigationVelocityPipeline::Commit(const NavigationVelocityWork &work,
                                                                             const NavigationAvoidanceOutcome &outcome,
                                                                             const NavigationCrowdSnapshot &current,
                                                                             const NavigationMovementObservation &observation) {
        using Return = Result<NavigationVelocityPublication>;
        if (std::this_thread::get_id() != ownerThread_)
            return Return::Failure(MakeError(NavigationErrors::AgentDescriptorInvalid));
        if (retired_)
            return Return::Failure(MakeError(NavigationErrors::AdmissionRejected));
        if (!pending_ || work.sequence != pending_->sequence || work.binding != binding_ || work.characterRevision != characterRevision_ ||
            work.preferred.operation != pending_->preferred.operation || work.preferred.path != pending_->preferred.path ||
            work.preferred.tick != pending_->preferred.tick || work.dynamicRevision != pending_->dynamicRevision || !current.IsValid() ||
            current.Binding() != binding_ || current.CaptureTick() != pending_->preferred.tick ||
            current.DynamicRevision() != pending_->dynamicRevision || !pendingCapture_ ||
            current.Agents().data() != pendingCapture_->Agents().data() || pending_->request.agentIndex >= current.Agents().size() ||
            current.Agents()[pending_->request.agentIndex].handle != agent_ || outcome.binding != binding_ ||
            outcome.captureTick != pending_->preferred.tick || outcome.dynamicRevision != pending_->dynamicRevision ||
            !movement_->IsCurrentIntent(pending_->preferred))
            return Return::Failure(MakeError(NavigationErrors::StaleSnapshot));
        if (observation.position != current.Agents()[pending_->request.agentIndex].position ||
            observation.stepSeconds != pending_->request.stepSeconds)
            return Return::Failure(MakeError(NavigationErrors::StaleSnapshot));
        if (const auto valid = movement_->ValidateIntentAtSafePoint(pending_->preferred, observation); valid.HasError())
            return Return::Failure(valid.ErrorValue());
        if (!Detail::ValidCrowdOutcome(outcome, pending_->request, current.Agents()[pending_->request.agentIndex].velocity))
            return Return::Failure(MakeError(NavigationErrors::OutcomeDescriptorInvalid));
        auto intent = pending_->preferred;
        intent.desiredVelocity = intent.state == NavigationMovementState::Following && intent.desiredVelocity != Math::Vec3{}
                                     ? outcome.desiredVelocity
                                     : Math::Vec3{};
        publication_ = NavigationVelocityPublication{intent,
                                                     binding_,
                                                     characterRevision_,
                                                     outcome.disposition,
                                                     outcome.stopReason,
                                                     pending_->request.preferredVelocity,
                                                     pending_->recovery};
        publishedStepSeconds_ = pending_->request.stepSeconds;
        pending_.reset();
        pendingCapture_.reset();
        return Return::Success(*publication_);
    }

    /** @copydoc NavigationVelocityPipeline::ValidatePublicationAtSafePoint */
    Result<void> NavigationVelocityPipeline::ValidatePublicationAtSafePoint(const NavigationVelocityPublication &publication,
                                                                            const NavigationMovementObservation &current) const {
        if (std::this_thread::get_id() != ownerThread_)
            return Result<void>::Failure(MakeError(NavigationErrors::AgentDescriptorInvalid));
        if (retired_)
            return Result<void>::Failure(MakeError(NavigationErrors::AdmissionRejected));
        if (!publication_ || publication.binding != binding_ || publication.characterRevision != characterRevision_ ||
            publication.intent.operation != publication_->intent.operation || publication.intent.path != publication_->intent.path ||
            publication.intent.tick != publication_->intent.tick || publication.intent.state != publication_->intent.state ||
            publication.intent.desiredVelocity != publication_->intent.desiredVelocity ||
            publication.disposition != publication_->disposition || publication.stopReason != publication_->stopReason ||
            publication.preferredVelocity != publication_->preferredVelocity || publication.recovery != publication_->recovery ||
            current.stepSeconds != publishedStepSeconds_)
            return Result<void>::Failure(MakeError(NavigationErrors::StaleSnapshot));
        return movement_->ValidateIntentAtSafePoint(publication.intent, current);
    }

    /** @copydoc NavigationVelocityPipeline::CancelPending */
    bool NavigationVelocityPipeline::CancelPending(const NavigationVelocityWork &work) noexcept {
        if (std::this_thread::get_id() != ownerThread_)
            return false;
        if (!pending_ || work.sequence != pending_->sequence || work.characterRevision != characterRevision_ || work.binding != binding_)
            return false;
        pending_.reset();
        pendingCapture_.reset();
        return true;
    }

    /** @copydoc NavigationVelocityPipeline::ObserveFeedback */
    Result<void> NavigationVelocityPipeline::ObserveFeedback(const NavigationVelocityFeedback &feedback) {
        if (std::this_thread::get_id() != ownerThread_)
            return Result<void>::Failure(MakeError(NavigationErrors::AgentDescriptorInvalid));
        if (retired_)
            return Result<void>::Failure(MakeError(NavigationErrors::AdmissionRejected));
        if (!publication_ || feedback_ || feedback.operation != publication_->intent.operation ||
            feedback.path != publication_->intent.path || feedback.tick != publication_->intent.tick ||
            feedback.characterRevision != characterRevision_ || movement_->Status().id != feedback.operation)
            return Result<void>::Failure(MakeError(NavigationErrors::StaleSnapshot));
        if (!Math::IsFinite(feedback.achievedDisplacement) ||
            ComputeMotionMagnitude(feedback.achievedDisplacement) / publishedStepSeconds_ > std::numeric_limits<float>::max())
            return Result<void>::Failure(MakeError(NavigationErrors::OutcomeDescriptorInvalid));
        feedback_ = feedback;
        return Result<void>::Success();
    }

    /** @copydoc NavigationVelocityPipeline::AdmitDiscontinuity */
    Result<void> NavigationVelocityPipeline::AdmitDiscontinuity(const NavigationMovementObservation &current,
                                                                const std::uint64_t characterRevision) const {
        if (std::this_thread::get_id() != ownerThread_)
            return Result<void>::Failure(MakeError(NavigationErrors::AgentDescriptorInvalid));
        if (retired_ || characterRevision <= characterRevision_ || current.feedback.has_value())
            return Result<void>::Failure(MakeError(NavigationErrors::StaleSnapshot));
        return Result<void>::Success();
    }

    /** @copydoc NavigationVelocityPipeline::Reset */
    Result<void> NavigationVelocityPipeline::Reset(const NavigationMovementObservation &current, const std::uint64_t characterRevision) {
        if (const auto admitted = AdmitDiscontinuity(current, characterRevision); admitted.HasError())
            return admitted;
        if (current.binding != binding_)
            return Result<void>::Failure(MakeError(NavigationErrors::StaleSnapshot));
        if (const auto reset = movement_->ResetMotionAtSafePoint(current); reset.HasError())
            return Result<void>::Failure(reset.ErrorValue());
        characterRevision_ = characterRevision;
        ClearMotionHistory();
        return Result<void>::Success();
    }

    /** @copydoc NavigationVelocityPipeline::Rebind */
    Result<void> NavigationVelocityPipeline::Rebind(const CrowdAgentHandle agent, const NavigationPathCaller caller,
                                                    const NavigationMovementObservation &current, const std::uint64_t characterRevision) {
        if (const auto admitted = AdmitDiscontinuity(current, characterRevision); admitted.HasError())
            return admitted;
        if (const auto rebound = movement_->RebindAtSafePoint(agent, caller, current); rebound.HasError())
            return Result<void>::Failure(rebound.ErrorValue());
        agent_ = agent;
        binding_ = current.binding;
        characterRevision_ = characterRevision;
        ClearMotionHistory();
        return Result<void>::Success();
    }

    /** @copydoc NavigationVelocityPipeline::ClearMotionHistory */
    void NavigationVelocityPipeline::ClearMotionHistory() noexcept {
        pending_.reset();
        pendingCapture_.reset();
        publication_.reset();
        feedback_.reset();
        blockedTicks_ = 0;
        lastRepathTick_.reset();
    }

    /** @copydoc NavigationVelocityPipeline::SuspendAtSafePoint */
    Result<void> NavigationVelocityPipeline::SuspendAtSafePoint(const NavigationMovementObservation &current) {
        if (std::this_thread::get_id() != ownerThread_ || retired_)
            return Result<void>::Failure(MakeError(NavigationErrors::AdmissionRejected));
        if (const auto reset = movement_->ResetMotionAtSafePoint(current); reset.HasError())
            return reset;
        movement_->CollectRetiredAtSafePoint();
        ClearMotionHistory();
        return Result<void>::Success();
    }

    /** @copydoc NavigationVelocityPipeline::Retire */
    void NavigationVelocityPipeline::Retire() noexcept {
        if (std::this_thread::get_id() != ownerThread_)
            return;
        retired_ = true;
        ClearMotionHistory();
    }
}  // namespace Horo::Navigation
