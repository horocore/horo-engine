/** @file
 * @brief Private Scene-owner spawn state and retained operation evidence.
 */
#pragma once

#include "Horo/Prefab/PrefabErrors.h"
#include "Horo/Prefab/PrefabSpawnService.h"

#include <algorithm>
#include <limits>
#include <thread>

namespace Horo::Prefab::Detail {
    /** @brief Owned group identities qualified by unique service and module scope, with no Scene borrow. */
    struct PrefabInstanceState final {
        std::shared_ptr<const std::byte> identity;
        std::uint64_t scope{};
        std::vector<Runtime::EntityRef> entities;
    };

    /** @brief Bounded owned request, cancellation and Scene receipt; observation never owns the service. */
    struct PrefabSpawnOperation final {
        explicit PrefabSpawnOperation(const CancellationToken &parent) : cancellation(parent) {}

        Prefab::PrefabSpawnState status{Prefab::PrefabSpawnState::Pending};
        CancellationSource cancellation;
        CancellationToken scopeCancellation;
        std::uint64_t scope{};
        std::uint64_t due{};
        bool despawn{};
        PrefabSpawnRequest request;
        std::optional<PrefabTemplateLoadHandle> load;
        std::optional<PrefabTemplateLease> lease;
        PrefabInstance instance;
        std::shared_ptr<PrefabInstanceState> prepared;
        std::shared_ptr<const Runtime::SceneStructuralReceipt> receipt;
        std::optional<Error> error;
        std::vector<Assets::AssetId> lineage;

        /** @brief Records committed identity evidence without allocation, including shutdown during post-commit hooks. */
        void ObserveSuccess() noexcept {
            if (!receipt || !receipt->ResultValue() || status == Prefab::PrefabSpawnState::Committed)
                return;
            if (!despawn && prepared) {
                for (const auto &entry : receipt->ResultValue()->created)
                    prepared->entities.push_back(entry.entity);
                instance.state_ = std::move(prepared);
            }
            status = Prefab::PrefabSpawnState::Committed;
            load.reset();
            lease.reset();
        }

        /** @brief Preserves the original failure while retiring only unpublished preparation. */
        void Fail(Error failure) {
            error = std::move(failure);
            status = cancellation.Token().IsCancellationRequested() || scopeCancellation.IsCancellationRequested()
                         ? Prefab::PrefabSpawnState::Cancelled
                         : Prefab::PrefabSpawnState::Failed;
            load.reset();
            lease.reset();
            prepared.reset();
        }
    };

    /** @brief Single-owner state whose shutdown severs every borrowed authority before retained clients use it. */
    struct PrefabSpawnState final {
        PrefabSpawnState(PrefabTemplateProvider &templates, Runtime::RuntimeSceneService &sceneService,
                         const Gameplay::ComponentRegistry *registry)
            : provider(&templates), scenes(&sceneService), components(registry) {
            if (const auto active = scenes->ActiveScene())
                scene = active->RuntimeId();
        }

        /** @brief Validates owner lane and exact scene before dereferencing any borrowed authority. */
        [[nodiscard]] Result<void> Check() const {
            if (owner != std::this_thread::get_id())
                return Result<void>::Failure(MakeError(PrefabErrors::AdmissionRejected, "Prefab requests require the Scene owner lane."));
            if (closed || !provider || !scenes || !scene.IsValid())
                return Result<void>::Failure(MakeError(PrefabErrors::SceneUnavailable));
            if (!provider->UsesSceneService(*scenes))
                return Result<void>::Failure(MakeError(PrefabErrors::AdmissionRejected));
            if (const auto active = scenes->ActiveScene(); !active || active->RuntimeId() != scene)
                return Result<void>::Failure(MakeError(PrefabErrors::SceneUnavailable));
            return Result<void>::Success();
        }

