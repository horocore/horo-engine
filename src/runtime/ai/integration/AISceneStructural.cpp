#include "AISceneRuntimeDetail.h"
#include "Horo/AI/AIErrors.h"
#include "Horo/AI/AIScenePerceptionSource.h"

#include <algorithm>
#include <limits>
#include <new>
#include <ranges>
#include <utility>

namespace Horo::AI::Detail {
    /** @copydoc AiSceneRuntimeState::RetirePerceptionSources */
    void AiSceneRuntimeState::RetirePerceptionSources(const std::span<const PerceptionSourceRef> sources) noexcept {
        for (auto &agent : agents) {
            if (!agent.sensing)
                continue;
            auto &memory = agent.sensing->memory;
            const auto handle = agent.record.handle;
            const auto watches =
                scheduler ? scheduler->PerceptionWatches(handle, agent.cookedPlan) : std::span<const PerceptionListenerTypeId>{};
            if (watches.empty()) {
                memory.ForgetRetiredSources(sources);
                continue;
            }
            const auto previous = memory;
            memory.ForgetRetiredSources(sources);
            if (std::ranges::any_of(watches, [&](const auto listener) {
                return memory.DecisionFactsChanged(previous, listener);
            }))
                static_cast<void>(scheduler->Wake(handle, {.perception = true}));
        }
    }

    namespace {
        /** @brief Rejects stale publication without changing the active AI population. */
        template <typename T> Result<T> Failure(const char *message) {
            return Result<T>::Failure(MakeError(AIErrors::SceneActivationInvalid, message));
        }

        /** @brief Detached additions and preallocated indexes; existing tasks never move during preparation. */
        class StructuralCandidate final : public Runtime::SceneStructuralCandidate {
        public:
            StructuralCandidate() = default;
            StructuralCandidate(const StructuralCandidate &) = delete;
            StructuralCandidate &operator=(const StructuralCandidate &) = delete;

            std::shared_ptr<AiSceneRuntimeState> state;
            Runtime::RuntimeSceneView scene;
            std::uint64_t revision{};
            std::uint64_t lastAgentIdentity{};
            std::uint32_t nextTaskSlot{};
            std::vector<Runtime::SceneAgentIdentityProjection> identities;
            std::vector<AgentRuntimeState> additions;
            std::vector<Horo::Handle<AgentHandleTag>> slots;
            std::vector<std::size_t> removals;
            std::vector<AgentRuntimeState> retired;
            std::vector<AgentRuntimeState> tombstones;
            decltype(AiSceneRuntimeState::agentsByOwner) owners;
            std::optional<PerceptionGroupPublication> perception;
            std::vector<PerceptionSourceRef> retiredPerceptionSources;
            AiTaskScheduler *scheduler{};
            std::unique_ptr<AiTaskScheduler::PopulationChange> decisions;
            bool published{};

            ~StructuralCandidate() override {
                for (auto &agent : additions)
                    CancelOwnedWork(agent);
                for (auto &agent : retired)
                    CancelOwnedWork(agent);
            }

            std::span<const Runtime::SceneAgentIdentityProjection> AgentIdentities() const noexcept override {
                return identities;
            }

            Result<void> ValidatePublication() const override {
                if (!state || state->closed || state->revision != revision || !scene.IsCurrent() ||
                    state->binding.scene != scene.RuntimeId())
                    return Failure<void>("The staged AI group lost its exact owner or Scene publication.");
                if (state->scheduler.get() != scheduler)
                    return Failure<void>("The staged AI group lost its decision scheduler.");
                if (perception && !perception->IsCurrent())
                    return Result<void>::Failure(MakeError(AIErrors::PerceptionSpatialStale));
                if (!additions.empty() && state->perceptionExecution && !state->perceptionExecution->generation->CanAdmit())
                    return Result<void>::Failure(MakeError(AIErrors::RuntimeUnavailable));
                if (auto current = ValidateCurrentCookedDecisions(additions); current.HasError())
                    return current;
                return decisions ? decisions->ValidatePublication() : Result<void>::Success();
            }

            void Publish() noexcept override {
                if (published)
                    return;
                // Every vector/index has its final capacity before the aggregate publication fence.
                for (std::size_t removed = 0; removed < removals.size(); ++removed) {
                    const auto index = removals[removed];
                    retired.push_back(std::move(state->agents[index]));
                    // Cancellation only signals owned tokens here. Executor callbacks and storage teardown run after publication.
                    retired.back().cancellation.RequestCancellation();
                    state->agents[index] = std::move(tombstones[removed]);
                }
                for (std::size_t index = 0; index < additions.size(); ++index) {
                    const auto destination = slots[index].index;
                    if (destination == state->agents.size())
                        state->agents.push_back(std::move(additions[index]));
                    else
                        state->agents[destination] = std::move(additions[index]);
                }
                additions.clear();
                state->agentsByOwner.swap(owners);
                if (!retiredPerceptionSources.empty())
                    state->RetirePerceptionSources(retiredPerceptionSources);
                if (decisions)
                    decisions->Publish();
                state->lastAgentIdentity = lastAgentIdentity;
                state->nextTaskSlot = nextTaskSlot;
                ++state->revision;
                if (perception)
                    perception->Publish();
                published = true;
            }

