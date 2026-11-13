/** @file
 * @brief Private Scene-owner spawn state and retained operation evidence.
 */
#pragma once

#include "Horo/Prefab/PrefabErrors.h"
#include "Horo/Prefab/PrefabSpawnService.h"
#include "Horo/Prefab/PreparedPrefabSpawn.h"
#include "Horo/Runtime/Scene/BuiltinSceneComponents.h"
#include "PrefabRuntimeProjection.h"

#include <algorithm>
#include <functional>
#include <limits>
#include <thread>

namespace Horo::Prefab::Detail {
    /** @brief Owned group identities qualified by unique service and module scope,
     * with no Scene borrow. */
    struct PrefabInstanceState final {
        std::shared_ptr<const std::byte> identity;
        std::uint64_t scope{};
        std::vector<Runtime::EntityRef> entities;
        std::shared_ptr<IPrefabPersistencePublication> persistence;
    };

    /** @brief Bounded owned request, cancellation and Scene receipt; observation
     * never owns the service. */
    struct PrefabSpawnOperation final {
        explicit PrefabSpawnOperation(const CancellationToken &parent) : cancellation(parent) {}

        PrefabMetricOperation measurement;
        Prefab::PrefabSpawnState status{Prefab::PrefabSpawnState::Pending};
        CancellationSource cancellation;
        CancellationToken scopeCancellation;
        std::uint64_t scope{};
        std::uint64_t due{};
        bool despawn{};
        PrefabSpawnRequest request;
        std::optional<PrefabTemplateLoadHandle> load;
        std::optional<PrefabTemplateLease> lease;
        std::optional<PreparedPrefabSpawn> preparedTemplate;
        PrefabInstance instance;
        std::shared_ptr<PrefabInstanceState> prepared;
        std::shared_ptr<const Runtime::SceneStructuralReceipt> receipt;
        std::optional<Error> error;
        const Error cancelledFailure{MakeError(PrefabErrors::Cancelled)}; /**< Prepared before admission; noexcept
                                                                             shutdown only borrows it. */
        std::vector<Assets::AssetId> lineage;
        std::string creator;

        /** @brief Adopts exactly the real receipt's complete group using capacity reserved before queue admission. */
        void PublishCreatedGroup() noexcept {
            if (!prepared)
                return;
            for (const auto &entry : receipt->ResultValue()->created)
                prepared->entities.push_back(entry.entity);
            if (prepared->persistence)
                prepared->persistence->PublishSpawn(prepared->entities);
            instance.state_ = std::move(prepared);
        }

        /** @brief Records explicit deletion only from a successful actual group-destruction receipt. */
        void PublishDestroyedGroup() noexcept {
            if (instance.state_ && instance.state_->persistence)
                instance.state_->persistence->PublishDespawn();
        }

        /** @brief Records committed facts without allocation, including shutdown during actual post-commit hooks. */
        void ObserveSuccess() noexcept {
            if (!receipt || !receipt->ResultValue() || status == Prefab::PrefabSpawnState::Committed)
                return;
            if (despawn)
                PublishDestroyedGroup();
            else
                PublishCreatedGroup();
            measurement.Observe(PrefabMetricItem::RuntimeEntities, receipt->ResultValue()->created.size());
            measurement.Complete();
            status = Prefab::PrefabSpawnState::Committed;
            load.reset();
            lease.reset();
            preparedTemplate.reset();
        }

        /** @brief Preserves the original failure while retiring only unpublished
         * preparation. */
        void Fail(Error failure) {
            error = std::move(failure);
            status = cancellation.Token().IsCancellationRequested() || scopeCancellation.IsCancellationRequested()
                         ? Prefab::PrefabSpawnState::Cancelled
                         : Prefab::PrefabSpawnState::Failed;
            if (status == Prefab::PrefabSpawnState::Cancelled)
                measurement.Complete(PrefabMetricOutcome::Cancelled);
            else
                measurement.Fail(*error);
            load.reset();
            lease.reset();
            preparedTemplate.reset();
            if (prepared && prepared->persistence)
                prepared->persistence->Cancel();
            prepared.reset();
        }
    };

