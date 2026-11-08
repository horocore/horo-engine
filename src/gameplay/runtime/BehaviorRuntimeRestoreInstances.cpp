#include "BehaviorRuntimeRestoreInternal.h"

#include <map>
#include <new>
#include <tuple>
#include <type_traits>

namespace Horo::Gameplay {
    namespace {
        Result<void> InvokeInactivePreparation(IBehaviorRestorePreparation &actual, const BehaviorRestoreInstance &instance) {
            static_assert(std::is_nothrow_move_constructible_v<Result<void>>);
            auto allocationFailure = Result<void>::Failure(MakeError(Runtime::SaveErrors::RestoreAllocationFailed));
            auto callbackFailure = Result<void>::Failure(MakeError(Runtime::SaveErrors::LifecycleCallbackFailed));
            try {
                return actual.PrepareInactive(instance);
            } catch (const std::bad_alloc &) {
                return allocationFailure;
            } catch (...) {
                return callbackFailure;
            }
        }

        Result<std::size_t> InstanceCount(const Runtime::RuntimeSceneView &scene, BehaviorRuntimeLimits limits) {
            std::size_t count{};
            for (std::size_t slot = 0; slot < scene.SlotCount(); ++slot) {
                const auto entity = scene.EntityAt(slot);
                if (!entity)
                    continue;
                const auto additions = entity->components->behaviors.size();
                if (additions > limits.maximumInstances - count)
                    return Result<std::size_t>::Failure(BehaviorRestoreError("Actual behavior inventory exceeds its declared bound."));
                count += additions;
            }
            return Result<std::size_t>::Success(count);
        }

        BehaviorLifecycleFlags Flags(const auto &instance) noexcept {
            return {instance.created, instance.enabledCallbackActive, instance.started, instance.component.enabled};
        }

        Result<BehaviorLifecycleEntry> CaptureOne(const auto &instance, const Runtime::PersistentEntityIdentityMap &map) {
            auto id = map.Find(instance.entity);
            if (id.HasError())
                return Result<BehaviorLifecycleEntry>::Failure(id.ErrorValue());
            const auto record = std::ranges::lower_bound(map.Records(), id.Value(), {}, &Runtime::PersistentEntityRecord::identity);
            if (record == map.Records().end() || record->identity != id.Value())
                return Result<BehaviorLifecycleEntry>::Failure(BehaviorRestoreError("Missing actual persistent entity."));
            return Result<BehaviorLifecycleEntry>::Success(
                {id.Value(), record->generation, instance.component.instanceId, instance.component.typeId, Flags(instance)});
        }

        using AttachmentKey = std::tuple<Runtime::EntityRef, std::uint64_t, BehaviorTypeId>;
        using AttachmentIndex = std::map<AttachmentKey, std::size_t>;

        Result<AttachmentIndex> IndexAttachments(const auto &instances) {
            AttachmentIndex result;
            for (std::size_t index = 0; index < instances.size(); ++index)
                if (!result
                         .emplace(AttachmentKey{instances[index].entity, instances[index].component.instanceId.value,
                                                instances[index].component.typeId},
                                  index)
                         .second)
                    return Result<AttachmentIndex>::Failure(BehaviorRestoreError("Actual attachment identity is duplicated."));
            return Result<AttachmentIndex>::Success(std::move(result));
        }

        bool LifecycleLess(const BehaviorLifecycleEntry &a, const BehaviorLifecycleEntry &b) {
            return std::tie(a.entity, a.instance.value, a.type) < std::tie(b.entity, b.instance.value, b.type);
        }
    }  // namespace

    Result<void> BehaviorRuntime::BindRestoreVfx(Impl &impl, BehaviorRuntimeServiceBindings &services) {
        if (!services.vfx) {
            if (!services.vfxModuleId.empty())
                return Result<void>::Failure(MakeError(GameplayVfxErrors::ScopeInvalid));
            Detail::GameplayVfxHostAccess::RevokeClient(impl.vfx);
            impl.vfx.reset();
            return Result<void>::Success();
        }
        if (services.vfxModuleId.empty() || services.vfx->ModuleId() != services.vfxModuleId ||
            services.vfx->Scene().Value() != impl.sceneIdentity.value || !services.vfx->IsUsable())
            return Result<void>::Failure(MakeError(GameplayVfxErrors::ScopeInvalid));
        auto child = Detail::GameplayVfxHostAccess::ForkRuntime(services.vfx);
        if (child.HasError())
            return Result<void>::Failure(child.ErrorValue());
        Detail::GameplayVfxHostAccess::RevokeClient(impl.vfx);
        impl.vfx = std::move(child).Value();
        return Result<void>::Success();
    }