        /** @brief Checks finite request/observation capacities and overflow-safe scheduling before allocation. */
        [[nodiscard]] Result<void> Admit(const std::uint64_t due, const CancellationToken &scope, const CancellationToken &cancellation) {
            if (auto valid = Check(); valid.HasError())
                return valid;
            if (scope.IsCancellationRequested() || cancellation.IsCancellationRequested())
                return Result<void>::Failure(MakeError(PrefabErrors::Cancelled));
            if (due == 0 || due < tick || due - tick > 64 || due > std::numeric_limits<std::uint64_t>::max() - 64)
                return Result<void>::Failure(MakeError(PrefabErrors::AdmissionRejected, "Invalid bounded prefab safe-point tick."));
            std::erase_if(retained, [](const auto &entry) {
                return entry.expired();
            });
            if (pending.size() >= 32 || retained.size() >= 128)
                return Result<void>::Failure(MakeError(PrefabErrors::AdmissionRejected, "Prefab operation capacity exhausted."));
            // Both allocations precede worker admission, so accepted loads cannot be abandoned by queue growth.
            pending.reserve(32);
            retained.reserve(128);
            return Result<void>::Success();
        }

        /** @brief Rejects absent, oversized and duplicate copied initialization values before load admission. */
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

        /** @brief Rejects invalid, stale and duplicate external bindings on the Scene owner lane. */
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

        /** @brief Admits copied initialization identities without inspecting source data or queuing structural work. */
        [[nodiscard]] Result<PrefabOperation> Spawn(const PrefabSpawnRequest &request, const std::uint64_t scopeId,
                                                    const CancellationToken &scope, const CancellationToken &cancellation,
                                                    const std::span<const Assets::AssetId> lineage) {
            if (auto valid = Admit(request.notBeforeTick, scope, cancellation); valid.HasError())
                return Result<PrefabOperation>::Failure(valid.ErrorValue());
            if (lineage.size() >= 16)
                return Result<PrefabOperation>::Failure(MakeError(PrefabErrors::SpawnDepthExceeded));
            if (std::ranges::find(lineage, request.source.asset) != lineage.end())
                return Result<PrefabOperation>::Failure(MakeError(PrefabErrors::SpawnRecursionDetected));
            if (request.placement.TryToMatrix().HasError() || request.initialization.size() > MaximumPrefabInitializationValues ||
                request.bindings.size() > 64)
                return Result<PrefabOperation>::Failure(MakeError(PrefabErrors::AdmissionRejected));
            if (auto valid = ValidateInitialization(request); valid.HasError())
                return Result<PrefabOperation>::Failure(valid.ErrorValue());
            if (auto valid = ValidateBindings(request); valid.HasError())
                return Result<PrefabOperation>::Failure(valid.ErrorValue());
            if (request.parent && scenes->ActiveScene()->Get(*request.parent).HasError())
                return Result<PrefabOperation>::Failure(MakeError(PrefabErrors::InvalidParent));
            auto operation = std::make_shared<PrefabSpawnOperation>(cancellation);
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
            PrefabOperation handle;
            handle.operation_ = std::move(operation);
            return Result<PrefabOperation>::Success(std::move(handle));
        }

        /** @brief Accepts only exact committed group identities created under this module scope. */
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

        /** @brief Validates declared coverage, supplied types and constraints before constructing the detached candidate. */
        [[nodiscard]] Result<void> ValidateInputs(const PrefabSpawnOperation &operation) const {
            const auto &data = operation.lease->Template()->Data();
            for (const auto &input : operation.request.initialization) {
                const auto declaration = std::ranges::find(data.initialization, input.id, &CookedPrefabInitialization::id);
                if (declaration == data.initialization.end())
                    return Result<void>::Failure(MakeError(PrefabErrors::AdmissionRejected, "Undeclared prefab initialization ID."));
                if (auto valid = ValidatePrefabInitializationValue(*declaration, input.value); valid.HasError())
                    return valid;
            }
            for (const auto &declaration : data.initialization) {
                if (declaration.required && std::ranges::find(operation.request.initialization, declaration.id,
                                                              &PrefabInitializationValue::id) == operation.request.initialization.end())
                    return Result<void>::Failure(MakeError(PrefabErrors::AdmissionRejected, "Required prefab initialization is missing."));
            }
            const auto active = scenes->ActiveScene();
            for (const auto &input : operation.request.bindings) {
                const auto declaration = std::ranges::find(data.bindings, input.id, &CookedPrefabBindingDeclaration::id);
                if (declaration == data.bindings.end())
                    return Result<void>::Failure(MakeError(PrefabErrors::ReferenceRewriteInvalid));
                auto target = active->Get(input.target);
                if (target.HasError())
                    return Result<void>::Failure(target.ErrorValue());
                if (declaration->componentType &&
                    !std::ranges::any_of(target.Value().components->gameplayComponents, [&](const auto &component) {
                    return component.typeId == *declaration->componentType;
                }))
                    return Result<void>::Failure(MakeError(PrefabErrors::ReferenceRewriteInvalid));
            }
            for (const auto &declaration : data.bindings) {
                if (declaration.required && std::ranges::find(operation.request.bindings, declaration.id, &PrefabRuntimeBinding::id) ==
                                                operation.request.bindings.end())
                    return Result<void>::Failure(MakeError(PrefabErrors::ReferenceRewriteInvalid));
            }
            return Result<void>::Success();
        }