    /** @brief Single-owner state whose shutdown severs every borrowed authority
     * before retained clients use it. */
    struct PrefabSpawnState final {
        using SpawnDispatcher =
            std::function<Result<PrefabOperation>(PrefabSpawnState &, const PrefabSpawnRequest &, std::uint64_t, const CancellationToken &,
                                                  const CancellationToken &, std::span<const Assets::AssetId>, std::string_view)>;
        using DespawnDispatcher =
            std::function<Result<PrefabOperation>(PrefabSpawnState &, const PrefabInstance &, std::uint64_t, std::uint64_t,
                                                  const CancellationToken &, const CancellationToken &)>;

        PrefabSpawnState(PrefabTemplateProvider &templates, Runtime::RuntimeSceneService &sceneService,
                         const Gameplay::ComponentRegistry *registry, const PrefabComponentProjector *codec, SpawnDispatcher spawn,
                         DespawnDispatcher despawn, std::shared_ptr<IPrefabPersistenceOwner> persistence,
                         std::shared_ptr<const PrefabValueSchemaSnapshot> schemas)
            : submitSpawn(std::move(spawn)), submitDespawn(std::move(despawn)), provider(&templates), scenes(&sceneService),
              components(registry), projector(codec), metrics(templates.Metrics()),
              maximumSpawnDepth(templates.LimitProfile().Policy().maximumRuntimeSpawnDepth),
              maximumInstanceBindings(templates.LimitProfile().Policy().maximumInstanceBindings), persistence(std::move(persistence)),
              schemas(std::move(schemas)) {
            if (const auto active = scenes->ActiveScene())
                scene = active->RuntimeId();
        }

        /** @brief Rejects closed/foreign owners before borrowing any Scene/provider; this check grants no command permission. */
        Result<void> CheckOwner() const {
            if (owner != std::this_thread::get_id())
                return Result<void>::Failure(MakeError(PrefabErrors::AdmissionRejected, "Prefab requests require the Scene owner lane."));
            if (closed || !provider || !scenes || !scene.IsValid())
                return Result<void>::Failure(MakeError(PrefabErrors::SceneUnavailable));
            if (!provider->UsesSceneService(*scenes))
                return Result<void>::Failure(MakeError(PrefabErrors::AdmissionRejected));
            return Result<void>::Success();
        }

        /** @brief All command/reference/tick admission still requires the actual published active Scene incarnation. */
        Result<void> Check() const {
            if (auto owner = CheckOwner(); owner.HasError())
                return owner;
            const auto active = scenes->ActiveScene();
            if (!active || active->RuntimeId() != scene)
                return Result<void>::Failure(MakeError(PrefabErrors::SceneUnavailable));
            return Result<void>::Success();
        }

        /** @brief Cold attachment binding may read only the actual retained private candidate; it cannot queue/execute commands. */
        Result<Runtime::RuntimeSceneView> BindingView() const {
            if (auto owner = CheckOwner(); owner.HasError())
                return Result<Runtime::RuntimeSceneView>::Failure(owner.ErrorValue());
            const auto active = scenes->ActiveScene();
            if (active && active->RuntimeId() == scene)
                return Result<Runtime::RuntimeSceneView>::Success(*active);
            if (preparedScene && preparedScene->View().RuntimeId() == scene)
                return Result<Runtime::RuntimeSceneView>::Success(preparedScene->View());
            return Result<Runtime::RuntimeSceneView>::Failure(MakeError(PrefabErrors::SceneUnavailable));
        }

        /** @brief Overflow-safe bounded tick admission shared by the existing request path. */
        bool ValidDue(std::uint64_t due) const noexcept {
            return due != 0 && due >= tick && due - tick <= 64 && due <= std::numeric_limits<std::uint64_t>::max() - 64;
        }