    Result<void> BehaviorRuntime::BindRestorePhysics(Impl &impl, BehaviorRuntimeServiceBindings &services) {
        if (services.physics && services.physics->Binding().scene != impl.sceneIdentity.value)
            return Result<void>::Failure(MakeError(Physics::PhysicsErrors::HandleWorldMismatch));
        if (services.character && services.character->Binding().scene != 0 &&
            services.character->Binding().scene != impl.sceneIdentity.value)
            return Result<void>::Failure(MakeError(Character::CharacterErrors::HandleWorldMismatch));
        impl.physics = std::move(services.physics);
        if (!services.character)
            return Result<void>::Success();
        if (const auto dependency = services.character->ValidateDependency(impl.physics.get()); dependency.HasError())
            return dependency;
        auto child = services.character->Derive();
        if (child.HasError())
            return Result<void>::Failure(child.ErrorValue());
        impl.character = std::move(child).Value();
        return Result<void>::Success();
    }

    Result<void> BehaviorRuntime::BindRestoreNavigationUi(Impl &impl, BehaviorRuntimeServiceBindings &services) {
        if (services.navigation && services.navigation->Binding().scene.scene.IsValid() &&
            services.navigation->Binding().scene.scene.Value() != impl.sceneIdentity.value)
            return Result<void>::Failure(MakeError(Navigation::NavigationErrors::InvalidWorld));
        impl.navigation = std::move(services.navigation);
        if (!services.runtimeUi)
            return Result<void>::Success();
        if (services.runtimeUi->Grant().scene != impl.sceneIdentity.value)
            return Result<void>::Failure(MakeError(Runtime::Ui::UiErrors::HandleOwnerMismatch));
        auto child = services.runtimeUi->Derive();
        if (child.HasError())
            return Result<void>::Failure(child.ErrorValue());
        impl.runtimeUi = std::move(child).Value();
        return Result<void>::Success();
    }

    bool BehaviorRuntime::RestoreBindingAdmitted(const Impl &impl) noexcept {
        if (impl.shutdown || !impl.registry.IsFrozen())
            return false;
        const auto *admission = impl.registry.generationLeaseAdmission_;
        if (!admission)
            return !impl.generationLease;
        return impl.generationLease && admission->load(std::memory_order_acquire) &&
               impl.registry.generationLease_.lock().get() == impl.generationLease.get();
    }

    bool BehaviorRuntimeRestoreAccess::BindingAdmitted(const BehaviorRuntime &runtime) noexcept {
        return BehaviorRuntime::RestoreBindingAdmitted(*runtime.impl_);
    }

    Result<std::shared_ptr<void>> BehaviorRuntime::AcquireRestoreGeneration(const BehaviorRegistry &registry) {
        return registry.AcquireGenerationLease();
    }

    Result<void> BehaviorRuntimeRestoreAccess::ValidateGeneration(const BehaviorRuntime &runtime) {
        auto current = BehaviorRuntime::AcquireRestoreGeneration(runtime.impl_->registry);
        if (current.HasError())
            return Result<void>::Failure(current.ErrorValue());
        return current.Value().get() == runtime.impl_->generationLease.get()
                   ? Result<void>::Success()
                   : Result<void>::Failure(BehaviorRestoreError("Factory generation changed."));
    }