            Result<void> AfterPublication() override {
                // Cancellation/blackboard observer teardown is deferred until every owner and Scene is visible.
                if (decisions)
                    decisions->FinishRetirements();
                for (auto &agent : retired)
                    CancelOwnedWork(agent);
                retired.clear();
                return Result<void>::Success();
            }
        };
    }  // namespace

    /** @brief Explicit runtime borrow resolved at preparation, allowing registration before initial Scene activation. */
    class AiSceneStructuralParticipant final : public Runtime::SceneStructuralParticipant {
    public:
        AiSceneStructuralParticipant(AiSceneRuntime &runtime, const std::span<const AiControllerDescriptor> descriptors,
                                     std::optional<AiScenePerceptionGroupInput> perception)
            : runtime_(&runtime), descriptors_(descriptors.begin(), descriptors.end()), perception_(std::move(perception)) {}

        Runtime::SceneStructuralOwner Owner() const noexcept override {
            return Runtime::SceneStructuralOwner::AI;
        }

        Runtime::SceneStructuralPreparationPhase PreparationPhase() const noexcept override {
            return Runtime::SceneStructuralPreparationPhase::Identity;
        }

        Result<std::unique_ptr<Runtime::SceneStructuralCandidate>> Prepare(const Runtime::RuntimeSceneView &scene,
                                                                           const std::span<const Runtime::RuntimeEntityView> created,
                                                                           const std::span<const Runtime::EntityRef> destroyed) override {
            return PrepareWithHierarchy(scene, created, destroyed, {});
        }

        Result<std::unique_ptr<Runtime::SceneStructuralCandidate>> PrepareWithHierarchy(
            const Runtime::RuntimeSceneView &scene, std::span<const Runtime::RuntimeEntityView> created,
            std::span<const Runtime::EntityRef> destroyed, const Runtime::SceneStructuralHierarchyView &hierarchy) override {
            if (runtime_->shutdown_ || !runtime_->active_ || runtime_->active_->closed || !scene.IsCurrent() ||
                runtime_->active_->binding.scene != scene.RuntimeId())
                return Failure<std::unique_ptr<Runtime::SceneStructuralCandidate>>(
                    "AI structural admission requires the active Scene owner.");
            try {
                return PrepareOwned(scene, created, destroyed, hierarchy);
            } catch (const std::bad_alloc &) {
                return Result<std::unique_ptr<Runtime::SceneStructuralCandidate>>::Failure(
                    MakeError(AIErrors::AgentCapacityExceeded, "AI structural preparation could not allocate bounded staging storage."));
            }
        }