        /** @brief Projects existing immutable members through current schema metadata, never a source parser or override engine. */
        struct Projection final {
            std::vector<Runtime::RuntimeComponentSet> components;
            std::vector<std::vector<Runtime::RuntimeGroupMemberIdentity>> members;
            std::vector<std::vector<Runtime::RuntimeGroupReference>> references;
        };

        /** @brief Projects one cooked member while retaining the original behavior identity allocation order. */
        [[nodiscard]] Result<void> ProjectMember(const PrefabSpawnOperation &operation, Projection &projection, const std::size_t entity,
                                                 const std::size_t member) {
            const auto &data = operation.lease->Template()->Data();
            auto &target = projection.components[entity];
            const auto &source = data.entities[entity].members[member];
            if (const auto *raw = std::get_if<RawComponentPayload>(&source)) {
                if (!components || !components->IsFrozen())
                    return Result<void>::Failure(MakeError(PrefabErrors::ComponentTypeUnregistered));
                auto inspected = components->Inspect(raw->component);
                if (inspected.HasError())
                    return Result<void>::Failure(inspected.ErrorValue());
                if (inspected.Value().status != Gameplay::ComponentInspectionStatus::Current)
                    return Result<void>::Failure(MakeError(PrefabErrors::ComponentTypeUnregistered));
                target.gameplayComponents.push_back(raw->component);
                projection.members[entity].emplace_back(raw->component.typeId, raw->instance.Value());
                return Result<void>::Success();
            }
            if (lastBehaviorId == std::numeric_limits<std::uint64_t>::max())
                return Result<void>::Failure(MakeError(PrefabErrors::EntityAllocationExhausted));
            auto behavior = std::get<Gameplay::BehaviorComponent>(source);
            behavior.instanceId.value = ++lastBehaviorId;
            for (const auto &declaration : data.initialization) {
                if (declaration.owner.entity.value != entity || declaration.owner.member != member)
                    continue;
                if (const auto input = std::ranges::find(operation.request.initialization, declaration.id, &PrefabInitializationValue::id);
                    input != operation.request.initialization.end())
                    behavior.fields[declaration.field].value = input->value;
            }
            projection.members[entity].emplace_back(behavior.typeId, behavior.instanceId.value);
            target.behaviors.push_back(std::move(behavior));
            return Result<void>::Success();
        }

        /** @brief Projects portable references after every cooked member identity has been assigned. */
        void ProjectReferences(const PrefabSpawnOperation &operation, Projection &projection) const {
            const auto &data = operation.lease->Template()->Data();
            for (const auto &reference : data.references) {
                Runtime::RuntimeGroupReference output{reference.owner.member, reference.property.Value(), {}};
                std::visit([&data, &operation, &output]<typename Target>(const Target &target) {
                    if constexpr (std::is_same_v<Target, CookedPrefabEntitySlot>)
                        output.target = Runtime::RuntimeGroupEntitySlot{target.value};
                    else if constexpr (std::is_same_v<Target, CookedPrefabMemberSlot>)
                        output.target = Runtime::RuntimeGroupMemberSlot{{target.entity.value}, target.member};
                    else if constexpr (std::is_same_v<Target, CookedPrefabAssetSlot>)
                        output.target = data.dependencies[target.value].asset;
                    else {
                        const auto &declaration = data.bindings[target.value];
                        if (const auto input = std::ranges::find(operation.request.bindings, declaration.id, &PrefabRuntimeBinding::id);
                            input != operation.request.bindings.end())
                            output.target = Runtime::RuntimeGroupExternalReference{input->target, declaration.componentType};
                    }
                }, reference.target);
                projection.references[reference.owner.entity.value].push_back(std::move(output));
            }
        }

