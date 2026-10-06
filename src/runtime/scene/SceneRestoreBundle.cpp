#include "Horo/Runtime/Scene/SceneRestoreBundle.h"

#include "RuntimeSceneErrors.h"

#include <algorithm>
#include <new>
#include <tuple>

namespace Horo::Runtime {
    namespace {
        /** @brief Retains original typed errors through the Scene-owned composition boundary. */
        template <typename T> Result<T> Failure(const ErrorCodeDescriptor &error) {
            return Result<T>::Failure(MakeError(error));
        }

        /** @brief Validates bounded component ownership and canonical schema-local selections before candidate creation. */
        Result<void> ValidateComponentSelections(SceneRestoreBundleRequest &request) {
            std::size_t bytes{};
            std::size_t selectedReferences{};
            for (auto &component : request.components) {
                if (const auto *owner = request.participants.Find(component.owner);
                    !owner || !HasSaveParticipantRole(owner->Descriptor().roles, SaveParticipantRole::Restore) ||
                    component.record.payload.size() > request.maximumComponentBytes - bytes ||
                    component.references.size() > request.graphLimits.maximumReferences - selectedReferences)
                    return Failure<void>(SaveErrors::RestoreParticipantInvalid);
                bytes += component.record.payload.size();
                selectedReferences += component.references.size();
                std::ranges::sort(component.references, {}, &SaveRestoreReferenceId::value);
                if ((!component.references.empty() && component.references.front().value == 0) ||
                    std::ranges::adjacent_find(component.references) != component.references.end())
                    return Failure<void>(SaveErrors::ReferenceInvalid);
            }
            return Result<void>::Success();
        }
    }  // namespace

    struct SceneRestoreBundle::State final : ISaveableComponentAuthority {
        struct ComponentCandidate final {
            SaveParticipantId owner;
            RestoreComponentReference source;
            std::vector<SaveRestoreReferenceId> references;
            std::unique_ptr<IPreparedSaveableComponentState> candidate;
        };

        State(SceneRestoreBundleRequest request, std::unique_ptr<ISceneRestoreOwnerSource> source,
              std::shared_ptr<const ISceneRestoreGenerationAuthority> authority) noexcept
            : request(std::move(request)), source(std::move(source)), authority(std::move(authority)) {}

        bool Contains(const EntityRef entity, const Gameplay::ComponentTypeId &type) const noexcept override {
            if (!scene)
                return false;
            const auto current = scene->View().Get(entity);
            return current.HasValue() && current.Value().components &&
                   std::ranges::any_of(current.Value().components->gameplayComponents, [&](const auto &component) {
                return component.typeId == type;
            });
        }

        /** @brief Applies validated child-first tombstones only to the unpublished Scene. */
        Result<void> ApplyTombstones() {
            // Child-first tombstones are applied only to this unpublished candidate. A live child of
            // a deleted parent remains a typed structural error rather than an implicit reparent.
            std::vector<std::pair<std::size_t, EntityRef>> tombstones;
            for (const auto &record : request.entities) {
                const auto *authored = std::get_if<AuthoredPersistentEntityProvenance>(&record.provenance);
                if (!authored || authored->scene != request.definition)
                    return Result<void>::Failure(MakeError(SaveErrors::RestoreParticipantInvalid,
                                                           "Spawned provenance requires the owning dynamic prefab restore adapter."));
                const auto entity = scene->View().Find(authored->object);
                if (record.disposition != PersistentEntityDisposition::Tombstone || !entity)
                    continue;
                std::size_t depth{};
                auto parent = scene->View().Get(*entity).Value().parent;
                while (parent) {
                    if (++depth > request.maximumEntities)
                        return Failure<void>(SaveErrors::RestoreParticipantInvalid);
                    auto current = scene->View().Get(*parent);
                    if (current.HasError())
                        return Result<void>::Failure(current.ErrorValue());
                    parent = current.Value().parent;
                }
                tombstones.emplace_back(depth, *entity);
            }
            std::ranges::sort(tombstones, std::greater<>{});
            SceneCommandBuffer commands;
            for (const auto &[depth, entity] : tombstones) {
                static_cast<void>(depth);
                commands.Destroy(entity);
            }
            if (!commands.Empty()) {
                const auto destroyed = scene->Commit(commands);
                if (destroyed.HasError())
                    return Result<void>::Failure(destroyed.ErrorValue());
            }
            return Result<void>::Success();
        }

