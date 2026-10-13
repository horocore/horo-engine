#include "Horo/Navigation/NavigationSceneAgentLifecycle.h"

#include <new>
#include <utility>

namespace Horo::Navigation {
    /** @copydoc NavigationSceneAgentLifecycle::LiveEntity */
    bool NavigationSceneAgentLifecycle::LiveEntity(const Runtime::RuntimeSceneView scene, const Runtime::EntityRef entity) {
        if (!scene.IsCurrent() || scene.RuntimeId() != entity.runtime)
            return false;
        const auto value = scene.Get(entity);
        return value.HasValue() && value.Value().components && value.Value().components->navigationAgent &&
               value.Value().components->navigationAgent->enabled;
    }

    /** @copydoc NavigationSceneAgentLifecycle::Create */
    Result<std::unique_ptr<NavigationSceneAgentLifecycle>> NavigationSceneAgentLifecycle::Create(
        NavigationAgentRegistry &registry, NavigationWorldLifecycle &world, NavigationCoordinator &coordinator,
        const Runtime::RuntimeSceneView scene, const Runtime::EntityRef entity, const CrowdAgentHandle agent,
        const NavigationPathCaller caller, const std::uint64_t characterRevision) {
        using Return = Result<std::unique_ptr<NavigationSceneAgentLifecycle>>;
        const auto record = registry.Find(agent);
        if (!LiveEntity(scene, entity) || record.HasError() || record.Value().owner.scene.Value() != entity.runtime.value ||
            record.Value().owner.entityIndex != entity.entity.index || record.Value().owner.entityGeneration != entity.entity.generation)
            return Return::Failure(MakeError(NavigationErrors::AgentRegistryStale));
        auto movement = NavigationMovementController::Create(registry, world, coordinator, agent, caller);
        if (movement.HasError())
            return Return::Failure(movement.ErrorValue());
        auto velocity = NavigationVelocityPipeline::Create(*movement.Value(), movement.Value()->Binding(), agent, characterRevision);
        if (velocity.HasError())
            return Return::Failure(velocity.ErrorValue());
        try {
            return Return::Success(std::unique_ptr<NavigationSceneAgentLifecycle>(
                new NavigationSceneAgentLifecycle{registry, entity, std::move(movement).Value(), std::move(velocity).Value(),
                                                  characterRevision}));
        } catch (const std::bad_alloc &) {
            return Return::Failure(MakeError(NavigationErrors::CapacityExceeded));
        }
    }

    /** @copydoc NavigationSceneAgentLifecycle::NavigationSceneAgentLifecycle */
    NavigationSceneAgentLifecycle::NavigationSceneAgentLifecycle(NavigationAgentRegistry &registry, const Runtime::EntityRef entity,
                                                                 std::unique_ptr<NavigationMovementController> movement,
                                                                 std::unique_ptr<NavigationVelocityPipeline> velocity,
                                                                 const std::uint64_t revision) noexcept
        : registry_(registry), entity_(entity), movement_(std::move(movement)), velocity_(std::move(velocity)),
          characterRevision_(revision) {}

    /** @copydoc NavigationSceneAgentLifecycle::~NavigationSceneAgentLifecycle */
    NavigationSceneAgentLifecycle::~NavigationSceneAgentLifecycle() {
        DisableAtSafePoint();
    }

    /** @copydoc NavigationSceneAgentLifecycle::ReconcileAtSafePoint */
    Result<void> NavigationSceneAgentLifecycle::ReconcileAtSafePoint(const std::optional<Runtime::RuntimeSceneView> scene) {
        if (movement_)
            movement_->CollectRetiredAtSafePoint();
        return ValidateSceneAuthority(scene);
    }

    /** @copydoc NavigationSceneAgentLifecycle::ValidateSceneAuthority */
    Result<void> NavigationSceneAgentLifecycle::ValidateSceneAuthority(const std::optional<Runtime::RuntimeSceneView> scene) {
        if (!movement_)
            return Result<void>::Failure(MakeError(NavigationErrors::AdmissionRejected));
        if (scene.has_value() && !scene->IsCurrent())
            return Result<void>::Failure(MakeError(NavigationErrors::StaleSnapshot));
        const auto record = registry_.Find(movement_->Agent());
        const auto binding = registry_.ActiveBinding();
        if (!scene.has_value() || !LiveEntity(*scene, entity_) || record.HasError() || !record.Value().enabled || binding.HasError() ||
            binding.Value() != movement_->Binding()) {
            DisableAtSafePoint();
            return Result<void>::Failure(MakeError(NavigationErrors::AgentRegistryStale));
        }
        return Result<void>::Success();
    }