    private:
        Result<std::unique_ptr<Runtime::SceneStructuralCandidate>> PrepareOwned(const Runtime::RuntimeSceneView &scene,
                                                                                const std::span<const Runtime::RuntimeEntityView> created,
                                                                                const std::span<const Runtime::EntityRef> destroyed,
                                                                                const Runtime::SceneStructuralHierarchyView &hierarchy) {
            auto candidate = std::make_unique<StructuralCandidate>();
            candidate->state = runtime_->active_;
            candidate->scene = scene;
            candidate->revision = candidate->state->revision;
            if (candidate->revision == std::numeric_limits<std::uint64_t>::max())
                return Failure<std::unique_ptr<Runtime::SceneStructuralCandidate>>("AI structural mutation revision is exhausted.");

            auto &state = *candidate->state;
            if (state.perceptionExecution && !destroyed.empty()) {
                candidate->retiredPerceptionSources.reserve(destroyed.size());
                for (const auto entity : destroyed)
                    candidate->retiredPerceptionSources.push_back(ProjectPerceptionSource(entity));
                std::ranges::sort(candidate->retiredPerceptionSources);
            }
            for (std::size_t index = 0; index < state.agents.size(); ++index) {
                const auto &agent = state.agents[index];
                if (!agent.retired && std::ranges::find(destroyed, agent.record.owner) != destroyed.end())
                    candidate->removals.push_back(index);
            }
            auto agents = ProjectAgents(created);
            if (auto reserved = ReserveIdentities(*candidate, agents, created); reserved.HasError())
                return Result<std::unique_ptr<Runtime::SceneStructuralCandidate>>::Failure(reserved.ErrorValue());
            // Reuse only retired slots with a fresh generation. Exhausted generations remain permanent tombstones.
            for (std::size_t index = 0; index < state.agents.size() && candidate->slots.size() < agents.size(); ++index) {
                const auto &agent = state.agents[index];
                if ((agent.retired || std::ranges::find(candidate->removals, index) != candidate->removals.end()) &&
                    agent.record.handle.slot.generation < std::numeric_limits<std::uint32_t>::max())
                    candidate->slots.push_back({static_cast<std::uint32_t>(index), agent.record.handle.slot.generation + 1});
            }
            const auto appended = agents.size() - candidate->slots.size();
            if (appended > runtime_->settings_.maximumAgents - state.agents.size())
                return Result<std::unique_ptr<Runtime::SceneStructuralCandidate>>::Failure(MakeError(AIErrors::AgentCapacityExceeded));
            for (std::size_t index = 0; index < appended; ++index)
                candidate->slots.push_back({static_cast<std::uint32_t>(state.agents.size() + index), 1});
            for (const auto &input : agents) {
                for (std::size_t index = 0; index < state.agents.size(); ++index) {
                    const auto &resident = state.agents[index];
                    if (!resident.retired && std::ranges::find(candidate->removals, index) == candidate->removals.end() &&
                        (resident.record.owner == input.owner || resident.record.agent.agent == input.agent.agent))
                        return Failure<std::unique_ptr<Runtime::SceneStructuralCandidate>>(
                            "AI group conflicts with a resident agent identity.");
                }
            }
            auto prepared = PrepareStructuralAgents(state.binding, agents, descriptors_, runtime_->settings_, candidate->slots);
            if (prepared.HasError())
                return Result<std::unique_ptr<Runtime::SceneStructuralCandidate>>::Failure(prepared.ErrorValue());
            candidate->additions = std::move(prepared).Value();
            if (auto capacity = ValidatePerceptionCapacity(state.agents, candidate->additions, candidate->removals); capacity.HasError())
                return Result<std::unique_ptr<Runtime::SceneStructuralCandidate>>::Failure(capacity.ErrorValue());
            if (perception_) {
                auto preparedPerception =
                    PerceptionGroupPublication::Prepare(*perception_, scene, created, destroyed, candidate->additions, hierarchy);
                if (preparedPerception.HasError())
                    return Result<std::unique_ptr<Runtime::SceneStructuralCandidate>>::Failure(preparedPerception.ErrorValue());
                candidate->perception = std::move(preparedPerception).Value();
            }
            if (state.perceptionExecution && !candidate->additions.empty()) {
                if (!perception_ || &perception_->spatial != state.perceptionSpatial)
                    return Result<std::unique_ptr<Runtime::SceneStructuralCandidate>>::Failure(
                        MakeError(AIErrors::PerceptionDependencyMissing));
                auto senses = PreparePerceptionSenses(*state.perceptionExecution, state.agents, candidate->additions, candidate->removals);
                if (senses.HasError())
                    return Result<std::unique_ptr<Runtime::SceneStructuralCandidate>>::Failure(senses.ErrorValue());
            }
            if (auto staged = PrepareDecisionPopulation(*candidate); staged.HasError())
                return Result<std::unique_ptr<Runtime::SceneStructuralCandidate>>::Failure(staged.ErrorValue());
            PreparePublicationStorage(*candidate, agents, appended);
            return Result<std::unique_ptr<Runtime::SceneStructuralCandidate>>::Success(std::move(candidate));
        }

        /** @brief Stages new executions and exact retirements before the same Scene publication fence. */
        Result<void> PrepareDecisionPopulation(StructuralCandidate &candidate) const {
            const auto &state = *candidate.state;
            candidate.scheduler = state.scheduler.get();
            candidate.nextTaskSlot = state.nextTaskSlot;
            auto additions = runtime_->PrepareCookedDecisionRegistrations(candidate.state, candidate.additions, candidate.nextTaskSlot);
            if (additions.HasError())
                return Result<void>::Failure(additions.ErrorValue());
            if (!candidate.scheduler)
                return additions.Value().empty() ? Result<void>::Success() : Result<void>::Failure(MakeError(AIErrors::RuntimeUnavailable));
            if (additions.Value().empty() && candidate.removals.empty())
                return Result<void>::Success();

            std::vector<AgentHandle> removals;
            removals.reserve(candidate.removals.size());
            for (const auto index : candidate.removals)
                removals.push_back(state.agents[index].record.handle);
            auto staged = candidate.scheduler->PreparePopulationChange(additions.Value(), removals);
            if (staged.HasError())
                return Result<void>::Failure(staged.ErrorValue());
            candidate.decisions = std::move(staged).Value();
            return Result<void>::Success();
        }