        /** @brief Checks finite request/observation capacities and overflow-safe
         * scheduling before allocation. */
        [[nodiscard]] Result<void> Admit(const std::uint64_t due, const CancellationToken &scope, const CancellationToken &cancellation,
                                         bool *overflow = nullptr) {
            if (auto valid = Check(); valid.HasError())
                return valid;
            if (scope.IsCancellationRequested() || cancellation.IsCancellationRequested())
                return Result<void>::Failure(MakeError(PrefabErrors::Cancelled));
            if (!ValidDue(due))
                return Result<void>::Failure(MakeError(PrefabErrors::AdmissionRejected, "Invalid bounded prefab safe-point tick."));
            std::erase_if(retained, [](const auto &entry) {
                return entry.expired();
            });
            if (pending.size() >= 32 || retained.size() >= 128) {
                if (overflow)
                    *overflow = true;
                return Result<void>::Failure(MakeError(PrefabErrors::AdmissionRejected, "Prefab operation capacity exhausted."));
            }
            // Both allocations precede worker admission, so accepted loads cannot be
            // abandoned by queue growth.
            pending.reserve(32);
            retained.reserve(128);
            return Result<void>::Success();
        }

        /** @brief Rejects absent, oversized and duplicate copied initialization
         * values before load admission. */
        [[nodiscard]] static Result<void> ValidateInitialization(const PrefabSpawnRequest &request) {
            for (std::size_t index = 0; index < request.initialization.size(); ++index) {
                const auto &input = request.initialization[index];
                if (input.id.value == 0 || input.value.index() == 0 ||
                    (std::holds_alternative<std::string>(input.value) && std::get<std::string>(input.value).size() > 256))
                    return Result<void>::Failure(MakeError(PrefabErrors::AdmissionRejected));
                for (std::size_t prior = 0; prior < index; ++prior) {
                    if (request.initialization[prior].id == input.id)
                        return Result<void>::Failure(MakeError(PrefabErrors::AdmissionRejected));
                }
            }
            return Result<void>::Success();
        }

        /** @brief Rejects invalid, stale and duplicate external bindings on the Scene
         * owner lane. */
        [[nodiscard]] Result<void> ValidateBindings(const PrefabSpawnRequest &request) const {
            for (std::size_t index = 0; index < request.bindings.size(); ++index) {
                if (request.bindings[index].id.Value() == 0 || scenes->ActiveScene()->Get(request.bindings[index].target).HasError())
                    return Result<void>::Failure(MakeError(PrefabErrors::ReferenceRewriteInvalid));
                for (std::size_t prior = 0; prior < index; ++prior) {
                    if (request.bindings[prior].id == request.bindings[index].id)
                        return Result<void>::Failure(MakeError(PrefabErrors::ReferenceRewriteInvalid));
                }
            }
            return Result<void>::Success();
        }

        /** @brief Retains inherited lineage bounds and exact recursion rejection before any copied request/load admission. */
        Result<void> ValidateSpawnLineage(const PrefabSpawnRequest &request, std::span<const Assets::AssetId> lineage) const {
            if (lineage.size() >= maximumSpawnDepth)
                return Result<void>::Failure(MakeError(PrefabErrors::SpawnDepthExceeded));
            if (std::ranges::find(lineage, request.source.asset) != lineage.end())
                return Result<void>::Failure(MakeError(PrefabErrors::SpawnRecursionDetected));
            return Result<void>::Success();
        }

        /** @brief Preserves finite placement/count admission and its exact overflow metric classification. */
        Result<void> ValidateSpawnEnvelope(const PrefabSpawnRequest &request, bool &overflow) const {
            if (request.placement.TryToMatrix().HasError() || request.initialization.size() > MaximumPrefabInitializationValues ||
                request.bindings.size() > maximumInstanceBindings) {
                overflow =
                    request.initialization.size() > MaximumPrefabInitializationValues || request.bindings.size() > maximumInstanceBindings;
                return Result<void>::Failure(MakeError(PrefabErrors::AdmissionRejected));
            }
            return Result<void>::Success();
        }

