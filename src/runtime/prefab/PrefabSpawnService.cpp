#include "Horo/Prefab/PrefabSpawnService.h"

#include "PrefabSpawnState.h"

namespace Horo::Gameplay {
    /** @copydoc GameplayPrefabContext::GameplayPrefabContext */
    GameplayPrefabContext::GameplayPrefabContext(ConstructionKey, std::shared_ptr<Prefab::Detail::PrefabSpawnState> state,
                                                 Prefab::GameplayPrefabBinding binding, const std::uint64_t scope, CancellationToken parent,
                                                 std::vector<Assets::AssetId> lineage)
        : state_(std::move(state)), binding_(std::move(binding)), revocation_(parent), scope_(scope), lineage_(std::move(lineage)) {}

    GameplayPrefabContext::~GameplayPrefabContext() {
        Revoke();
    }

    /** @copydoc GameplayPrefabContext::Spawn */
    Result<Prefab::PrefabOperation> GameplayPrefabContext::Spawn(const Prefab::PrefabSpawnRequest &request,
                                                                 const CancellationToken &cancellation) const {
        return state_->Spawn(request, scope_, revocation_.Token(), cancellation, lineage_);
    }

    /** @copydoc GameplayPrefabContext::Despawn */
    Result<Prefab::PrefabOperation> GameplayPrefabContext::Despawn(const Prefab::PrefabInstance &instance,
                                                                   const std::uint64_t notBeforeTick,
                                                                   const CancellationToken &cancellation) const {
        return state_->Despawn(instance, notBeforeTick, scope_, revocation_.Token(), cancellation);
    }

    /** @copydoc GameplayPrefabContext::Revoke */
    void GameplayPrefabContext::Revoke() const noexcept {
        revocation_.RequestCancellation();
    }

    /** @copydoc GameplayPrefabContext::Binding */
    const Prefab::GameplayPrefabBinding &GameplayPrefabContext::Binding() const noexcept {
        return binding_;
    }

    /** @copydoc GameplayPrefabContext::ForEntity */
    Result<std::shared_ptr<const GameplayPrefabContext>> GameplayPrefabContext::ForEntity(const Runtime::EntityRef entity) const {
        using Context = std::shared_ptr<const GameplayPrefabContext>;
        if (auto valid = state_->Check(); valid.HasError())
            return Result<Context>::Failure(valid.ErrorValue());
        auto owner = state_->scenes->ActiveScene()->Get(entity);
        if (owner.HasError())
            return Result<Context>::Failure(owner.ErrorValue());
        std::vector<Assets::AssetId> lineage(owner.Value().groupSpawnLineage.begin(), owner.Value().groupSpawnLineage.end());
        if (lineage.empty()) {
            for (const auto &resource : owner.Value().groupAssets) {
                if (resource.metadata.expectedType.Value() == "core.prefab") {
                    lineage.push_back(resource.metadata.id);
                    break;
                }
            }
        }
        Context child =
            std::make_shared<GameplayPrefabContext>(ConstructionKey{}, state_, binding_, scope_, revocation_.Token(), std::move(lineage));
        return Result<Context>::Success(std::move(child));
    }