        /** @brief Recognizes exact new prefab admission without rewriting ordinary authored Scene identities. */
        static bool IsPrefabAgent(const Runtime::RuntimeEntityView &entity) {
            return entity.components && entity.components->aiAgent && !entity.groupSpawnLineage.empty() &&
                   std::ranges::any_of(entity.groupAssets, [&](const auto &asset) {
                return asset.metadata.id == entity.groupSpawnLineage.back() && asset.metadata.expectedType.Value() == "core.prefab" &&
                       !asset.artifact.Bytes().empty();
            });
        }

        /** @brief Reserves detached logical IDs; rollback does not advance the published owner's high-water. */
        static Result<void> ReserveIdentities(StructuralCandidate &candidate, std::span<AiSceneAgentDescriptor> agents,
                                              const std::span<const Runtime::RuntimeEntityView> created) {
            candidate.lastAgentIdentity = candidate.state->lastAgentIdentity;
            // Ordinary authored IDs retain their established identity and reserve their domain before any prefab allocation.
            for (const auto &agent : agents) {
                const auto entity = std::ranges::find(created, agent.owner, &Runtime::RuntimeEntityView::entity);
                if (entity == created.end())
                    return Failure<void>("AI identity input has no exact candidate entity.");
                if (!IsPrefabAgent(*entity))
                    candidate.lastAgentIdentity = std::max(candidate.lastAgentIdentity, agent.agent.agent.Value());
            }
            candidate.identities.reserve(agents.size());
            for (auto &agent : agents) {
                if (const auto entity = std::ranges::find(created, agent.owner, &Runtime::RuntimeEntityView::entity);
                    !IsPrefabAgent(*entity))
                    continue;
                if (!agent.agent.agent.IsValid() || candidate.lastAgentIdentity == std::numeric_limits<std::uint64_t>::max())
                    return Failure<void>("Prefab AI identity reservation is invalid or exhausted.");
                const auto original = agent.agent.agent;
                agent.agent.agent = AgentId::Create(++candidate.lastAgentIdentity).Value();
                candidate.identities.emplace_back(agent.owner, original, agent.agent.agent);
            }
            return Result<void>::Success();
        }

        /** @brief Copies only typed AI projections into detached agent descriptors. */
        static std::vector<AiSceneAgentDescriptor> ProjectAgents(const std::span<const Runtime::RuntimeEntityView> created) {
            std::vector<AiSceneAgentDescriptor> agents;
            agents.reserve(created.size());
            for (const auto &entity : created) {
                if (!entity.components || (!entity.components->aiAgent && !entity.components->aiController))
                    continue;
                agents.push_back({.owner = entity.entity,
                                  .agent = entity.components->aiAgent.value_or(AiAgentComponent{}),
                                  .controller = entity.components->aiController});
            }
            return agents;
        }

        /** @brief Reserves final indexes and teardown storage while resident task ownership is unchanged. */
        static void PreparePublicationStorage(StructuralCandidate &candidate, const std::span<const AiSceneAgentDescriptor> agents,
                                              const std::size_t appended) {
            auto &state = *candidate.state;
            candidate.retired.reserve(candidate.removals.size());
            candidate.tombstones.reserve(candidate.removals.size());
            for (const auto index : candidate.removals) {
                AgentRuntimeState tombstone;
                tombstone.record.handle = state.agents[index].record.handle;
                tombstone.retired = true;
                candidate.tombstones.push_back(std::move(tombstone));
            }
            candidate.owners.reserve(state.agentsByOwner.size() + agents.size());
            for (const auto &[owner, index] : state.agentsByOwner)
                if (!state.agents[index].retired && std::ranges::find(candidate.removals, index) == candidate.removals.end())
                    candidate.owners.emplace(owner, index);
            for (std::size_t index = 0; index < agents.size(); ++index)
                candidate.owners.emplace(agents[index].owner, candidate.slots[index].index);
            state.agents.reserve(state.agents.size() + appended);
        }

        AiSceneRuntime *runtime_;
        std::vector<AiControllerDescriptor> descriptors_;
        std::optional<AiScenePerceptionGroupInput> perception_;
    };

    /** @copydoc MakeStructuralParticipant */
    std::unique_ptr<Runtime::SceneStructuralParticipant> MakeStructuralParticipant(
        AiSceneRuntime &runtime, const std::span<const AiControllerDescriptor> descriptors,
        std::optional<AiScenePerceptionGroupInput> perception) {
        return std::make_unique<AiSceneStructuralParticipant>(runtime, descriptors, std::move(perception));
    }
}  // namespace Horo::AI::Detail