        /** @brief Pins exact stable-to-runtime entity bindings after tombstone application. */
        Result<void> BindIdentities() {
            std::vector<PersistentEntityRuntimeBinding> bindings;
            bindings.reserve(request.entities.size());
            for (const auto &record : request.entities) {
                if (record.disposition != PersistentEntityDisposition::Live)
                    continue;
                const auto &authored = std::get<AuthoredPersistentEntityProvenance>(record.provenance);
                const auto entity = scene->View().Find(authored.object);
                if (!entity)
                    return Failure<void>(SaveErrors::ReferenceResolutionInvalid);
                bindings.emplace_back(record.identity, record.generation, *entity);
            }
            auto mapped =
                PersistentEntityIdentityMap::Create(scene->View().RuntimeId(), request.entities, bindings, request.maximumEntities);
            if (mapped.HasError())
                return Result<void>::Failure(mapped.ErrorValue());
            identities = std::move(mapped).Value();
            return Result<void>::Success();
        }

        /** @brief Stages core transform overrides through exact candidate identity bindings. */
        Result<void> ApplyTransforms() {
            SceneCommandBuffer transforms;
            for (const auto &transform : request.transforms) {
                auto entity = identities->Resolve(transform.entity, transform.generation);
                if (entity.HasError())
                    return Result<void>::Failure(entity.ErrorValue());
                transforms.SetLocalTransform(entity.Value(), transform.transform);
            }
            if (!transforms.Empty()) {
                const auto applied = scene->Commit(transforms);
                if (applied.HasError())
                    return Result<void>::Failure(applied.ErrorValue());
            }
            return Result<void>::Success();
        }

        /** @brief Creates actual inactive component candidates through their existing typed adapters. */
        Result<void> PrepareComponents() {
            components.reserve(request.components.size());
            for (const auto &component : request.components) {
                auto candidate = request.componentAdapters->PrepareRestore(component.record, *identities, *this);
                if (candidate.HasError())
                    return Result<void>::Failure(candidate.ErrorValue());
                if (candidate.Value())
                    components.push_back({component.owner,
                                          {{{request.world, component.record.entity}, component.record.generation}, component.record.type},
                                          component.references,
                                          std::move(candidate).Value()});
                else if (!component.references.empty())
                    return Failure<void>(SaveErrors::RestoreAdapterContractInvalid);
            }
            return Result<void>::Success();
        }

        /** @brief Delivers exact component-owned reference subsets after complete graph resolution. */
        Result<void> FixupComponents(const SaveRestoreReferenceContext &references) {
            for (auto &component : components) {
                const auto ownerView = references.ForParticipant(component.owner);
                std::vector<SaveRestoreReferenceResult> selected;
                selected.reserve(component.references.size());
                for (const auto identity : component.references) {
                    const auto *reference = ownerView.Find(identity);
                    if (const auto *resolved = graph->Find(component.owner, identity.value);
                        !reference || !resolved || resolved->request.source != RestoreReferenceTarget{component.source})
                        return Failure<void>(SaveErrors::ReferenceResolutionInvalid);
                    selected.push_back(*reference);
                }
                if (const auto fixed = component.candidate->FixupReferences({ownerView.generation, selected}); fixed.HasError())
                    return Result<void>::Failure(fixed.ErrorValue());
            }
            return Result<void>::Success();
        }

        /** @brief Resolves after all owner candidates have applied state, then delivers exact component-owned subsets. */
        struct Resolver final : IStagedRestoreReferenceResolver {
            explicit Resolver(State &state) noexcept : state(state) {}

