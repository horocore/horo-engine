#include "SceneAnimationState.h"

#include <algorithm>
#include <exception>
#include <limits>

namespace Horo::Runtime {
    namespace A = Animation;
    using namespace AnimationSceneInternal;

    SceneAnimationRuntime::State::State(SceneAnimationRuntimeConfig value) : config(std::move(value)), owner(std::this_thread::get_id()) {}

    SceneAnimationRuntime::State::Actor::Actor(PreparedAssets prepared, A::AnimationGraphInstance instanceOwner, EntityRef owner,
                                               A::AnimationAuthoringComponent component, A::AnimationInstanceHandle identity)
        : assets(std::move(prepared)), graph(std::move(instanceOwner)), entity(owner), authoring(component), instance(identity),
          playing(component.startsPlaying) {}

    bool SceneAnimationRuntime::State::OnOwner() const noexcept {
        return owner == std::this_thread::get_id();
    }

    Result<void> SceneAnimationRuntime::State::Admit(bool controls) const {
        if (!OnOwner())
            return Result<void>::Failure(MakeError(A::AnimationErrors::HandleOwnerMismatch));
        if (closed)
            return Result<void>::Failure(MakeError(A::AnimationAssetErrors::Unavailable));
        if (config.ownerCancellation.IsCancellationRequested())
            return Result<void>::Failure(MakeError(A::AnimationAssetErrors::Cancelled));
        if (controls && attempting)
            return Result<void>::Failure(MakeError(A::AnimationErrors::GraphAdmissionRejected));
        return Result<void>::Success();
    }

    std::shared_ptr<SceneAnimationRuntime::State::Actor> SceneAnimationRuntime::State::Find(EntityRef entity) const noexcept {
        if (!active || active->closed || !active->current)
            return {};
        const auto found = std::ranges::find_if(active->current->actors, [&](const auto &actor) {
            return actor && actor->entity == entity;
        });
        return found == active->current->actors.end() ? nullptr : *found;
    }

    bool SceneAnimationRuntime::State::CurrentScene(const RuntimeSceneView &scene) const noexcept {
        return scene.IsCurrent() && active && !active->closed && active->scene == scene.RuntimeId();
    }

    void SceneAnimationRuntime::State::Scope::Close() noexcept {
        if (closed)
            return;
        closed = true;
        if (!current)
            return;
        for (const auto &actor : current->actors)
            if (actor && actor->graph.Shutdown().HasError())
                std::terminate();
    }

    Result<std::shared_ptr<SceneAnimationRuntime::State::Actor>> SceneAnimationRuntime::State::PrepareActor(const RuntimeSceneView &scene,
                                                                                                            const RuntimeEntityView &entity,
                                                                                                            std::uint32_t slot) {
        using Return = Result<std::shared_ptr<Actor>>;
        if (!entity.components || !entity.components->animation)
            return Return::Failure(MakeError(A::AnimationErrors::ComponentInvalid));
        if (nextGeneration == 0)
            return Return::Failure(MakeError(A::AnimationErrors::GenerationExhausted));
        const auto generation = nextGeneration++;
        const auto &component = *entity.components->animation;
        A::AnimationCookedContext codec;
        codec.cancellation = config.ownerCancellation;
        auto assets = PrepareAssets(scene, entity.groupAssets, component, {config.target, config.limits, codec, assetGenerations});
        if (assets.HasError())
            return Return::Failure(assets.ErrorValue());
        auto prepared = std::move(assets).Value();
        const A::AnimationInstanceHandle identity{config.runtimeOwner, component.component, {slot, generation}};
        A::GraphInstanceCreateContext context;
        context.instance = identity;
        context.maximumStorageBytes = config.limits.maximumInstanceStorageBytes;
        context.maximumInstructions = config.limits.maximumInstructions;
        context.maximumJointOperations = config.limits.maximumJointOperations;
        context.clipLimits = config.limits.clip;
        context.actions = config.limits.actions;
        context.cancellation = config.ownerCancellation;
        context.evaluationDomain = A::AnimationEvaluationDomain::AuthoritativeSimulation;
        auto graph = A::AnimationGraphInstance::Create(prepared.Borrow(), context);
        if (graph.HasError())
            return Return::Failure(graph.ErrorValue());
        return Return::Success(std::make_shared<Actor>(std::move(prepared), std::move(graph).Value(), entity.entity, component, identity));
    }