    Result<std::unique_ptr<BehaviorRuntime>> BehaviorRuntimeRestoreAccess::Construct(Runtime::RuntimeScene &scene,
                                                                                     const BehaviorRegistry &registry,
                                                                                     BehaviorRuntimeLimits limits) {
        using Outcome = Result<std::unique_ptr<BehaviorRuntime>>;
        if (!registry.IsFrozen() || limits.maximumInstances == 0)
            return Outcome::Failure(BehaviorRestoreError("Inactive runner requires a frozen registry and positive instance bound."));
        auto lease = BehaviorRuntime::AcquireRestoreGeneration(registry);
        if (lease.HasError())
            return Outcome::Failure(lease.ErrorValue());
        auto impl = std::make_shared<BehaviorRuntime::Impl>(scene, registry, limits, std::move(lease).Value());
        auto runtime = std::unique_ptr<BehaviorRuntime>{new BehaviorRuntime{impl}};
        impl->inactive = true;
        auto count = InstanceCount(scene.View(), limits);
        if (count.HasError())
            return Outcome::Failure(count.ErrorValue());
        impl->instances.reserve(count.Value());
        if (auto built = impl->BuildInstances(false); built.HasError())
            return Outcome::Failure(built.ErrorValue());
        return Outcome::Success(std::move(runtime));
    }

    Result<void> BehaviorRuntime::BindRestoreServices(Impl &impl, BehaviorRuntimeServiceBindings services,
                                                      std::shared_ptr<const GameplayPrefabContext> prefabs) {
        if (prefabs && prefabs->Binding().scene != impl.sceneIdentity)
            return Result<void>::Failure(MakeError(Prefab::PrefabErrors::SceneUnavailable));
        if (auto vfx = BindRestoreVfx(impl, services); vfx.HasError())
            return vfx;
        if (auto physics = BindRestorePhysics(impl, services); physics.HasError())
            return physics;
        if (auto others = BindRestoreNavigationUi(impl, services); others.HasError())
            return others;
        impl.prefabs = std::move(prefabs);
        for (auto &instance : impl.instances)
            if (auto bound = impl.InitializePrefabContext(instance); bound.HasError())
                return bound;
        return Result<void>::Success();
    }

    Result<void> BehaviorRuntimeRestoreAccess::ConfigureState(BehaviorRuntime::Impl &impl, BehaviorRuntimeServiceBindings services,
                                                              std::shared_ptr<const GameplayPrefabContext> prefabs) {
        return BehaviorRuntime::BindRestoreServices(impl, std::move(services), std::move(prefabs));
    }

    Result<void> BehaviorRuntimeRestoreAccess::Configure(BehaviorRuntime &runtime, BehaviorRuntimeServiceBindings services,
                                                         std::shared_ptr<const GameplayPrefabContext> prefabs,
                                                         Runtime::RuntimeSceneService *scenes) {
        if (!runtime.impl_->inactive || runtime.impl_->shutdown)
            return Result<void>::Failure(BehaviorRestoreError("Runtime is not inactive."));
        auto result = ConfigureState(*runtime.impl_, std::move(services), std::move(prefabs));
        if (result.HasValue())
            runtime.impl_->scenes = scenes;
        return result;
    }

    Result<void> BehaviorRuntimeRestoreAccess::Activate(BehaviorRuntime &runtime) {
        if (!runtime.impl_->inactive || runtime.impl_->shutdown)
            return Result<void>::Failure(BehaviorRestoreError("Ordinary activation is not pending."));
        auto result = runtime.impl_->ActivateInstances(0);
        if (result.HasValue())
            runtime.impl_->inactive = false;
        return result;
    }

    std::vector<BehaviorReloadLifecycle> BehaviorRuntimeRestoreAccess::LiveFlags(const BehaviorRuntime &runtime) {
        // The size constructor lets MSVC Debug iterator-proxy allocation reach the caller's bad_alloc guard.
        std::vector<BehaviorReloadLifecycle> result(0);
        result.reserve(runtime.impl_->instances.size());
        for (const auto &instance : runtime.impl_->instances)
            result.push_back({instance.entity, instance.component.instanceId, instance.component.typeId, Flags(instance)});
        return result;
    }