        /** @brief Checks exact copied typed values/interfaces before queued preparation; no source contents or defaults are inferred. */
        Result<void> ValidateSpawnInputs(const PrefabSpawnRequest &request) const {
            if (request.overrides)
                if (auto valid = ValidatePrefabValueOverrideSet(*request.overrides, provider->LimitProfile()); valid.HasError())
                    return valid;
            if (auto valid = ValidateInitialization(request); valid.HasError())
                return valid;
            if (auto valid = ValidateBindings(request); valid.HasError())
                return valid;
            if (request.parent && scenes->ActiveScene()->Get(*request.parent).HasError())
                return Result<void>::Failure(MakeError(PrefabErrors::InvalidParent));
            return Result<void>::Success();
        }

        /** @brief Rebinds the same receipt-qualified inventory to a fresh ordinary scope cookie. */
        Result<PrefabInstance> Lookup(Runtime::SavePrefabInstanceId instance, std::uint64_t scope, const CancellationToken &revocation,
                                      std::string_view creator) const {
            if (auto valid = Check(); valid.HasError())
                return Result<PrefabInstance>::Failure(valid.ErrorValue());
            if (revocation.IsCancellationRequested() || !persistence)
                return Result<PrefabInstance>::Failure(MakeError(PrefabErrors::AdmissionRejected));
            auto found = persistence->Lookup(instance, creator);
            if (found.HasError())
                return Result<PrefabInstance>::Failure(found.ErrorValue());
            auto state = std::make_shared<PrefabInstanceState>();
            state->identity = identity;
            state->scope = scope;
            state->entities = std::move(found.Value().entities);
            state->persistence = std::move(found.Value().publication);
            PrefabInstance handle;
            handle.state_ = std::move(state);
            return Result<PrefabInstance>::Success(std::move(handle));
        }

        /** @brief Admits copied initialization identities without inspecting source
         * data or queuing structural work. */
        [[nodiscard]] Result<PrefabOperation> Spawn(const PrefabSpawnRequest &request, const std::uint64_t scopeId,
                                                    const CancellationToken &scope, const CancellationToken &cancellation,
                                                    const std::span<const Assets::AssetId> lineage, std::string_view creator) {
            PrefabMetricOperation attempt{metrics, PrefabMetricPhase::Spawn};
            bool overflow{};
            auto result = [&]() -> Result<PrefabOperation> {
                if (auto valid = Admit(request.notBeforeTick, scope, cancellation, &overflow); valid.HasError())
                    return Result<PrefabOperation>::Failure(valid.ErrorValue());
                if (auto valid = ValidateSpawnLineage(request, lineage); valid.HasError())
                    return Result<PrefabOperation>::Failure(valid.ErrorValue());
                if (auto valid = ValidateSpawnEnvelope(request, overflow); valid.HasError())
                    return Result<PrefabOperation>::Failure(valid.ErrorValue());
                if (auto valid = ValidateSpawnInputs(request); valid.HasError())
                    return Result<PrefabOperation>::Failure(valid.ErrorValue());
                auto operation = std::make_shared<PrefabSpawnOperation>(cancellation);
                operation->creator = creator;
                operation->scope = scopeId;
                operation->scopeCancellation = scope;
                operation->due = request.notBeforeTick;
                operation->request = request;
                operation->lineage.assign(lineage.begin(), lineage.end());
                operation->lineage.push_back(operation->request.source.asset);
                auto load = provider->LoadAsync(operation->request.source, operation->cancellation.Token());
                if (load.HasError())
                    return Result<PrefabOperation>::Failure(load.ErrorValue());
                operation->load.emplace(std::move(load).Value());
                pending.push_back(operation);
                retained.push_back(operation);
                operation->measurement = std::move(attempt);
                PrefabOperation handle;
                handle.operation_ = std::move(operation);
                return Result<PrefabOperation>::Success(std::move(handle));
            }();
            if (result.HasError()) {
                if (overflow)
                    attempt.Complete(PrefabMetricOutcome::Overflow);
                else
                    attempt.Fail(result.ErrorValue());
            }
            return result;
        }