    Result<void> SceneAnimationRuntime::State::AddActor(Set &set, const RuntimeSceneView &scene, const RuntimeEntityView &entity) {
        if (!entity.components || !entity.components->animation)
            return Result<void>::Success();
        if (std::ranges::any_of(set.actors, [&](const auto &actor) {
            return actor && actor->entity == entity.entity;
        }))
            return Result<void>::Failure(MakeError(A::AnimationErrors::ComponentBindingMismatch));
        const auto free = std::ranges::find(set.actors, nullptr);
        if (free == set.actors.end())
            return Result<void>::Failure(MakeError(A::AnimationAssetErrors::Capacity));
        auto actor = PrepareActor(scene, entity, static_cast<std::uint32_t>(free - set.actors.begin()));
        if (actor.HasError())
            return Result<void>::Failure(actor.ErrorValue());
        *free = std::move(actor).Value();
        return Result<void>::Success();
    }

    Result<std::shared_ptr<SceneAnimationRuntime::State::Scope>> SceneAnimationRuntime::State::PrepareScope(const RuntimeSceneView &scene) {
        using Return = Result<std::shared_ptr<Scope>>;
        if (const auto admission = Admit(true); admission.HasError())
            return Return::Failure(admission.ErrorValue());
        if (!scene.IsCurrent())
            return Return::Failure(MakeError(A::AnimationAssetErrors::Stale));
        auto scope = std::make_shared<Scope>();
        scope->scene = scene.RuntimeId();
        scope->current = std::make_unique<Set>();
        scope->current->actors.resize(config.limits.maximumInstances);
        for (std::size_t slot = 0; slot < scene.SlotCount(); ++slot) {
            const auto entity = scene.EntityAt(slot);
            if (!entity)
                continue;
            const auto added = AddActor(*scope->current, scene, *entity);
            if (added.HasError())
                return Return::Failure(added.ErrorValue());
        }
        return Return::Success(std::move(scope));
    }

    Result<std::unique_ptr<SceneAnimationRuntime::State::Set>> SceneAnimationRuntime::State::PrepareChange(
        const RuntimeSceneView &scene, std::span<const RuntimeEntityView> created, std::span<const EntityRef> destroyed) {
        using Return = Result<std::unique_ptr<Set>>;
        if (const auto admission = Admit(true); admission.HasError())
            return Return::Failure(admission.ErrorValue());
        if (!CurrentScene(scene))
            return Return::Failure(MakeError(A::AnimationAssetErrors::Stale));
        if (active->revision == std::numeric_limits<std::uint64_t>::max())
            return Return::Failure(MakeError(A::AnimationErrors::GenerationExhausted));
        auto next = std::make_unique<Set>(*active->current);
        for (auto &actor : next->actors)
            if (actor && std::ranges::find(destroyed, actor->entity) != destroyed.end())
                actor.reset();
        for (const auto &entity : created) {
            const auto added = AddActor(*next, scene, entity);
            if (added.HasError())
                return Return::Failure(added.ErrorValue());
        }
        return Return::Success(std::move(next));
    }

    void SceneAnimationRuntime::State::Close() noexcept {
        if (!OnOwner())
            std::terminate();
        closed = true;
        // A pending attempt belongs to the scheduler until its exact publish/abort callback.
        if (!attempting && active)
            active->Close();
    }
}  // namespace Horo::Runtime