    Result<std::vector<BehaviorLifecycleFlags>> BehaviorRuntimeRestoreAccess::MatchReload(const BehaviorRuntime &runtime,
                                                                                          std::span<const BehaviorReloadLifecycle> saved) {
        using Outcome = Result<std::vector<BehaviorLifecycleFlags>>;
        if (saved.size() != runtime.impl_->instances.size())
            return Outcome::Failure(BehaviorRestoreError("Reload attachment inventory changed."));
        std::map<AttachmentKey, const BehaviorReloadLifecycle *> identities;
        for (const auto &entry : saved)
            if (!identities.emplace(AttachmentKey{entry.entity, entry.instance.value, entry.type}, &entry).second)
                return Outcome::Failure(BehaviorRestoreError("Reload attachment identity is duplicated."));
        std::vector<BehaviorLifecycleFlags> flags;
        flags.reserve(saved.size());
        for (const auto &instance : runtime.impl_->instances) {
            const auto found =
                identities.find(AttachmentKey{instance.entity, instance.component.instanceId.value, instance.component.typeId});
            if (found == identities.end())
                return Outcome::Failure(BehaviorRestoreError("Reload attachment ID changed."));
            const auto &entry = *found->second;
            flags.push_back(entry.flags);
        }
        return Outcome::Success(std::move(flags));
    }

    Result<void> BehaviorRuntimeRestoreAccess::RestoreReloadRunner(BehaviorRuntime &runtime, const BehaviorRuntimeReloadSnapshot &snapshot,
                                                                   std::span<const BehaviorLifecycleFlags> flags,
                                                                   BehaviorRuntimeServiceBindings services,
                                                                   std::shared_ptr<const GameplayPrefabContext> prefabs,
                                                                   Runtime::RuntimeSceneService *scenes) {
        if (auto configured = Configure(runtime, std::move(services), std::move(prefabs), scenes); configured.HasError())
            return configured;
        if (auto enabled = ReloadEnabled(runtime, flags); enabled.HasError())
            return enabled;
        if (auto activated = Activate(runtime); activated.HasError())
            return activated;
        if (auto restored = runtime.RestoreReloadSnapshot(snapshot); restored.HasError())
            return restored;
        Publish(runtime, flags);
        return Result<void>::Success();
    }

    Result<std::unique_ptr<BehaviorRuntime>> BehaviorRuntimeRestoreAccess::PrepareReloadRunner(
        Runtime::RuntimeScene &scene, const BehaviorRegistry &registry, BehaviorRuntimeLimits limits,
        const BehaviorRuntimeReloadSnapshot &snapshot, std::span<const BehaviorReloadLifecycle> lifecycle,
        BehaviorRuntimeServiceBindings services, std::shared_ptr<const GameplayPrefabContext> prefabs,
        Runtime::RuntimeSceneService *scenes) {
        using Outcome = Result<std::unique_ptr<BehaviorRuntime>>;
        try {
            auto built = Construct(scene, registry, limits);
            if (built.HasError())
                return built;
            auto runtime = std::move(built).Value();
            auto flags = MatchReload(*runtime, lifecycle);
            if (flags.HasError())
                return Outcome::Failure(flags.ErrorValue());
            if (auto restored = RestoreReloadRunner(*runtime, snapshot, flags.Value(), std::move(services), std::move(prefabs), scenes);
                restored.HasError())
                return Outcome::Failure(restored.ErrorValue());
            return Outcome::Success(std::move(runtime));
        } catch (const std::bad_alloc &) {
            return Outcome::Failure(MakeError(Runtime::SaveErrors::RestoreAllocationFailed));
        } catch (...) {
            return Outcome::Failure(MakeError(Runtime::SaveErrors::LifecycleCallbackFailed));
        }
    }

    Result<void> BehaviorRuntimeRestoreAccess::ReloadEnabled(BehaviorRuntime &runtime, std::span<const BehaviorLifecycleFlags> flags) {
        if (flags.size() != runtime.impl_->instances.size())
            return Result<void>::Failure(BehaviorRestoreError("Reload attachment count changed."));
        for (std::size_t i = 0; i < flags.size(); ++i)
            runtime.impl_->instances[i].component.enabled = flags[i].enabled;
        return Result<void>::Success();
    }

    std::shared_ptr<void> BehaviorRuntimeRestoreAccess::Generation(const BehaviorRuntime &runtime) {
        return runtime.impl_->generationLease;
    }

