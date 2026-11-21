#include "SceneAiNavigationMotion.h"

#include "ScenePhysicsHost.h"

#include <new>
#include <utility>

namespace Horo::Application::Internal {
    namespace Nav = Navigation;

    /** @copydoc SceneAiNavigationMotion::SceneAiNavigationMotion */
    SceneAiNavigationMotion::SceneAiNavigationMotion(AI::AiNavigationMovementTask task, Nav::NavigationVelocityPipeline &velocity,
                                                     Nav::NavigationCharacterAdapter &character) noexcept
        : task_(std::move(task)), velocity_(velocity), character_(character) {}

    SceneAiNavigationMotion::~SceneAiNavigationMotion() {
        DiscardFeedback();
    }

    /** @copydoc SceneAiNavigationMotion::Submit */
    Result<std::optional<Nav::NavigationVelocityPublication>> SceneAiNavigationMotion::Submit(
        const Nav::NavigationMovementObservation &current, const Nav::NavigationCrowdSnapshot &capture,
        Nav::NavigationAvoidanceRequest request, Nav::INavigationCrowdBackend &solver) {
        using Publication = Result<std::optional<Nav::NavigationVelocityPublication>>;
        if (pendingTick_ != 0 || !character_.UsesPipeline(velocity_))
            return Publication::Failure(MakeError(Nav::NavigationErrors::StaleSnapshot));
        auto work = task_.PrepareSafeMotion(velocity_, current, capture, request);
        if (work.HasError())
            return Publication::Failure(std::move(work).ErrorValue());
        if (!work.Value())
            return Publication::Success(std::nullopt);
        auto submitted = SubmitWork(*work.Value(), current, capture, solver);
        if (submitted.HasError()) {
            (void)velocity_.CancelPending(*work.Value());
            return Publication::Failure(task_.FailMotion(std::move(submitted).ErrorValue()));
        }
        pendingTick_ = current.path.tick;
        return Publication::Success(std::move(submitted).Value());
    }

    /** @copydoc SceneAiNavigationMotion::SubmitWork */
    Result<Nav::NavigationVelocityPublication> SceneAiNavigationMotion::SubmitWork(const Nav::NavigationVelocityWork &work,
                                                                                   const Nav::NavigationMovementObservation &current,
                                                                                   const Nav::NavigationCrowdSnapshot &capture,
                                                                                   Nav::INavigationCrowdBackend &solver) {
        using Publication = Result<Nav::NavigationVelocityPublication>;
        auto sampled = solver.Solve(capture, work.request);
        if (sampled.HasError())
            return Publication::Failure(std::move(sampled).ErrorValue());
        auto safe = velocity_.Commit(work, sampled.Value(), capture, current);
        if (safe.HasError())
            return safe;
        auto motion = character_.Prepare(safe.Value(), current, current.path.tick);
        if (motion.HasError())
            return Publication::Failure(std::move(motion).ErrorValue());
        auto admitted = character_.Submit(motion.Value(), current);
        if (admitted.HasError())
            return Publication::Failure(std::move(admitted).ErrorValue());
        if (admitted.Value().absent || admitted.Value().admission.status != Character::CharacterCommandAdmissionStatus::Deferred)
            return Publication::Failure(MakeError(Nav::NavigationErrors::AdmissionRejected));
        return safe;
    }

    /** @copydoc SceneAiNavigationMotion::Observe */
    Result<void> SceneAiNavigationMotion::Observe(const std::uint64_t tick) {
        if (pendingTick_ == 0)
            return Result<void>::Success();
        if (pendingTick_ != tick)
            return Result<void>::Failure(MakeError(Nav::NavigationErrors::StaleSnapshot));
        auto observed = character_.ObserveFeedback();
        if (observed.HasError())
            return Result<void>::Failure(std::move(observed).ErrorValue());
        pendingTick_ = 0;
        return Result<void>::Success();
    }

    /** @copydoc SceneAiNavigationMotion::DiscardFeedback */
    void SceneAiNavigationMotion::DiscardFeedback() noexcept {
        if (pendingTick_ != 0) {
            character_.DiscardFeedback();
            pendingTick_ = 0;
        }
    }

    SceneAiNavigationActor::SceneAiNavigationActor(std::unique_ptr<Nav::NavigationMovementController> movement,
                                                   std::unique_ptr<Nav::NavigationVelocityPipeline> velocity,
                                                   Nav::NavigationCharacterAdapter bridge, Character::CharacterCapability character,
                                                   Nav::NavigationCharacterBinding binding, Nav::CrowdAgentHandle agent) noexcept
        : movement_(std::move(movement)), velocity_(std::move(velocity)), bridge_(std::move(bridge)), character_(std::move(character)),
          binding_(binding), agent_(agent) {}