        /** @brief Accepts only exact committed group identities created under this
         * module scope. */
        [[nodiscard]] Result<PrefabOperation> Despawn(const PrefabInstance &instance, const std::uint64_t due, const std::uint64_t scopeId,
                                                      const CancellationToken &scope, const CancellationToken &cancellation) {
            if (auto valid = Admit(due, scope, cancellation); valid.HasError())
                return Result<PrefabOperation>::Failure(valid.ErrorValue());
            if (!instance.state_ || instance.state_->identity != identity || instance.state_->scope != scopeId ||
                instance.state_->entities.empty() || instance.Root().runtime != scene)
                return Result<PrefabOperation>::Failure(MakeError(PrefabErrors::AdmissionRejected));
            auto operation = std::make_shared<PrefabSpawnOperation>(cancellation);
            operation->scope = scopeId;
            operation->scopeCancellation = scope;
            operation->due = due;
            operation->despawn = true;
            operation->instance = instance;
            pending.push_back(operation);
            retained.push_back(operation);
            PrefabOperation handle;
            handle.operation_ = std::move(operation);
            return Result<PrefabOperation>::Success(std::move(handle));
        }

        /** @brief Uses exactly one canonical delta preparation and one cooked projection for normal and restored groups. */
        Result<PrefabGroupProjection> Project(PrefabSpawnOperation &operation) {
            auto prepared = PreparedPrefabSpawn::Prepare(*operation.lease, operation.request.overrides, schemas.get(),
                                                         provider->LimitProfile(), operation.cancellation.Token());
            if (prepared.HasError())
                return Result<PrefabGroupProjection>::Failure(prepared.ErrorValue());
            operation.preparedTemplate.emplace(std::move(prepared).Value());
            return PreparePrefabGroupProjection(operation.preparedTemplate->Data(), operation.request, *scenes->ActiveScene(), components,
                                                projector, lastBehaviorId);
        }

        /** @brief Reserves exact durable identity/lease metadata before the canonical
         * structural queue accepts a group. */
        Result<void> StagePersistence(PrefabSpawnOperation &operation, std::span<const Runtime::RuntimeComponentSet> projected) const {
            if (!persistence)
                return Result<void>::Success();
            auto prepared =
                persistence->PrepareSpawn(*operation.preparedTemplate, operation.request, projected, operation.lineage, operation.creator);
            if (prepared.HasError())
                return Result<void>::Failure(prepared.ErrorValue());
            if (!prepared.Value() || !prepared.Value()->InstanceIdentity().IsValid())
                return Result<void>::Failure(
                    MakeError(PrefabErrors::AdmissionRejected, "Prefab persistence owner returned no metadata transaction."));
            operation.prepared->persistence = std::move(prepared).Value();
            return Result<void>::Success();
        }

        /** @brief Queues one fully prepared transaction; Scene retains artifacts and
         * repeats all admission fences. */
        [[nodiscard]] Result<void> Queue(PrefabSpawnOperation &operation) {
            if (operation.despawn) {
                Runtime::SceneCommandBuffer commands;
                const auto active = scenes->ActiveScene();
                if (auto queued = commands.DestroyGroup(operation.instance.state_->entities,
                                                        {scene, active->AssetRegistryRevision(), operation.cancellation.Token(),
                                                         retirement.Token(), operation.scopeCancellation});
                    queued.HasError())
                    return queued;
                auto receipt = scenes->QueueTrackedStructuralCommands(std::move(commands));
                if (receipt.HasError())
                    return Result<void>::Failure(receipt.ErrorValue());
                operation.receipt = std::move(receipt).Value();
            } else {
                auto projected = Project(operation);
                if (projected.HasError())
                    return Result<void>::Failure(projected.ErrorValue());
                operation.prepared = std::make_shared<PrefabInstanceState>();
                operation.prepared->identity = identity;
                operation.prepared->scope = operation.scope;
                operation.prepared->entities.reserve(projected.Value().components.size());
                if (auto staged = StagePersistence(operation, projected.Value().components); staged.HasError())
                    return staged;
                auto projection = std::move(projected).Value();
                auto queued = provider->QueuePreparedGroup(*operation.preparedTemplate, std::move(projection.components),
                                                           operation.cancellation.Token(),
                                                           {.physicsReferences = std::move(projection.physicsReferences),
                                                            .rootPlacement = operation.request.placement,
                                                            .parent = operation.request.parent,
                                                            .receipt = &operation.receipt,
                                                            .scopeCancellation = operation.scopeCancellation,
                                                            .members = std::move(projection.members),
                                                            .references = std::move(projection.references),
                                                            .spawnLineage = operation.lineage,
                                                            .primitiveMeshes = std::move(projection.primitiveMeshes)});
                if (queued.HasError())
                    return Result<void>::Failure(queued.ErrorValue());
            }
            hasQueuedStructure = true;
            return Result<void>::Success();
        }