    Result<std::vector<BehaviorRestoreInstance>> BehaviorRuntimeRestoreAccess::Project(BehaviorRuntime &runtime) {
        std::vector<BehaviorRestoreInstance> result;
        result.reserve(runtime.impl_->instances.size());
        for (const auto &instance : runtime.impl_->instances) {
            auto entity = runtime.impl_->SceneView().Get(instance.entity);
            if (entity.HasError())
                return Result<std::vector<BehaviorRestoreInstance>>::Failure(entity.ErrorValue());
            result.push_back({BehaviorRuntimeDetail::ToGameplay(instance.entity),
                              instance.component.instanceId,
                              instance.component.typeId,
                              instance.component.fields,
                              *entity.Value().localTransform,
                              {},
                              0,
                              instance.implementation});
        }
        return Result<std::vector<BehaviorRestoreInstance>>::Success(std::move(result));
    }

    Result<std::vector<BehaviorAttachmentProjection>> BehaviorRuntimeRestoreAccess::AttachmentIdentities(
        const BehaviorRuntime &runtime, const BehaviorTypeId &type, const Runtime::PersistentEntityIdentityMap &map) {
        using Outcome = Result<std::vector<BehaviorAttachmentProjection>>;
        std::vector<BehaviorAttachmentProjection> result;
        for (const auto &instance : runtime.impl_->instances) {
            if (instance.component.typeId != type)
                continue;
            auto captured = CaptureOne(instance, map);
            if (captured.HasError())
                return Outcome::Failure(captured.ErrorValue());
            const auto &value = captured.Value();
            result.push_back({instance.entity, {value.entity, value.generation.value, value.instance}, value.type});
        }
        std::ranges::sort(result, [](const auto &a, const auto &b) {
            return AttachmentKey{a.runtime, a.identity.instance.value, a.type} <
                   AttachmentKey{b.runtime, b.identity.instance.value, b.type};
        });
        return Outcome::Success(std::move(result));
    }

    Result<void> BehaviorRuntime::CheckRestorePhysicsServices(const Impl &impl, std::uint32_t bits) {
        const auto unavailable = []() {
            return Result<void>::Failure(BehaviorRestoreError("Required restore physics service is not admitted for this candidate."));
        };
        if ((bits & 1U) != 0) {
            if (!impl.physics)
                return unavailable();
            const auto &binding = impl.physics->Binding();
            if (impl.physics->Acquire(binding.moduleId, impl.sceneIdentity.value, binding.sceneGeneration).HasError())
                return unavailable();
        }
        if ((bits & 4U) != 0) {
            if (!impl.character)
                return unavailable();
            const auto &binding = impl.character->Binding();
            if (impl.character->Acquire(binding.moduleId, impl.sceneIdentity.value, binding.sceneGeneration).HasError())
                return unavailable();
        }
        return Result<void>::Success();
    }

    Result<void> BehaviorRuntime::CheckRestoreServices(const Impl &impl, BehaviorRestoreServices required) {
        const auto bits = static_cast<std::uint32_t>(required);
        if ((bits & ~15U) != 0)
            return Result<void>::Failure(BehaviorRestoreError("Unknown required restore service."));
        if (auto physics = CheckRestorePhysicsServices(impl, bits); physics.HasError())
            return physics;
        const bool navigationReady = impl.navigation && impl.navigation->IsUsable();
        if ((bits & 2U) != 0 && !navigationReady)
            return Result<void>::Failure(BehaviorRestoreError("Required restore navigation service is unavailable."));
        if ((bits & 8U) != 0 && !impl.runtimeUi)
            return Result<void>::Failure(BehaviorRestoreError("Required restore UI service is unavailable."));
        return Result<void>::Success();
    }

    Result<void> BehaviorRuntimeRestoreAccess::ValidateKnownNoServiceRequirements(const BehaviorRuntime &runtime) {
        if (!BindingAdmitted(runtime) || runtime.impl_->inactive || runtime.impl_->structuralPending)
            return Result<void>::Failure(BehaviorRestoreError("Known restore requirement inspection requires an admitted active runtime."));
        for (const auto &instance : runtime.impl_->instances) {
            const auto *actual = dynamic_cast<const IBehaviorRestorePreparation *>(instance.implementation);
            if (!actual)
                return Result<void>::Failure(BehaviorRestoreError("Actual active behavior has no inactive restore prehook capability."));
            if (actual->RequiredRestoreServices() != BehaviorRestoreServices::None)
                return Result<void>::Failure(
                    BehaviorRestoreError("The no-candidate-services profile rejects required or unknown restore services."));
        }
        return Result<void>::Success();
    }