    /** @copydoc SceneAiNavigationActor::CreateForScene */
    Result<std::unique_ptr<SceneAiNavigationActor>> SceneAiNavigationActor::CreateForScene(
        Nav::NavigationAgentRegistry &registry, Nav::NavigationWorldLifecycle &world, Nav::NavigationCoordinator &coordinator,
        const Nav::NavigationPathCaller caller, ScenePhysicsHost &physics, const Runtime::RuntimeSceneView &scene,
        const Runtime::EntityRef entity, const CharacterInput::DesiredMotionPrincipal principal) {
        using Actor = Result<std::unique_ptr<SceneAiNavigationActor>>;
        const auto controller = physics.ResolveCharacterController(scene, entity);
        if (controller.HasError())
            return Actor::Failure(controller.ErrorValue());
        const auto binding = registry.ActiveBinding();
        if (binding.HasError())
            return Actor::Failure(binding.ErrorValue());
        if (binding.Value().scene.Value() != entity.runtime.value)
            return Actor::Failure(MakeError(Nav::NavigationErrors::StaleSnapshot));
        const auto agent = registry.FindOwner({binding.Value().scene, entity.entity.index, entity.entity.generation});
        if (!agent)
            return Actor::Failure(MakeError(Nav::NavigationErrors::AgentRegistryStale));
        const auto native = physics.Borrow();
        if (native.HasError())
            return Actor::Failure(native.ErrorValue());
        const auto grant = native.Value().character.IssueCapability();
        if (grant.HasError())
            return Actor::Failure(grant.ErrorValue());
        try {
            auto actor = Create(registry, world, coordinator, caller, *agent, grant.Value(),
                                {principal, controller.Value(), binding.Value(), controller.Value().slot.generation});
            if (actor.HasError())
                grant.Value().Revoke();
            return actor;
        } catch (const std::bad_alloc &) {
            grant.Value().Revoke();
            return Actor::Failure(MakeError(Nav::NavigationErrors::CapacityExceeded));
        }
    }

    /** @copydoc SceneAiNavigationActor::Create */
    Result<std::unique_ptr<SceneAiNavigationActor>> SceneAiNavigationActor::Create(
        Nav::NavigationAgentRegistry &registry, Nav::NavigationWorldLifecycle &world, Nav::NavigationCoordinator &coordinator,
        const Nav::NavigationPathCaller caller, const Nav::CrowdAgentHandle agent, Character::CharacterCapability character,
        const Nav::NavigationCharacterBinding &binding) {
        using Actor = Result<std::unique_ptr<SceneAiNavigationActor>>;
        auto movement = Nav::NavigationMovementController::Create(registry, world, coordinator, agent, caller);
        if (movement.HasError())
            return Actor::Failure(std::move(movement).ErrorValue());
        auto velocity = Nav::NavigationVelocityPipeline::Create(*movement.Value(), binding.navigation, agent, binding.characterRevision);
        if (velocity.HasError())
            return Actor::Failure(std::move(velocity).ErrorValue());
        auto bridge = Nav::NavigationCharacterAdapter::Create(*velocity.Value(), character, binding);
        if (bridge.HasError())
            return Actor::Failure(std::move(bridge).ErrorValue());
        return Actor::Success(std::unique_ptr<SceneAiNavigationActor>{
            new SceneAiNavigationActor{std::move(movement).Value(), std::move(velocity).Value(), std::move(bridge).Value(),
                                       std::move(character), binding, agent}});
    }

    /** @copydoc SceneAiNavigationActor::Admit */
    Result<void> SceneAiNavigationActor::Admit(AI::AiRuntimeCapability ai, AI::AiTaskContinuation task,
                                               Nav::NavigationMovementRequest request, const Nav::NavigationMovementObservation &current) {
        if (task_.IsCurrent())
            return Result<void>::Failure(MakeError(Nav::NavigationErrors::AdmissionRejected));
        const auto observed = Capture(current);
        if (observed.HasError())
            return Result<void>::Failure(observed.ErrorValue());
        motion_.reset();
        auto selected = AI::AiNavigationMovementTask::Start(std::move(ai), task, *movement_, std::move(request), observed.Value());
        if (selected.HasError())
            return Result<void>::Failure(std::move(selected).ErrorValue());
        motion_.emplace(std::move(selected).Value(), *velocity_, bridge_);
        task_ = std::move(task);
        return Result<void>::Success();
    }

    /** @copydoc SceneAiNavigationActor::Motion */
    SceneAiNavigationMotion *SceneAiNavigationActor::Motion() noexcept {
        return motion_ ? &*motion_ : nullptr;
    }

    /** @copydoc SceneAiNavigationActor::Capture */
    Result<Nav::NavigationMovementObservation> SceneAiNavigationActor::Capture(Nav::NavigationMovementObservation current) const {
        using Observation = Result<Nav::NavigationMovementObservation>;
        if (current.binding != binding_.navigation)
            return Observation::Failure(MakeError(Nav::NavigationErrors::StaleSnapshot));
        const auto root = character_.ControllerTransform(binding_.controller);
        if (root.HasError())
            return Observation::Failure(root.ErrorValue());
        current.position = root.Value().position;
        return Observation::Success(current);
    }

    /** @copydoc SceneAiNavigationActor::Agent */
    Nav::CrowdAgentHandle SceneAiNavigationActor::Agent() const noexcept {
        return agent_;
    }
}  // namespace Horo::Application::Internal
