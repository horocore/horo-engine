#include "HeadlessNavigationPathScope.h"

#include "HeadlessNetworkServices.h"
#include "Horo/Navigation/NavigationDefinition.h"
#include "ProjectSceneNavigationComposition.h"
#include "SceneAiNavigationMotion.h"

#include <algorithm>
#include <limits>
#include <new>

namespace Horo::Application::Internal {
    namespace Nav = Navigation;

    /** @copydoc HeadlessNavigationPathScope::Create */
    Result<std::unique_ptr<HeadlessNavigationPathScope>> HeadlessNavigationPathScope::Create(
        ProjectSceneNavigationWorld &owner, const ProjectSceneNavigationSelection &selection, const HeadlessNavigationPathPolicy &policy,
        const Runtime::SceneRuntimeId scene, const std::string_view module) {
        using Scope = Result<std::unique_ptr<HeadlessNavigationPathScope>>;
        if (!policy.IsValid() || !selection.catalog || selection.policy.MaximumQuery().query != Nav::NavigationQueryKind::Path)
            return Scope::Failure(MakeError(Nav::NavigationErrors::CapabilityDescriptorInvalid));
        const auto filter = selection.catalog->Registry().ResolveFilter(policy.filter);
        if (filter.HasError())
            return Scope::Failure(filter.ErrorValue());
        auto world = owner.assets ? owner.assets->ActiveWorld() : nullptr;
        if (!world)
            return Scope::Failure(MakeError(Nav::NavigationErrors::NoNavigationData));
        const auto active = world->ActiveDescriptor();
        if (active.HasError())
            return Scope::Failure(active.ErrorValue());
        if (active.Value().scene.Value() != scene.value)
            return Scope::Failure(MakeError(Nav::NavigationErrors::StaleSnapshot));
        const auto lease = world->Acquire(active.Value().world);
        if (lease.HasError())
            return Scope::Failure(lease.ErrorValue());
        const auto requirement = selection.policy.MaximumQuery();
        const auto profileRevision = selection.policy.Revision().Value();
        if (auto admitted = Nav::AdmitNavigationQuery(lease.Value().Backend().Capabilities(), profileRevision, requirement);
            admitted.HasError())
            return Scope::Failure(admitted.ErrorValue());

        // The supported provider seals its filter table, static obstacle-free overlay, and baked coordinate frame in
        // the immutable topology. Any change to these requires a different world. The project query profile has its own revision.
        const auto epoch = active.Value().topology.Value();
        const Nav::NavigationOutcomeProvenance source{.snapshot = Nav::NavigationSnapshotToken::Create(epoch).Value(),
                                                      .world = active.Value().world,
                                                      .topology = active.Value().topology,
                                                      .obstacleRevision = epoch,
                                                      .filterRevision = epoch,
                                                      .profileRevision = profileRevision,
                                                      .originRevision = epoch};
        // There is exactly one native gameplay principal in this standalone world, with an independent caller epoch per world.
        const Nav::NavigationPathCaller caller{Nav::NavigationDynamicOwnerId::Create(active.Value().world.Value()).Value(),
                                               Nav::NavigationDynamicOwnerGeneration::Create(active.Value().sceneGeneration.Value())
                                                   .Value()};
        const Nav::NavigationAgentSceneBinding binding{active.Value().world, active.Value().scene, active.Value().sceneGeneration};
        try {
            auto context = Gameplay::GameplayNavigationContext::CreateWithBehaviorAgentPaths({std::string{module}, binding, true, false},
                                                                                             *world, owner.coordinator, owner.agents,
                                                                                             {.caller = caller,
                                                                                              .request = {.filter = policy.filter,
                                                                                                          .requirement = requirement},
                                                                                              .capabilityRevision = profileRevision,
                                                                                              .deadlineTick = policy.deadlineTicks},
                                                                                             owner.maximumPendingPathRequests);
            if (context.HasError())
                return Scope::Failure(context.ErrorValue());
            if (auto refreshed = context.Value()->Refresh({.binding = binding, .path = {.provenance = source}}); refreshed.HasError())
                return Scope::Failure(refreshed.ErrorValue());
            return Scope::Success(
                std::unique_ptr<HeadlessNavigationPathScope>{new HeadlessNavigationPathScope{std::move(world),
                                                                                             owner.coordinator,
                                                                                             active.Value(),
                                                                                             source,
                                                                                             owner.agents,
                                                                                             {caller},
                                                                                             std::move(context).Value()}});
        } catch (const std::bad_alloc &) {
            return Scope::Failure(MakeError(Nav::NavigationErrors::CapacityExceeded));
        }
    }

