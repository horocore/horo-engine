#include "BehaviorRuntimeState.h"

namespace Horo::Gameplay {
    BehaviorRuntime::BehaviorRuntime(std::shared_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

    /** @copydoc BehaviorRuntime::Create */
    Result<std::unique_ptr<BehaviorRuntime>> BehaviorRuntime::Create(Runtime::RuntimeScene &scene, const BehaviorRegistry &registry,
                                                                     const BehaviorRuntimeLimits limits) {
        return Create(scene, registry, limits,
                      GameplayPhysicsContext::Withheld({.scene = scene.View().RuntimeId().value}, GameplayPhysicsDenial::Unavailable));
    }

    /** @copydoc BehaviorRuntime::Create */
    Result<std::unique_ptr<BehaviorRuntime>> BehaviorRuntime::Create(Runtime::RuntimeScene &scene, const BehaviorRegistry &registry,
                                                                     const BehaviorRuntimeLimits limits,
                                                                     std::shared_ptr<const GameplayPhysicsContext> physics) {
        if (physics && physics->Binding().scene != scene.View().RuntimeId().value)
            return Result<std::unique_ptr<BehaviorRuntime>>::Failure(MakeError(Physics::PhysicsErrors::HandleWorldMismatch));
        auto generationLease = registry.AcquireGenerationLease();
        if (generationLease.HasError())
            return Result<std::unique_ptr<BehaviorRuntime>>::Failure(generationLease.ErrorValue());
        auto impl = std::make_shared<Impl>(scene, registry, limits, std::move(generationLease).Value());
        impl->physics = std::move(physics);
        return ActivateImpl(std::move(impl));
    }

    /** @copydoc BehaviorRuntime::Create */
    Result<std::unique_ptr<BehaviorRuntime>> BehaviorRuntime::Create(Runtime::RuntimeSceneService &scenes, const BehaviorRegistry &registry,
                                                                     const BehaviorRuntimeLimits limits,
                                                                     std::shared_ptr<const GameplayPhysicsContext> physics,
                                                                     std::shared_ptr<const GameplayPrefabContext> prefabs) {
        if (const auto active = scenes.ActiveScene(); !active || (prefabs && prefabs->Binding().scene != active->RuntimeId()) ||
                                                      (physics && physics->Binding().scene != active->RuntimeId().value))
            return Result<std::unique_ptr<BehaviorRuntime>>::Failure(MakeError(Prefab::PrefabErrors::SceneUnavailable));
        auto generationLease = registry.AcquireGenerationLease();
        if (generationLease.HasError())
            return Result<std::unique_ptr<BehaviorRuntime>>::Failure(generationLease.ErrorValue());
        auto impl = std::make_shared<Impl>(scenes, registry, limits, std::move(generationLease).Value());
        impl->physics = std::move(physics);
        impl->prefabs = std::move(prefabs);
        return ActivateImpl(std::move(impl));
    }

    /** @copydoc BehaviorRuntime::ActivateImpl */
    Result<std::unique_ptr<BehaviorRuntime>> BehaviorRuntime::ActivateImpl(std::shared_ptr<Impl> impl) {
        if (Result<void> built = impl->BuildInstances(); built.HasError()) {
            if (impl->physics)
                impl->physics->Revoke();
            if (impl->prefabs)
                impl->prefabs->Revoke();
            bool rollbackComplete = true;
            for (auto iterator = impl->instances.rbegin(); iterator != impl->instances.rend(); ++iterator) {
                rollbackComplete = impl->RollbackInstance(*iterator) && rollbackComplete;
            }
            if (!rollbackComplete)
                return Result<std::unique_ptr<BehaviorRuntime>>::Failure(
                    MakeError(GameplayErrors::GameplayFactoryFailed, "Behavior activation failed and rollback callbacks threw."));
            return Result<std::unique_ptr<BehaviorRuntime>>::Failure(built.ErrorValue());
        }
        return Result<std::unique_ptr<BehaviorRuntime>>::Success(
            std::unique_ptr<BehaviorRuntime>{new BehaviorRuntime{std::move(impl)}});  // NOSONAR(cpp:S5950) Private constructor
    }

    BehaviorRuntime::~BehaviorRuntime() {
        Shutdown();
    }

    /** @copydoc BehaviorRuntime::FixedUpdate */
    Result<void> BehaviorRuntime::FixedUpdate(const std::span<const GameplayInputAction> input, const FixedDeltaTime delta) const {
        if (impl_->SceneView().RuntimeId() != impl_->sceneIdentity) {
            impl_->ShutdownNow();
            return Result<void>::Failure(MakeError(Prefab::PrefabErrors::SceneUnavailable));
        }
        if (impl_->shutdown || impl_->structuralPending)
            return Result<void>::Failure(MakeError(GameplayErrors::InvalidBehaviorComponent, "Behavior runtime is shut down."));
        impl_->events.BeginTick();
        Runtime::SceneCommandBuffer commands;
        for (Impl::Instance &instance : impl_->instances) {
            if (!instance.component.enabled)
                continue;
            Impl::ContextBackend backend{*impl_, instance, input, commands};
            BehaviorContext context{backend};
            if (!instance.started) {
                instance.implementation->OnStart(context);
                instance.started = true;
            }
            for (const GameplayEvent &event : impl_->events.current) {
                if (!event.target || ToRuntime(*event.target) == instance.entity)
                    instance.implementation->OnEvent(context, event);
            }
            for (const GameplayInputAction &action : input) {
                if (action.pressed || action.released)
                    instance.implementation->OnInputAction(context, action);
            }
            instance.implementation->OnFixedUpdate(context, delta);
        }
        if (!commands.Empty()) {
            Result<void> committed = impl_->CommitCommands(std::move(commands));
            if (committed.HasError())
                return Result<void>::Failure(committed.ErrorValue());
        }
        return Result<void>::Success();
    }

    /** @copydoc BehaviorRuntime::PresentationUpdate */
    void BehaviorRuntime::PresentationUpdate(const FrameDeltaTime delta) const {
        if (impl_->SceneView().RuntimeId() != impl_->sceneIdentity) {
            impl_->ShutdownNow();
            return;
        }
        if (impl_->shutdown || impl_->structuralPending)
            return;
        Runtime::SceneCommandBuffer rejectedCommands;
        for (Impl::Instance &instance : impl_->instances) {
            if (!instance.component.enabled)
                continue;
            Impl::ContextBackend backend{*impl_, instance, {}, rejectedCommands, false};
            BehaviorContext context{backend};
            instance.implementation->OnPresentationUpdate(context, delta);
        }
    }

    /** @copydoc BehaviorRuntime::SetEnabled */
    Result<void> BehaviorRuntime::SetEnabled(const BehaviorInstanceId instanceId, const bool enabled) const {
        if (impl_->shutdown || impl_->structuralPending)
            return Result<void>::Failure(MakeError(GameplayErrors::InvalidBehaviorComponent));
        const auto found = std::ranges::find(impl_->instances, instanceId, [](const Impl::Instance &instance) {
            return instance.component.instanceId;
        });
        if (found == impl_->instances.end())
            return Result<void>::Failure(MakeError(GameplayErrors::InvalidBehaviorInstanceId));
        if (found->component.enabled == enabled)
            return Result<void>::Success();
        Runtime::SceneCommandBuffer commands;
        Impl::ContextBackend backend{*impl_, *found, {}, commands, false};
        BehaviorContext context{backend};
        if (enabled) {
            found->enabledCallbackActive = true;
            try {
                found->implementation->OnEnable(context);
            } catch (...) {  // NOSONAR(cpp:S2738) Native project callbacks may throw arbitrary exception types.
                return Result<void>::Failure(MakeError(GameplayErrors::GameplayFactoryFailed, "Behavior OnEnable threw an exception."));
            }
        } else {
            try {
                found->implementation->OnDisable(context);
            } catch (...) {  // NOSONAR(cpp:S2738) Native project callbacks may throw arbitrary exception types.
                return Result<void>::Failure(MakeError(GameplayErrors::GameplayFactoryFailed, "Behavior OnDisable threw an exception."));
            }
            found->enabledCallbackActive = false;
        }
        found->component.enabled = enabled;
        return Result<void>::Success();
    }

    /** @copydoc BehaviorRuntime::CaptureReloadSnapshot */
    Result<BehaviorRuntimeReloadSnapshot> BehaviorRuntime::CaptureReloadSnapshot() const {
        if (!impl_ || impl_->shutdown || impl_->structuralPending)
            return Result<BehaviorRuntimeReloadSnapshot>::Failure(MakeError(GameplayErrors::GameplayReloadSnapshotInvalid));
        BehaviorRuntimeReloadSnapshot snapshot;
        snapshot.instances.reserve(impl_->instances.size());
        std::size_t totalBytes = 0;
        for (const Impl::Instance &instance : impl_->instances) {
            Result<std::vector<std::byte>> captured = Result<std::vector<std::byte>>::Success({});
            try {
                captured = instance.implementation->CaptureReloadState();
            } catch (...) {  // NOSONAR(cpp:S2738) Native project callbacks may throw arbitrary exception types.
                return Result<BehaviorRuntimeReloadSnapshot>::Failure(
                    MakeError(GameplayErrors::GameplayReloadSnapshotInvalid, "Behavior reload capture threw an exception."));
            }
            if (captured.HasError())
                return Result<BehaviorRuntimeReloadSnapshot>::Failure(captured.ErrorValue());
            std::vector<std::byte> payload = std::move(captured).Value();
            if (payload.size() > MaximumBehaviorReloadStateBytes || totalBytes > MaximumBehaviorReloadSnapshotBytes - payload.size())
                return Result<BehaviorRuntimeReloadSnapshot>::Failure(MakeError(GameplayErrors::GameplayReloadSnapshotInvalid));
            totalBytes += payload.size();
            snapshot.instances.emplace_back(instance.component.instanceId, instance.component.typeId, std::move(payload), instance.started);
        }
        return Result<BehaviorRuntimeReloadSnapshot>::Success(std::move(snapshot));
    }

    /** @copydoc BehaviorRuntime::RestoreReloadSnapshot */
    Result<void> BehaviorRuntime::RestoreReloadSnapshot(const BehaviorRuntimeReloadSnapshot &snapshot) const {
        if (!impl_ || impl_->shutdown || impl_->structuralPending || snapshot.instances.size() != impl_->instances.size())
            return Result<void>::Failure(MakeError(GameplayErrors::GameplayReloadSnapshotInvalid));
        std::unordered_map<std::uint64_t, const BehaviorInstanceReloadState *> states;
        states.reserve(snapshot.instances.size());
        std::size_t totalBytes = 0;
        for (const BehaviorInstanceReloadState &state : snapshot.instances) {
            if (!state.instanceId.IsValid() || !state.typeId.IsValid() || state.payload.size() > MaximumBehaviorReloadStateBytes ||
                totalBytes > MaximumBehaviorReloadSnapshotBytes - state.payload.size() ||
                !states.emplace(state.instanceId.value, &state).second)
                return Result<void>::Failure(MakeError(GameplayErrors::GameplayReloadSnapshotInvalid));
            totalBytes += state.payload.size();
        }
        for (Impl::Instance &instance : impl_->instances) {
            const auto found = states.find(instance.component.instanceId.value);
            if (found == states.end() || found->second->typeId != instance.component.typeId)
                return Result<void>::Failure(MakeError(GameplayErrors::GameplayReloadSnapshotInvalid));
            try {
                if (Result<void> restored = instance.implementation->RestoreReloadState(found->second->payload); restored.HasError())
                    return restored;
            } catch (...) {  // NOSONAR(cpp:S2738) Native project callbacks may throw arbitrary exception types.
                return Result<void>::Failure(
                    MakeError(GameplayErrors::GameplayReloadRestoreFailed, "Behavior reload restore threw an exception."));
            }
            instance.started = found->second->started;
        }
        return Result<void>::Success();
    }

    /** @copydoc BehaviorRuntime::Shutdown */
    void BehaviorRuntime::Shutdown() const noexcept {
        if (!impl_ || impl_->shutdown)
            return;
        impl_->shutdownRequested = true;
        if (!impl_->structuralPending)
            impl_->ShutdownNow();
    }

    /** @copydoc BehaviorRuntime::InstanceCount */
    std::size_t BehaviorRuntime::InstanceCount() const noexcept {
        return impl_->instances.size();
    }

    /** @copydoc BehaviorRuntime::MakeStructuralParticipant */
    std::unique_ptr<Runtime::SceneStructuralParticipant> BehaviorRuntime::MakeStructuralParticipant() {
        return std::make_unique<Impl::StructuralParticipant>(impl_);
    }
}  // namespace Horo::Gameplay