            Result<SaveRestoreReferenceContext> Resolve(const StagedRestoreContext &context,
                                                        const std::span<const StagedRestorePreparedParticipant> participants) override {
                std::vector<SaveParticipantId> owners;
                owners.reserve(participants.size());
                for (const auto &participant : participants) {
                    if (!participant.state)
                        return Failure<SaveRestoreReferenceContext>(SaveErrors::RestoreAdapterContractInvalid);
                    owners.push_back(participant.requirement.participant);
                }
                for (const auto &component : state.components)
                    if (std::ranges::find(owners, component.owner) == owners.end())
                        return Failure<SaveRestoreReferenceContext>(SaveErrors::RestoreParticipantInvalid);
                const RestoreReferenceAuthorities authorities{.world = state.request.world,
                                                              .scene = state.scene->View(),
                                                              .identities = &*state.identities,
                                                              .components = &state,
                                                              .services = state.request.services,
                                                              .activeServices = state.request.activeServices,
                                                              .serviceGeneration = state.request.serviceGeneration,
                                                              .serviceLease = state.request.serviceLease,
                                                              .participants = state.request.participants,
                                                              .preparedOwners = owners};
                auto graph = PreparedRestoreReferenceGraph::Create(authorities, state.request.nodes, state.request.references,
                                                                   state.request.graphLimits);
                if (graph.HasError())
                    return Result<SaveRestoreReferenceContext>::Failure(graph.ErrorValue());
                state.graph = std::move(graph).Value();
                auto references = state.graph->MakeContext(
                    {context.sessionGeneration, context.registryGeneration, state.scene->View().RuntimeId().value});
                if (references.HasError())
                    return references;
                if (const auto fixed = state.FixupComponents(references.Value()); fixed.HasError())
                    return Result<SaveRestoreReferenceContext>::Failure(fixed.ErrorValue());
                return references;
            }

            State &state;
        };

        SceneRestoreBundleRequest request;
        std::unique_ptr<ISceneRestoreOwnerSource> source;
        std::shared_ptr<const ISceneRestoreGenerationAuthority> authority;
        RuntimeScene *scene{};
        std::optional<PersistentEntityIdentityMap> identities;
        std::optional<PreparedRestoreReferenceGraph> graph;
        std::vector<ComponentCandidate> components;
        std::optional<StagedRestoreTransaction> transaction;
        bool closed{};
        bool published{};
    };