    /** @copydoc GameplayPrefabContext::Reference */
    Result<Runtime::RuntimeResolvedGroupReference> GameplayPrefabContext::Reference(const Runtime::EntityRef owner,
                                                                                    const Runtime::RuntimeGroupMemberIdentity &member,
                                                                                    const Prefab::PrefabPropertyId property) const {
        using Reference = Runtime::RuntimeResolvedGroupReference;
        if (auto valid = state_->Check(); valid.HasError())
            return Result<Reference>::Failure(valid.ErrorValue());
        if (revocation_.Token().IsCancellationRequested())
            return Result<Reference>::Failure(MakeError(Prefab::PrefabErrors::Cancelled));
        if (!std::visit([&](const auto &type) {
            return type.Value().starts_with(binding_.moduleId + ".");
        }, member.type))
            return Result<Reference>::Failure(MakeError(Prefab::PrefabErrors::AdmissionRejected));
        const auto scene = state_->scenes->ActiveScene();
        auto entity = scene->Get(owner);
        if (entity.HasError())
            return Result<Reference>::Failure(entity.ErrorValue());
        const auto reference = std::ranges::find_if(entity.Value().groupReferences, [&](const auto &entry) {
            return entry.owner == member && entry.property == property.Value();
        });
        if (reference == entity.Value().groupReferences.end())
            return Result<Reference>::Failure(MakeError(Prefab::PrefabErrors::ReferenceRewriteInvalid));
        if (const bool available = std::visit(
                [&]<typename Target>(const Target &target) {
            if constexpr (std::is_same_v<Target, Runtime::EntityRef>)
                return scene->Get(target).HasValue();
            else if constexpr (std::is_same_v<Target, Runtime::RuntimeGroupMemberReference>)
                return scene->Get(target.entity).HasValue();
            else if constexpr (std::is_same_v<Target, Runtime::RuntimeGroupExternalReference>) {
                auto view = scene->Get(target.entity);
                return view.HasValue() &&
                       (!target.component || std::ranges::any_of(view.Value().components->gameplayComponents, [&](const auto &component) {
                    return component.typeId == *target.component;
                }));
            } else
                return true;
        }, reference->target);
            !available)
            return Result<Reference>::Failure(
                MakeError(Prefab::PrefabErrors::ReferenceRewriteInvalid, "Committed prefab reference target is no longer available."));
        return Result<Reference>::Success(*reference);
    }
}  // namespace Horo::Gameplay

namespace Horo::Prefab {
    /** @copydoc PrefabInstance::Root */
    Runtime::EntityRef PrefabInstance::Root() const noexcept {
        return state_ && !state_->entities.empty() ? state_->entities.front() : Runtime::EntityRef{};
    }

    /** @copydoc PrefabOperation::State */
    PrefabSpawnState PrefabOperation::State() const noexcept {
        using enum PrefabSpawnState;
        if (!operation_)
            return Failed;
        operation_->ObserveSuccess();
        if (operation_->receipt && operation_->receipt->Failure()) {
            return operation_->cancellation.Token().IsCancellationRequested() || operation_->scopeCancellation.IsCancellationRequested()
                       ? Cancelled
                       : Failed;
        }
        return operation_->status;
    }

    /** @copydoc PrefabOperation::Spawned */
    Result<PrefabInstance> PrefabOperation::Spawned() const {
        if (!operation_ || operation_->despawn)
            return Result<PrefabInstance>::Failure(MakeError(PrefabErrors::AdmissionRejected));
        const auto status = State();
        if (const auto *error = Failure())
            return Result<PrefabInstance>::Failure(*error);
        if (status == PrefabSpawnState::Cancelled)
            return Result<PrefabInstance>::Failure(MakeError(PrefabErrors::Cancelled));
        if (status != PrefabSpawnState::Committed)
            return Result<PrefabInstance>::Failure(MakeError(PrefabErrors::AssetNotLoaded));
        return Result<PrefabInstance>::Success(operation_->instance);
    }

    /** @copydoc PrefabOperation::Failure */
    const Error *PrefabOperation::Failure() const noexcept {
        if (!operation_)
            return nullptr;
        if (operation_->error)
            return &*operation_->error;
        return operation_->receipt ? operation_->receipt->Failure() : nullptr;
    }

    /** @copydoc PrefabOperation::Cancel */
    void PrefabOperation::Cancel() const noexcept {
        if (operation_)
            operation_->cancellation.RequestCancellation();
    }

    /** @copydoc PrefabSpawnService::PrefabSpawnService */
    PrefabSpawnService::PrefabSpawnService(PrefabTemplateProvider &provider, Runtime::RuntimeSceneService &scenes,
                                           const Gameplay::ComponentRegistry *components)
        : state_(std::make_shared<Detail::PrefabSpawnState>(provider, scenes, components)) {}

    PrefabSpawnService::~PrefabSpawnService() {
        Shutdown();
    }