    HeadlessNavigationPathScope::HeadlessNavigationPathScope(std::shared_ptr<Nav::NavigationWorldLifecycle> lifecycle,
                                                             Nav::NavigationCoordinator &coordinator,
                                                             const Nav::NavigationWorldActivationDescriptor activation,
                                                             const Nav::NavigationOutcomeProvenance source,
                                                             Nav::NavigationAgentRegistry &agents,
                                                             std::vector<Nav::NavigationPathCaller> callers,
                                                             std::shared_ptr<Gameplay::GameplayNavigationContext> context) noexcept
        : lifecycle_(std::move(lifecycle)), coordinator_(coordinator), activation_(activation), source_(source), agents_(agents),
          callers_(std::move(callers)), context_(std::move(context)) {}

    HeadlessNavigationPathScope::~HeadlessNavigationPathScope() = default;

    /** @copydoc HeadlessNavigationPathScope::PrepareActors */
    Result<void> HeadlessNavigationPathScope::PrepareActors(ScenePhysicsHost &physics, const Runtime::RuntimeSceneView &scene,
                                                            std::span<const HeadlessNavigationActorPolicy> selected) {
        if (selected.empty())
            return Result<void>::Success();
        auto population = agents_.Snapshot();
        if (population.HasError())
            return Result<void>::Failure(std::move(population).ErrorValue());
        if (!actors_.empty() || selected.size() > population.Value().Agents().size())
            return Result<void>::Failure(MakeError(Nav::NavigationErrors::AdmissionRejected));
        try {
            auto callers = callers_;
            std::vector<Actor> actors;
            callers.reserve(1 + selected.size());
            actors.reserve(selected.size());
            for (const auto &policy : selected) {
                const auto entity = scene.Find(policy.object);
                if (!entity || policy.principal == 0)
                    return Result<void>::Failure(MakeError(Nav::NavigationErrors::AgentDescriptorInvalid));
                const auto previous = callers.back().owner.Value();
                if (previous == std::numeric_limits<std::uint64_t>::max())
                    return Result<void>::Failure(MakeError(Nav::NavigationErrors::GenerationExhausted));
                const Nav::NavigationPathCaller caller{Nav::NavigationDynamicOwnerId::Create(previous + 1).Value(),
                                                       callers.front().generation};
                auto actor = SceneAiNavigationActor::CreateForScene(agents_, *lifecycle_, coordinator_, caller, physics, scene, *entity,
                                                                    {.identity = policy.principal,
                                                                     .source = CharacterInput::DesiredMotionSource::ExternalIntent,
                                                                     .permissionGranted = true});
                if (actor.HasError())
                    return Result<void>::Failure(std::move(actor).ErrorValue());
                if (std::ranges::any_of(actors, [&](const auto &existing) {
                    return existing.owner->Agent() == actor.Value()->Agent();
                }))
                    return Result<void>::Failure(MakeError(Nav::NavigationErrors::AgentDescriptorInvalid));
                actors.push_back({std::move(actor).Value(), {}});
                callers.push_back(caller);
            }
            actors_ = std::move(actors);
            callers_ = std::move(callers);
            return Result<void>::Success();
        } catch (const std::bad_alloc &) {
            return Result<void>::Failure(MakeError(Nav::NavigationErrors::CapacityExceeded));
        }
    }

    /** @copydoc HeadlessNavigationPathScope::BeginMotionTick */
    void HeadlessNavigationPathScope::BeginMotionTick(const std::uint64_t tick, const float stepSeconds) noexcept {
        motionTick_ = tick;
        motionStep_ = stepSeconds;
    }

    Result<Nav::NavigationMovementObservation> HeadlessNavigationPathScope::ObserveActor(
        const Runtime::RuntimeSceneView &scene, const HeadlessNavigationMotionPolicy &policy, const SceneAiNavigationActor &actor,
        const std::optional<Nav::NavigationAgentOwner> target) const {
        using Observation = Result<Nav::NavigationMovementObservation>;
        Nav::NavigationMovementObservation current{.binding = context_->Binding().scene,
                                                   .stepSeconds = motionStep_,
                                                   .path = {.provenance = source_,
                                                            .linkRevision = activation_.topology.Value(),
                                                            .goalRevision = 1,
                                                            .tick = motionTick_}};
        if (target) {
            if (!policy.follow)
                return Observation::Failure(MakeError(Nav::NavigationErrors::CapabilityUnavailable));
            auto followed = policy.follow(scene, *target, motionTick_);
            if (followed.HasError())
                return Observation::Failure(std::move(followed).ErrorValue());
            current.target = std::move(followed).Value();
        }
        return actor.Capture(current);
    }