    /** @copydoc SceneRestoreBundle::Create */
    Result<std::unique_ptr<SceneRestoreBundle>> SceneRestoreBundle::Create(
        SceneRestoreBundleRequest request, std::unique_ptr<ISceneRestoreOwnerSource> source,
        std::shared_ptr<const ISceneRestoreGenerationAuthority> authority) {
        if (!source || !authority || !request.world.IsValid() || !request.definition.IsValid() || request.revision.value == 0 ||
            !request.participants.IsValid() || request.participants.Generation() != request.context.registryGeneration ||
            request.context.operation == 0 || request.context.sessionGeneration == 0 || request.context.sceneIncarnation == 0 ||
            request.context.maximumParticipants == 0 || request.context.maximumParticipants > MaximumSaveParticipantCount ||
            request.maximumEntities == 0 || request.maximumEntities > 16'384 || request.entities.size() > request.maximumEntities ||
            request.transforms.size() > request.maximumEntities || request.components.size() > request.maximumEntities ||
            request.maximumComponentBytes == 0 || request.maximumComponentBytes > 16U * 1024U * 1024U ||
            request.graphLimits.maximumNodes == 0 || request.graphLimits.maximumNodes > 16'384 ||
            request.graphLimits.maximumReferences == 0 || request.graphLimits.maximumReferences > 65'536 ||
            request.activeServices.size() > Gameplay::MaximumGameplayServices || request.nodes.size() > request.graphLimits.maximumNodes ||
            request.references.size() > request.graphLimits.maximumReferences ||
            (!request.components.empty() && !request.componentAdapters))
            return Failure<std::unique_ptr<SceneRestoreBundle>>(SaveErrors::RestoreContextInvalid);
        if (const auto components = ValidateComponentSelections(request); components.HasError())
            return Result<std::unique_ptr<SceneRestoreBundle>>::Failure(components.ErrorValue());
        try {
            std::ranges::sort(request.transforms, [](const auto &left, const auto &right) {
                return left.entity < right.entity;
            });
            if (std::ranges::adjacent_find(request.transforms, [](const auto &left, const auto &right) {
                return left.entity == right.entity;
            }) != request.transforms.end())
                return Failure<std::unique_ptr<SceneRestoreBundle>>(SaveErrors::RestoreParticipantInvalid);
            std::ranges::sort(request.components, [](const auto &left, const auto &right) {
                return std::tie(left.record.entity, left.record.type) < std::tie(right.record.entity, right.record.type);
            });
            if (std::ranges::adjacent_find(request.components, [](const auto &left, const auto &right) {
                return left.record.entity == right.record.entity && left.record.type == right.record.type;
            }) != request.components.end())
                return Failure<std::unique_ptr<SceneRestoreBundle>>(SaveErrors::RestoreParticipantInvalid);
            auto state = std::make_unique<State>(std::move(request), std::move(source), std::move(authority));
            return Result<std::unique_ptr<SceneRestoreBundle>>::Success(
                std::make_unique<SceneRestoreBundle>(ConstructionKey{}, std::move(state)));
        } catch (const std::bad_alloc &) {
            return Failure<std::unique_ptr<SceneRestoreBundle>>(SaveErrors::RestoreAllocationFailed);
        }
    }

    /** @copydoc SceneRestoreBundle::PrepareScene */
    Result<void> SceneRestoreBundle::PrepareScene(RuntimeScene &scene) {
        auto &state = *state_;
        if (state.closed || state.scene || scene.View().DefinitionId() != state.request.definition ||
            scene.View().DefinitionRevision() != state.request.revision)
            return Failure<void>(SaveErrors::RestoreContextInvalid);
        state.scene = &scene;
        try {
            if (const auto removed = state.ApplyTombstones(); removed.HasError())
                return removed;
            if (const auto mapped = state.BindIdentities(); mapped.HasError())
                return mapped;
            if (const auto applied = state.ApplyTransforms(); applied.HasError())
                return applied;
            if (const auto prepared = state.PrepareComponents(); prepared.HasError())
                return prepared;
            return Result<void>::Success();
        } catch (const std::bad_alloc &) {
            return Failure<void>(SaveErrors::RestoreAllocationFailed);
        } catch (...) {
            // Component adapters may throw non-standard exceptions; the unpublished Scene must remain rollback-owned.
            return Failure<void>(SaveErrors::RestoreAdapterContractInvalid);
        }
    }

