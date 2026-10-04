#include "AISceneRuntimeDetail.h"
#include "Horo/AI/AIErrors.h"

#include <algorithm>
#include <limits>
#include <new>
#include <ranges>
#include <utility>

namespace Horo::AI::Detail {
    namespace {
        /** @brief Rejects stale publication without changing the active AI population. */
        template <typename T> Result<T> Failure(const char *message) {
            return Result<T>::Failure(MakeError(AIErrors::SceneActivationInvalid, message));
        }

        /** @brief Detached additions and preallocated indexes; existing tasks never move during preparation. */
        class StructuralCandidate final : public Runtime::SceneStructuralCandidate {
        public:
            std::shared_ptr<AiSceneRuntimeState> state;
            Runtime::RuntimeSceneView scene;
            std::uint64_t revision{};
            std::vector<AgentRuntimeState> additions;
            std::vector<Horo::Handle<AgentHandleTag>> slots;
            std::vector<std::size_t> removals;
            std::vector<AgentRuntimeState> retired;
            std::vector<AgentRuntimeState> tombstones;
            decltype(AiSceneRuntimeState::agentsByOwner) owners;
            bool published{};

            ~StructuralCandidate() override {
                for (auto &agent : additions)
                    CancelOwnedWork(agent);
                for (auto &agent : retired)
                    CancelOwnedWork(agent);
            }

            Result<void> ValidatePublication() const override {
                if (!state || state->closed || state->revision != revision || !scene.IsCurrent() ||
                    state->binding.scene != scene.RuntimeId())
                    return Failure<void>("The staged AI group lost its exact owner or Scene publication.");
                return Result<void>::Success();
            }

            void Publish() noexcept override {
                if (published)
                    return;
                // Every vector/index has its final capacity before the aggregate publication fence.
                for (std::size_t removed = 0; removed < removals.size(); ++removed) {
                    const auto index = removals[removed];
                    retired.push_back(std::move(state->agents[index]));
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
                ++state->revision;
                published = true;
            }

            Result<void> AfterPublication() override {
                // Cancellation/blackboard observer teardown is deferred until every owner and Scene is visible.
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
        AiSceneStructuralParticipant(AiSceneRuntime &runtime, const std::span<const AiControllerDescriptor> descriptors)
            : runtime_(&runtime), descriptors_(descriptors.begin(), descriptors.end()) {}

        Runtime::SceneStructuralOwner Owner() const noexcept override {
            return Runtime::SceneStructuralOwner::AI;
        }

        Result<std::unique_ptr<Runtime::SceneStructuralCandidate>> Prepare(const Runtime::RuntimeSceneView scene,
                                                                           const std::span<const Runtime::RuntimeEntityView> created,
                                                                           const std::span<const Runtime::EntityRef> destroyed) override {
            if (runtime_->shutdown_ || !runtime_->active_ || runtime_->active_->closed || !scene.IsCurrent() ||
                runtime_->active_->binding.scene != scene.RuntimeId())
                return Failure<std::unique_ptr<Runtime::SceneStructuralCandidate>>(
                    "AI structural admission requires the active Scene owner.");
            try {
                return PrepareOwned(scene, created, destroyed);
            } catch (const std::bad_alloc &) {
                return Result<std::unique_ptr<Runtime::SceneStructuralCandidate>>::Failure(
                    MakeError(AIErrors::AgentCapacityExceeded, "AI structural preparation could not allocate bounded staging storage."));
            }
        }

    private:
        Result<std::unique_ptr<Runtime::SceneStructuralCandidate>> PrepareOwned(const Runtime::RuntimeSceneView scene,
                                                                                const std::span<const Runtime::RuntimeEntityView> created,
                                                                                const std::span<const Runtime::EntityRef> destroyed) {
            auto candidate = std::make_unique<StructuralCandidate>();
            candidate->state = runtime_->active_;
            candidate->scene = scene;
            candidate->revision = candidate->state->revision;
            if (candidate->revision == std::numeric_limits<std::uint64_t>::max())
                return Failure<std::unique_ptr<Runtime::SceneStructuralCandidate>>("AI structural mutation revision is exhausted.");

            auto &state = *candidate->state;
            for (std::size_t index = 0; index < state.agents.size(); ++index) {
                const auto &agent = state.agents[index];
                if (!agent.retired && std::ranges::find(destroyed, agent.record.owner) != destroyed.end())
                    candidate->removals.push_back(index);
            }
            std::vector<AiSceneAgentDescriptor> agents;
            agents.reserve(created.size());
            for (const auto &entity : created) {
                if (!entity.components || (!entity.components->aiAgent && !entity.components->aiController))
                    continue;
                agents.push_back({.owner = entity.entity,
                                  .agent = entity.components->aiAgent.value_or(AiAgentComponent{}),
                                  .controller = entity.components->aiController});
            }
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
            auto prepared =
                PrepareStructuralAgents(state.binding, agents, descriptors_, runtime_->settings_.availableCapabilities, candidate->slots);
            if (prepared.HasError())
                return Result<std::unique_ptr<Runtime::SceneStructuralCandidate>>::Failure(prepared.ErrorValue());
            candidate->additions = std::move(prepared).Value();
            candidate->retired.reserve(candidate->removals.size());
            candidate->tombstones.reserve(candidate->removals.size());
            for (const auto index : candidate->removals) {
                AgentRuntimeState tombstone;
                tombstone.record.handle = state.agents[index].record.handle;
                tombstone.retired = true;
                candidate->tombstones.push_back(std::move(tombstone));
            }
            candidate->owners.reserve(state.agentsByOwner.size() + agents.size());
            for (const auto &[owner, index] : state.agentsByOwner)
                if (!state.agents[index].retired && std::ranges::find(candidate->removals, index) == candidate->removals.end())
                    candidate->owners.emplace(owner, index);
            for (std::size_t index = 0; index < agents.size(); ++index)
                candidate->owners.emplace(agents[index].owner, candidate->slots[index].index);
            state.agents.reserve(state.agents.size() + appended);
            return Result<std::unique_ptr<Runtime::SceneStructuralCandidate>>::Success(std::move(candidate));
        }

        AiSceneRuntime *runtime_;
        std::vector<AiControllerDescriptor> descriptors_;
    };

    /** @copydoc MakeStructuralParticipant */
    std::unique_ptr<Runtime::SceneStructuralParticipant> MakeStructuralParticipant(
        AiSceneRuntime &runtime, const std::span<const AiControllerDescriptor> descriptors) {
        return std::make_unique<AiSceneStructuralParticipant>(runtime, descriptors);
    }
}  // namespace Horo::AI::Detail