    /** @copydoc HeadlessNavigationPathScope::AdmitMotion */
    Result<void> HeadlessNavigationPathScope::AdmitMotion(const Runtime::RuntimeSceneView &scene,
                                                          const HeadlessNavigationMotionPolicy &policy, AI::AiRuntimeCapability ai,
                                                          AI::AiTaskContinuation task, Nav::NavigationMovementRequest request,
                                                          const std::uint64_t tick) {
        if (tick == 0 || tick != motionTick_ || scene.RuntimeId().value != activation_.scene.Value())
            return Result<void>::Failure(MakeError(Nav::NavigationErrors::StaleSnapshot));
        const auto entity = ai.ResolveAgentOwner(task.Operation().agent);
        if (entity.HasError())
            return Result<void>::Failure(entity.ErrorValue());
        const auto actor = agents_.FindOwner({activation_.scene, entity.Value().entity.index, entity.Value().entity.generation});
        if (!actor)
            return Result<void>::Failure(MakeError(Nav::NavigationErrors::AgentRegistryStale));
        const auto selected = std::ranges::find_if(actors_, [&](const Actor &entry) {
            return entry.owner->Agent() == *actor;
        });
        if (selected == actors_.end())
            return Result<void>::Failure(MakeError(Nav::NavigationErrors::AdmissionRejected));
        const auto target = request.entity;
        auto current = ObserveActor(scene, policy, *selected->owner, target);
        if (current.HasError())
            return Result<void>::Failure(std::move(current).ErrorValue());
        auto admitted = selected->owner->Admit(std::move(ai), std::move(task), std::move(request), current.Value());
        if (admitted.HasValue())
            selected->target = target;
        return admitted;
    }

    /** @copydoc HeadlessNavigationPathScope::SubmitMotion */
    Result<void> HeadlessNavigationPathScope::SubmitMotion(const Runtime::RuntimeSceneView &scene,
                                                           const HeadlessNavigationMotionPolicy &policy) {
        if (std::ranges::none_of(actors_, [](const Actor &entry) {
            return entry.owner->Motion() != nullptr;
        }))
            return Result<void>::Success();
        auto population = agents_.Snapshot();
        if (population.HasError())
            return Result<void>::Failure(std::move(population).ErrorValue());
        auto capture = policy.capture(scene, population.Value(), motionTick_);
        if (capture.HasError())
            return Result<void>::Failure(std::move(capture).ErrorValue());
        const auto sampled = capture.Value().Agents();
        for (auto &actor : actors_) {
            auto *motion = actor.owner->Motion();
            if (!motion)
                continue;
            auto current = ObserveActor(scene, policy, *actor.owner, actor.target);
            if (current.HasError()) {
                motion->Reject(std::move(current).ErrorValue());
                continue;
            }
            const auto sample = std::ranges::find(sampled, actor.owner->Agent(), &Nav::NavigationCrowdAgentFact::handle);
            if (sample == sampled.end()) {
                motion->Reject(MakeError(Nav::NavigationErrors::StaleSnapshot));
                continue;
            }
            auto limits = policy.limits;
            limits.agentIndex = static_cast<std::size_t>(sample - sampled.begin());
            limits.stepSeconds = motionStep_;
            // Motion-stage failures are delivered by the binding to this actor's task.
            (void)motion->Submit(current.Value(), capture.Value(), limits, *policy.solver);
        }
        return Result<void>::Success();
    }

    /** @copydoc HeadlessNavigationPathScope::ObserveMotion */
    void HeadlessNavigationPathScope::ObserveMotion(const std::uint64_t tick) {
        for (auto &actor : actors_) {
            if (auto *motion = actor.owner->Motion()) {
                if (auto result = motion->Observe(tick); result.HasError())
                    motion->Reject(std::move(result).ErrorValue());
            }
        }
    }

    /** @copydoc HeadlessNavigationPathScope::DiscardMotionFeedback */
    void HeadlessNavigationPathScope::DiscardMotionFeedback() noexcept {
        for (auto &actor : actors_)
            if (auto *motion = actor.owner->Motion())
                motion->DiscardFeedback();
    }

    /** @copydoc HeadlessNavigationPathScope::Context */
    const std::shared_ptr<Gameplay::GameplayNavigationContext> &HeadlessNavigationPathScope::Context() const noexcept {
        return context_;
    }

    /** @copydoc HeadlessNavigationPathScope::CommitAtFixedTick */
    Result<void> HeadlessNavigationPathScope::CommitAtFixedTick(const std::uint64_t tick) const {
        const auto current = lifecycle_->ActiveDescriptor();
        if (current.HasError())
            return Result<void>::Failure(current.ErrorValue());
        if (current.Value() != activation_)
            return Result<void>::Failure(MakeError(Nav::NavigationErrors::StaleSnapshot));
        static_cast<void>(coordinator_.Commit({activation_, source_, callers_, tick}));
        return context_->Refresh({.binding = context_->Binding().scene, .path = {.provenance = source_, .tick = tick}});
    }

    /** @copydoc HeadlessNavigationPathScope::DispatchAtFixedTick */
    void HeadlessNavigationPathScope::DispatchAtFixedTick(const std::uint64_t tick) const {
        static_cast<void>(coordinator_.Dispatch(tick));
    }

    /** @copydoc HeadlessNavigationPathScope::Revoke */
    void HeadlessNavigationPathScope::Revoke() noexcept {
        actors_.clear();
        context_->Revoke();
    }

    /** @copydoc HeadlessNavigationPathScope::PollRetirement */
    Result<bool> HeadlessNavigationPathScope::PollRetirement() const {
        return context_->DrainRetired();
    }
}  // namespace Horo::Application::Internal