    /** @copydoc SceneRestoreBundle::PrepareOwners */
    Result<bool> SceneRestoreBundle::PrepareOwners() {
        auto &state = *state_;
        if (state.closed || !state.scene || !state.identities)
            return Failure<bool>(SaveErrors::RestoreTransitionInvalid);
        if (const auto current = state.authority->Current(); current.registryGeneration != state.request.context.registryGeneration ||
                                                             current.sessionGeneration != state.request.context.sessionGeneration ||
                                                             current.sceneIncarnation != state.request.context.sceneIncarnation)
            return Failure<bool>(SaveErrors::RestoreActivationStale);
        if (state.transaction) {
            if (state.transaction->State() == StagedRestoreTransactionState::ReadyToActivate)
                return Result<bool>::Success(true);
            if (const auto snapshot = state.transaction->Operation().Snapshot(); snapshot && snapshot->terminalError)
                return Result<bool>::Failure(*snapshot->terminalError);
            return Failure<bool>(SaveErrors::RestoreTransitionInvalid);
        }
        try {
            auto ready = state.source->TakeReady(state.scene->View());
            if (ready.HasError())
                return Result<bool>::Failure(ready.ErrorValue());
            if (!ready.Value())
                return Result<bool>::Success(false);
            auto owners = *std::move(ready).Value();
            auto transaction = StagedRestoreTransaction::Create(state.request.context, std::move(owners.operation),
                                                                state.request.participants, std::move(owners.receipts));
            if (transaction.HasError())
                return Result<bool>::Failure(transaction.ErrorValue());
            state.transaction = std::move(transaction).Value();
            if (const auto bound = state.transaction->SetReferenceResolver(std::make_unique<State::Resolver>(state)); bound.HasError())
                return Result<bool>::Failure(bound.ErrorValue());
            if (const auto prepared = state.transaction->Prepare(); prepared.HasError())
                return Result<bool>::Failure(prepared.ErrorValue());
            return Result<bool>::Success(true);
        } catch (const std::bad_alloc &) {
            return Failure<bool>(SaveErrors::RestoreAllocationFailed);
        } catch (...) {
            // The source/resolver may throw non-standard exceptions; sole producer and candidates remain rollback-owned.
            return Failure<bool>(SaveErrors::RestoreAdapterContractInvalid);
        }
    }

    /** @copydoc SceneRestoreBundle::ValidatePublication */
    Result<void> SceneRestoreBundle::ValidatePublication(const RuntimeSceneView *active) const {
        const auto &state = *state_;
        if (const auto evidence = state.authority->Current();
            !state.transaction || state.closed || !state.graph ||
            state.transaction->State() != StagedRestoreTransactionState::ReadyToActivate ||
            evidence.registryGeneration != state.request.context.registryGeneration ||
            evidence.sessionGeneration != state.request.context.sessionGeneration ||
            evidence.sceneIncarnation != state.request.context.sceneIncarnation ||
            (active && active->RuntimeId().value != state.request.context.sceneIncarnation))
            return Failure<void>(SaveErrors::RestoreActivationStale);
        return Result<void>::Success();
    }

    /** @copydoc SceneRestoreBundle::Commit */
    Result<void> SceneRestoreBundle::Commit(IStagedRestoreAggregatePublication &sceneTransfer) {
        auto &state = *state_;
        if (!state.transaction || state.closed || state.published)
            return Failure<void>(SaveErrors::RestoreTransitionInvalid);

        struct Transfer final : IStagedRestoreAggregatePublication {
            Transfer(State &state, IStagedRestoreAggregatePublication &scene) noexcept : state(state), scene(scene) {}

            void PublishPrepared() noexcept override {
                for (auto &component : state.components)
                    component.candidate->PublishPrepared();
                state.published = true;
                scene.PublishPrepared();
            }

            State &state;
            IStagedRestoreAggregatePublication &scene;
        };

        Transfer transfer{state, sceneTransfer};

        return state.transaction->Activate(state.authority->Current(), transfer);
    }

    /** @copydoc SceneRestoreBundle::Rollback */
    void SceneRestoreBundle::Rollback() noexcept {
        auto &state = *state_;
        if (state.closed || state.published)
            return;
        state.closed = true;
        state.source->Cancel();
        state.transaction.reset();
        state.components.clear();
        state.graph.reset();
        state.identities.reset();
        state.scene = nullptr;
    }

    /** @copydoc SceneRestoreBundle::Identities */
    const PersistentEntityIdentityMap *SceneRestoreBundle::Identities() const noexcept {
        return state_->identities ? &*state_->identities : nullptr;
    }

    /** @copydoc SceneRestoreBundle::~SceneRestoreBundle */
    SceneRestoreBundle::~SceneRestoreBundle() {
        Rollback();
    }

    /** @copydoc SceneRestoreBundle::SceneRestoreBundle */
    SceneRestoreBundle::SceneRestoreBundle(const ConstructionKey, std::unique_ptr<State> state) noexcept : state_(std::move(state)) {}
}  // namespace Horo::Runtime