    Result<void> BehaviorRuntimeRestoreAccess::Prehook(BehaviorRuntime &runtime) {
        auto projections = Project(runtime);
        if (projections.HasError())
            return Result<void>::Failure(projections.ErrorValue());
        for (const auto &instance : projections.Value()) {
            auto *hook = dynamic_cast<IBehaviorRestorePreparation *>(instance.implementation);
            if (!hook)
                return Result<void>::Failure(BehaviorRestoreError("Actual behavior has no inactive restore preparation capability."));
            if (auto services = BehaviorRuntime::CheckRestoreServices(*runtime.impl_, hook->RequiredRestoreServices()); services.HasError())
                return services;
            if (auto prepared = InvokeInactivePreparation(*hook, instance); prepared.HasError())
                return prepared;
        }
        return Result<void>::Success();
    }

    Result<BehaviorLifecycleSnapshot> BehaviorRuntimeRestoreAccess::Capture(const BehaviorRuntime &runtime,
                                                                            const Runtime::PersistentEntityIdentityMap &map) {
        if (runtime.impl_->inactive || runtime.impl_->shutdown || runtime.impl_->structuralPending)
            return Result<BehaviorLifecycleSnapshot>::Failure(BehaviorRestoreError("Runtime cannot capture lifecycle in this phase."));
        BehaviorLifecycleSnapshot result;
        result.instances.reserve(runtime.impl_->instances.size());
        for (const auto &instance : runtime.impl_->instances) {
            auto value = CaptureOne(instance, map);
            if (value.HasError())
                return Result<BehaviorLifecycleSnapshot>::Failure(value.ErrorValue());
            result.instances.push_back(std::move(value).Value());
        }
        std::ranges::sort(result.instances, LifecycleLess);
        return Result<BehaviorLifecycleSnapshot>::Success(std::move(result));
    }

    Result<std::vector<BehaviorLifecycleFlags>> BehaviorRuntimeRestoreAccess::Match(const BehaviorRuntime &runtime,
                                                                                    const BehaviorLifecycleSnapshot &snapshot,
                                                                                    const Runtime::PersistentEntityIdentityMap &map) {
        using Outcome = Result<std::vector<BehaviorLifecycleFlags>>;
        const auto &instances = runtime.impl_->instances;
        if (snapshot.instances.size() != instances.size())
            return Outcome::Failure(BehaviorRestoreError("Saved lifecycle coverage differs from actual restored attachments."));
        auto indexed = IndexAttachments(instances);
        if (indexed.HasError())
            return Outcome::Failure(indexed.ErrorValue());
        const auto &indices = indexed.Value();
        std::vector<BehaviorLifecycleFlags> result(instances.size());
        std::vector<bool> matched(instances.size());
        for (const auto &saved : snapshot.instances) {
            if (!ValidLifecycleFlags(saved.flags))
                return Outcome::Failure(BehaviorRestoreError("Saved lifecycle flags are inconsistent."));
            auto entity = map.Resolve(saved.entity, saved.generation);
            if (entity.HasError())
                return Outcome::Failure(entity.ErrorValue());
            const auto found = indices.find(AttachmentKey{entity.Value(), saved.instance.value, saved.type});
            if (found == indices.end())
                return Outcome::Failure(BehaviorRestoreError("Saved attachment is absent in restored Scene."));
            const auto index = found->second;
            if (matched[index])
                return Outcome::Failure(BehaviorRestoreError("Saved lifecycle duplicate, entity or type mismatch."));
            matched[index] = true;
            result[index] = saved.flags;
        }
        return Outcome::Success(std::move(result));
    }

    void BehaviorRuntimeRestoreAccess::Publish(BehaviorRuntime &runtime, std::span<const BehaviorLifecycleFlags> flags) noexcept {
        auto &instances = runtime.impl_->instances;
        if (instances.size() != flags.size())
            std::terminate();
        for (std::size_t i = 0; i < instances.size(); ++i) {
            instances[i].created = flags[i].created;
            instances[i].enabledCallbackActive = flags[i].enabledCallbackActive;
            instances[i].started = flags[i].started;
            instances[i].component.enabled = flags[i].enabled;
        }
        runtime.impl_->inactive = false;
    }
}  // namespace Horo::Gameplay