    /** @copydoc NavigationSceneAgentLifecycle::Start */
    Result<NavigationMovementAcceptance> NavigationSceneAgentLifecycle::Start(const Runtime::RuntimeSceneView scene,
                                                                              NavigationMovementRequest request,
                                                                              const NavigationMovementObservation &current) {
        if (const auto live = ValidateSceneAuthority(scene); live.HasError())
            return Result<NavigationMovementAcceptance>::Failure(live.ErrorValue());
        if (request.entity.has_value()) {
            const auto owner = *request.entity;
            const Runtime::EntityRef target{{owner.scene.Value()}, {owner.entityIndex, owner.entityGeneration}};
            if (scene.Get(target).HasError())
                return Result<NavigationMovementAcceptance>::Failure(MakeError(NavigationErrors::InvalidHandle));
        }
        const auto target = request.entity;
        auto accepted = movement_->Start(std::move(request), current);
        if (accepted.HasValue())
            target_ = target;
        return accepted;
    }

    /** @copydoc NavigationSceneAgentLifecycle::LiveTarget */
    bool NavigationSceneAgentLifecycle::LiveTarget(const Runtime::RuntimeSceneView scene) const {
        if (!target_.has_value())
            return true;
        const Runtime::EntityRef target{{target_->scene.Value()}, {target_->entityIndex, target_->entityGeneration}};
        return scene.Get(target).HasValue();
    }

    /** @copydoc NavigationSceneAgentLifecycle::QualifyTarget */
    NavigationMovementObservation NavigationSceneAgentLifecycle::QualifyTarget(const Runtime::RuntimeSceneView scene,
                                                                               const NavigationMovementObservation &current) const {
        auto qualified = current;
        if (!LiveTarget(scene))
            qualified.target.reset();
        return qualified;
    }

    /** @copydoc NavigationSceneAgentLifecycle::Update */
    Result<NavigationMovementIntent> NavigationSceneAgentLifecycle::Update(const Runtime::RuntimeSceneView scene,
                                                                           const NavigationMovementObservation &current) {
        if (const auto live = ValidateSceneAuthority(scene); live.HasError())
            return Result<NavigationMovementIntent>::Failure(live.ErrorValue());
        return movement_->Update(QualifyTarget(scene, current));
    }

    /** @copydoc NavigationSceneAgentLifecycle::CaptureMotion */
    Result<NavigationCrowdMotionSample> NavigationSceneAgentLifecycle::CaptureMotion(const Runtime::RuntimeSceneView scene,
                                                                                     const NavigationMovementObservation &current,
                                                                                     const Math::Vec3 committedVelocity,
                                                                                     const std::uint64_t characterRevision) {
        using Return = Result<NavigationCrowdMotionSample>;
        if (const auto live = ValidateSceneAuthority(scene); live.HasError())
            return Return::Failure(live.ErrorValue());
        if (current.binding != movement_->Binding() || characterRevision != characterRevision_ || !Math::IsFinite(current.position) ||
            !Math::IsFinite(committedVelocity) || (motionTick_.has_value() && current.path.tick < *motionTick_) ||
            (resetTick_ == current.path.tick && committedVelocity != Math::Vec3{}))
            return Return::Failure(MakeError(NavigationErrors::StaleSnapshot));
        motion_ = {.handle = movement_->Agent(), .position = current.position, .velocity = committedVelocity};
        motionTick_ = current.path.tick;
        return Return::Success(motion_);
    }

    /** @copydoc NavigationSceneAgentLifecycle::ResetMotionSample */
    void NavigationSceneAgentLifecycle::ResetMotionSample(const NavigationMovementObservation &current,
                                                          const std::uint64_t revision) noexcept {
        characterRevision_ = revision;
        motion_ = {.handle = movement_->Agent(), .position = current.position, .velocity = {}};
        motionTick_ = current.path.tick;
        resetTick_ = current.path.tick;
    }

    /** @copydoc NavigationSceneAgentLifecycle::TeleportAtSafePoint */
    Result<void> NavigationSceneAgentLifecycle::TeleportAtSafePoint(const Runtime::RuntimeSceneView scene,
                                                                    const NavigationMovementObservation &current,
                                                                    const std::uint64_t characterRevision) {
        if (const auto live = ValidateSceneAuthority(scene); live.HasError())
            return live;
        if (const auto reset = velocity_->Reset(current, characterRevision); reset.HasError())
            return reset;
        ResetMotionSample(current, characterRevision);
        return Result<void>::Success();
    }