        /** @brief Reserves ordered projection containers and stops on the first typed member failure. */
        [[nodiscard]] Result<Projection> Project(const PrefabSpawnOperation &operation) {
            if (auto valid = ValidateInputs(operation); valid.HasError())
                return Result<Projection>::Failure(valid.ErrorValue());
            const auto active = scenes->ActiveScene();
            for (std::size_t slot = 0; slot < active->SlotCount(); ++slot) {
                if (const auto entity = active->EntityAt(slot)) {
                    for (const auto &behavior : entity->components->behaviors)
                        lastBehaviorId = std::max(lastBehaviorId, behavior.instanceId.value);
                }
            }
            Projection projection;
            const auto &data = operation.lease->Template()->Data();
            projection.components.resize(data.entities.size());
            projection.members.resize(data.entities.size());
            projection.references.resize(data.entities.size());
            for (std::size_t entity = 0; entity < data.entities.size(); ++entity) {
                for (std::size_t member = 0; member < data.entities[entity].members.size(); ++member) {
                    if (auto projected = ProjectMember(operation, projection, entity, member); projected.HasError())
                        return Result<Projection>::Failure(projected.ErrorValue());
                }
            }
            ProjectReferences(operation, projection);
            return Result<Projection>::Success(std::move(projection));
        }

        /** @brief Queues one fully prepared transaction; Scene retains artifacts and repeats all admission fences. */
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
                auto projection = std::move(projected).Value();
                auto queued =
                    provider->QueuePreparedGroup(*operation.lease, std::move(projection.components), operation.cancellation.Token(),
                                                 {.rootPlacement = operation.request.placement,
                                                  .parent = operation.request.parent,
                                                  .receipt = &operation.receipt,
                                                  .scopeCancellation = operation.scopeCancellation,
                                                  .members = std::move(projection.members),
                                                  .references = std::move(projection.references),
                                                  .spawnLineage = operation.lineage});
                if (queued.HasError())
                    return Result<void>::Failure(queued.ErrorValue());
            }
            return Result<void>::Success();
        }

        /** @brief Converts exact Scene receipts into durable observation without calling hooks or consuming host errors. */
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

        /** @brief Retires unavailable preparation without discarding completed Scene publication evidence. */
        void RetireUnavailable(const Error &failure) {
            for (const auto &operation : pending) {
                if (operation->receipt && operation->receipt->Complete())
                    Observe(*operation);
                else
                    operation->Fail(failure);
            }
            pending.clear();
        }

        /** @brief Propagates provider errors only to operations that have not entered Scene submission. */
        void FailUnsubmitted(const Error &failure) {
            for (const auto &operation : pending) {
                if (!operation->receipt)
                    operation->Fail(failure);
            }
            std::erase_if(pending, [](const auto &operation) {
                return operation->status != Prefab::PrefabSpawnState::Pending;
            });
        }

        /** @brief Transfers a completed load once; loading or failed operations cannot enter the structural queue. */
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

        PrefabTemplateProvider *provider;
        Runtime::RuntimeSceneService *scenes;
        const Gameplay::ComponentRegistry *components;
        const std::thread::id owner{std::this_thread::get_id()};
        Runtime::SceneRuntimeId scene;
        std::shared_ptr<const std::byte> identity{std::make_shared<const std::byte>()};
        CancellationSource retirement;
        std::vector<std::shared_ptr<PrefabSpawnOperation>> pending;
        std::vector<std::weak_ptr<PrefabSpawnOperation>> retained;
        std::vector<std::weak_ptr<Gameplay::GameplayPrefabContext>> scopes;
        std::uint64_t tick{};
        std::uint64_t lastBehaviorId{};
        std::uint64_t nextScope{1};
        bool closed{};
    };
}  // namespace Horo::Prefab::Detail