    /** @copydoc PrefabSpawnService::Acquire */
    Result<std::shared_ptr<Gameplay::GameplayPrefabContext>> PrefabSpawnService::Acquire(GameplayPrefabBinding binding) const {
        using Context = std::shared_ptr<Gameplay::GameplayPrefabContext>;
        if (auto valid = state_->Check(); valid.HasError())
            return Result<Context>::Failure(valid.ErrorValue());
        std::erase_if(state_->scopes, [](const auto &entry) {
            return entry.expired();
        });
        if (!binding.enabled || !binding.permissionGranted || binding.scene != state_->scene || binding.moduleId.empty() ||
            binding.moduleId.size() > Gameplay::MaximumBehaviorTypeIdBytes || state_->scopes.size() >= 32 ||
            state_->nextScope == std::numeric_limits<std::uint64_t>::max())
            return Result<Context>::Failure(MakeError(PrefabErrors::AdmissionRejected));
        auto context = std::make_shared<Gameplay::GameplayPrefabContext>(Gameplay::GameplayPrefabContext::ConstructionKey{}, state_,
                                                                         std::move(binding), state_->nextScope);
        ++state_->nextScope;
        state_->scopes.push_back(context);
        return Result<Context>::Success(std::move(context));
    }

    /** @copydoc PrefabSpawnService::Advance */
    Result<void> PrefabSpawnService::Advance(const std::uint64_t tick) const {
        if (auto valid = state_->Check(); valid.HasError()) {
            state_->RetireUnavailable(valid.ErrorValue());
            return valid;
        }
        if (tick < state_->tick)
            return Result<void>::Failure(MakeError(PrefabErrors::AdmissionRejected));
        state_->tick = tick;
        if (auto advanced = state_->provider->Advance(); advanced.HasError()) {
            state_->FailUnsubmitted(advanced.ErrorValue());
            return advanced;
        }
        bool transactionPending = std::ranges::any_of(state_->pending, [](const auto &operation) {
            return operation->receipt && !operation->receipt->Complete();
        });
        for (const auto &operation : state_->pending) {
            if (operation->receipt) {
                state_->Observe(*operation);
                continue;
            }
            if (operation->cancellation.Token().IsCancellationRequested() || operation->scopeCancellation.IsCancellationRequested() ||
                tick > operation->due + 64) {
                operation->Fail(MakeError(PrefabErrors::Cancelled));
                continue;
            }
            if (!state_->TakeLoadResult(*operation))
                continue;
            if (tick < operation->due || transactionPending)
                continue;
            if (auto queued = state_->Queue(*operation); queued.HasError())
                operation->Fail(queued.ErrorValue());
            else
                transactionPending = true;
        }
        std::erase_if(state_->pending, [](const auto &operation) {
            return operation->status != PrefabSpawnState::Pending;
        });
        return Result<void>::Success();
    }

    /** @copydoc PrefabSpawnService::Startup */
    Result<void> PrefabSpawnService::Startup(const CancellationToken &cancellation) {
        if (cancellation.IsCancellationRequested())
            return Result<void>::Failure(MakeError(PrefabErrors::Cancelled));
        return state_->Check();
    }

    /** @copydoc PrefabSpawnService::OnPhase */
    Result<void> PrefabSpawnService::OnPhase(const Runtime::RuntimePhase phase, const Runtime::FrameContext &) {
        return phase == Runtime::RuntimePhase::CommitDeferredLifecycleChanges ? Advance(state_->tick) : Result<void>::Success();
    }

    /** @copydoc PrefabSpawnService::OnFixedUpdate */
    Result<void> PrefabSpawnService::OnFixedUpdate(const Runtime::FixedStepContext &context) {
        if (context.cancellation.IsCancellationRequested())
            return Result<void>::Failure(MakeError(PrefabErrors::Cancelled));
        if (context.simulationTick < state_->tick)
            return Result<void>::Failure(MakeError(PrefabErrors::AdmissionRejected));
        state_->tick = context.simulationTick;
        return state_->Check();
    }

    /** @copydoc PrefabSpawnService::Shutdown */
    void PrefabSpawnService::Shutdown() noexcept {
        state_->closed = true;
        state_->retirement.RequestCancellation();
        for (const auto &scope : state_->scopes) {
            if (auto context = scope.lock())
                context->Revoke();
        }
        for (const auto &operation : state_->pending) {
            if (operation->receipt && operation->receipt->ResultValue())
                state_->Observe(*operation);
            else {
                operation->cancellation.RequestCancellation();
                operation->load.reset();
                operation->lease.reset();
                operation->status = PrefabSpawnState::Cancelled;
            }
        }
        state_->pending.clear();
        state_->provider = nullptr;
        state_->scenes = nullptr;
        state_->components = nullptr;
    }
}  // namespace Horo::Prefab