        /** @brief Converts exact Scene receipts into durable observation without
         * calling hooks or consuming host errors. */
        void Observe(PrefabSpawnOperation &operation) const {
            if (const auto *error = operation.receipt->Failure()) {
                operation.Fail(*error);
                return;
            }
            if (const auto *result = operation.receipt->ResultValue()) {
                (void)result;
                operation.ObserveSuccess();
            }
        }

        /** @brief Retires unavailable preparation without discarding completed Scene
         * publication evidence. */
        void RetireUnavailable(const Error &failure) {
            for (const auto &operation : pending) {
                if (operation->receipt && operation->receipt->Complete())
                    Observe(*operation);
                else
                    operation->Fail(failure);
            }
            pending.clear();
        }

        /** @brief Propagates provider errors only to operations that have not entered
         * Scene submission. */
        void FailUnsubmitted(const Error &failure) {
            for (const auto &operation : pending) {
                if (!operation->receipt)
                    operation->Fail(failure);
            }
            std::erase_if(pending, [](const auto &operation) {
                return operation->status != Prefab::PrefabSpawnState::Pending;
            });
        }

        /** @brief Transfers a completed load once; loading or failed operations
         * cannot enter the structural queue. */
        bool TakeLoadResult(PrefabSpawnOperation &operation) const {
            if (!operation.load || operation.lease)
                return true;
            if (operation.load->State() == PrefabTemplateLoadState::Loading)
                return false;
            auto lease = provider->TakeResult(*operation.load);
            if (lease.HasError()) {
                operation.Fail(lease.ErrorValue());
                return false;
            }
            operation.lease.emplace(std::move(lease).Value());
            operation.load.reset();
            return true;
        }

        // Only the explicit host service binds this route; operation/control-block
        // and cancellation destruction must remain in that image after a statically
        // linked module frontend unloads.
        const SpawnDispatcher submitSpawn;
        const DespawnDispatcher submitDespawn;
        PrefabTemplateProvider *provider;
        Runtime::RuntimeSceneService *scenes;
        const Gameplay::ComponentRegistry *components;
        const PrefabComponentProjector *projector;
        const PrefabMetricHandles metrics;
        const std::size_t maximumSpawnDepth; /**< Immutable provider policy; retained clients never
                                                query mutable project state. */
        const std::size_t maximumInstanceBindings;
        const std::thread::id owner{std::this_thread::get_id()};
        Runtime::SceneRuntimeId scene;
        const Runtime::RuntimeScene
            *preparedScene{}; /**< Actual candidate borrow; Scene-owned service shuts down before Scene destruction. */
        std::shared_ptr<const std::byte> identity{std::make_shared<const std::byte>()};
        CancellationSource retirement;
        std::vector<std::shared_ptr<PrefabSpawnOperation>> pending;
        std::vector<std::weak_ptr<PrefabSpawnOperation>> retained;
        std::vector<std::weak_ptr<Gameplay::GameplayPrefabContext>> scopes;
        std::uint64_t tick{};
        std::uint64_t lastBehaviorId{};
        std::uint64_t nextScope{1};
        std::shared_ptr<IPrefabPersistenceOwner> persistence;
        std::shared_ptr<const PrefabValueSchemaSnapshot> schemas;
        bool hasQueuedStructure{};
        bool closed{};
    };
}  // namespace Horo::Prefab::Detail