    /** @copydoc NavigationSceneAgentLifecycle::RebindAtSafePoint */
    Result<void> NavigationSceneAgentLifecycle::RebindAtSafePoint(const Runtime::RuntimeSceneView scene, const CrowdAgentHandle agent,
                                                                  const NavigationPathCaller caller,
                                                                  const NavigationMovementObservation &current,
                                                                  const std::uint64_t characterRevision) {
        // The old registration may already be retired by the published replacement transaction. Validate the entity,
        // then let the movement owner admit the new registration; ordinary reconciliation cannot adopt it implicitly.
        if (!movement_ || !LiveEntity(scene, entity_) || !LiveTarget(scene))
            return Result<void>::Failure(MakeError(NavigationErrors::AgentRegistryStale));
        if (const auto rebound = velocity_->Rebind(agent, caller, current, characterRevision); rebound.HasError())
            return rebound;
        ResetMotionSample(current, characterRevision);
        return Result<void>::Success();
    }

    /** @copydoc NavigationSceneAgentLifecycle::MatchesMotion */
    bool NavigationSceneAgentLifecycle::MatchesMotion(const NavigationCrowdSnapshot &snapshot, const std::uint32_t index,
                                                      const std::uint64_t tick) const noexcept {
        if (!motionTick_.has_value() || *motionTick_ != tick || index >= snapshot.Agents().size())
            return false;
        const auto &fact = snapshot.Agents()[index];
        return fact.handle == motion_.handle && fact.position == motion_.position && fact.velocity == motion_.velocity;
    }

    /** @copydoc NavigationSceneAgentLifecycle::PrepareVelocity */
    Result<NavigationVelocityWork> NavigationSceneAgentLifecycle::PrepareVelocity(const Runtime::RuntimeSceneView scene,
                                                                                  const NavigationMovementObservation &current,
                                                                                  const NavigationCrowdSnapshot &snapshot,
                                                                                  const NavigationAvoidanceRequest request) {
        if (const auto live = ValidateSceneAuthority(scene); live.HasError())
            return Result<NavigationVelocityWork>::Failure(live.ErrorValue());
        if (!MatchesMotion(snapshot, request.agentIndex, current.path.tick))
            return Result<NavigationVelocityWork>::Failure(MakeError(NavigationErrors::StaleSnapshot));
        return velocity_->Prepare(QualifyTarget(scene, current), snapshot, request);
    }

    /** @copydoc NavigationSceneAgentLifecycle::CommitVelocity */
    Result<NavigationVelocityPublication> NavigationSceneAgentLifecycle::CommitVelocity(const Runtime::RuntimeSceneView scene,
                                                                                        const NavigationVelocityWork &work,
                                                                                        const NavigationAvoidanceOutcome &outcome,
                                                                                        const NavigationCrowdSnapshot &snapshot,
                                                                                        const NavigationMovementObservation &current) {
        if (const auto live = ValidateSceneAuthority(scene); live.HasError())
            return Result<NavigationVelocityPublication>::Failure(live.ErrorValue());
        if (!LiveTarget(scene) || !MatchesMotion(snapshot, work.request.agentIndex, current.path.tick))
            return Result<NavigationVelocityPublication>::Failure(MakeError(NavigationErrors::StaleSnapshot));
        return velocity_->Commit(work, outcome, snapshot, current);
    }

    /** @copydoc NavigationSceneAgentLifecycle::ObserveFeedback */
    Result<void> NavigationSceneAgentLifecycle::ObserveFeedback(const Runtime::RuntimeSceneView scene,
                                                                const NavigationVelocityFeedback &feedback) {
        if (const auto live = ValidateSceneAuthority(scene); live.HasError())
            return live;
        return velocity_->ObserveFeedback(feedback);
    }

    /** @copydoc NavigationSceneAgentLifecycle::CancelVelocity */
    bool NavigationSceneAgentLifecycle::CancelVelocity(const NavigationVelocityWork &work) noexcept {
        return velocity_ && velocity_->CancelPending(work);
    }

    /** @copydoc NavigationSceneAgentLifecycle::DisableAtSafePoint */
    void NavigationSceneAgentLifecycle::DisableAtSafePoint() noexcept {
        if (!movement_)
            return;
        velocity_->Retire();
        static_cast<void>(registry_.UnregisterAtSafePoint(movement_->Agent()));
        velocity_.reset();
        movement_.reset();  // Destructor hands pending result reservations to coordinator-owned fenced retirement.
        motion_ = {};
        motionTick_.reset();
        resetTick_.reset();
        target_.reset();
    }
}  // namespace Horo::Navigation
