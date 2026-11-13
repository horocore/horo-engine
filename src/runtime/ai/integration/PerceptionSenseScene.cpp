#include "Horo/AI/PerceptionSenseScene.h"

#include "Horo/AI/AIErrors.h"
#include "Horo/AI/AIScenePerceptionSource.h"

#include <algorithm>

namespace Horo::AI {
    namespace {
        /** @brief Checks immutable recipient and declared query bounds before spending work. */
        bool MatchesBinding(const PerceptionSenseBinding &binding, const PerceptionSenseContract &contract,
                            const PerceptionSpatialQuery &query, const AIPerceptionMemory &memory) noexcept {
            return ProjectPerceptionSource(query.listener) == binding.listener && query.sense == contract.sense.identity &&
                   query.maximumCandidates > 0 && query.maximumCandidates <= contract.maximumCandidates &&
                   memory.Agent() == binding.agent && memory.SceneIncarnation() == binding.listener.sceneIncarnation;
        }

        /** @brief Reserves query entry only when the complete subsequent provider allowance fits. */
        bool ReserveQuery(const PerceptionSenseContract &contract, AiWorkBudget &agent, AiWorkBudget &global) noexcept {
            const auto required = contract.workUnits + PerceptionSenseGeneration::ValidationWorkUnits + 1;
            if (std::min(agent.Remaining(), global.Remaining()) < required)
                return false;
            static_cast<void>(agent.Consume());
            if (&agent != &global)
                static_cast<void>(global.Consume());
            return true;
        }

        /** @brief Checks the actual publication and exact listener before exposing or staging facts. */
        bool Current(const Runtime::RuntimeSceneView &scene, const PerceptionSpatialBroadphase &spatial,
                     const std::shared_ptr<const PerceptionSpatialSnapshot> &snapshot, const Runtime::EntityRef listener) {
            return scene.IsCurrent() && snapshot && snapshot == spatial.Current() && snapshot->ReadAuthorization()->IsCurrent() &&
                   snapshot->Scene() == scene.RuntimeId() && listener.runtime == scene.RuntimeId() && scene.Get(listener).HasValue();
        }

        /** @brief Admits the provider's owner lane before touching the live Scene/memory/spatial owner. */
        Result<std::shared_ptr<const PerceptionSpatialSnapshot>> AdmitScene(const Runtime::RuntimeSceneView &scene,
                                                                            const PerceptionSpatialBroadphase &spatial,
                                                                            const PerceptionSenseInstance &instance,
                                                                            const PerceptionSpatialQuery &query,
                                                                            const AIPerceptionMemory &memory) {
            using Output = Result<std::shared_ptr<const PerceptionSpatialSnapshot>>;
            if (!instance.CanEvaluate())
                return Output::Failure(MakeError(AIErrors::RuntimeUnavailable));
            if (!MatchesBinding(instance.Binding(), instance.Contract(), query, memory))
                return Output::Failure(MakeError(AIErrors::PerceptionSpatialInvalid));
            const auto snapshot = spatial.Current();
            if (!Current(scene, spatial, snapshot, query.listener))
                return Output::Failure(MakeError(AIErrors::PerceptionSpatialStale));
            return Output::Success(snapshot);
        }

        /** @brief Validates every gathered source before a callback and again before memory publication. */
        bool CurrentCandidates(const Runtime::RuntimeSceneView &scene, const PerceptionSpatialSnapshot &snapshot,
                               const PerceptionSpatialQuery &query, const PerceptionSpatialResult &candidates) {
            for (std::size_t index = 0; index < candidates.count; ++index) {
                const auto source = candidates.candidates[index].entity;
                if (scene.Get(source).HasError() || !snapshot.Allows(query.listener, source, query.sense))
                    return false;
            }
            return true;
        }

        /** @brief Revalidates the complete sensing window after arbitrary provider code returns. */
        bool CurrentWindow(const Runtime::RuntimeSceneView &scene, const PerceptionSpatialBroadphase &spatial,
                           const std::shared_ptr<const PerceptionSpatialSnapshot> &snapshot, const PerceptionSpatialQuery &query,
                           const PerceptionSpatialResult &candidates) {
            return Current(scene, spatial, snapshot, query.listener) && CurrentCandidates(scene, *snapshot, query, candidates);
        }

        /** @brief Gathers only live, policy-admitted facts from the exact retained publication. */
        Result<PerceptionSpatialResult> Gather(const Runtime::RuntimeSceneView &scene, const PerceptionSpatialSnapshot &snapshot,
                                               const PerceptionSpatialQuery &query) {
            auto gathered = snapshot.Query(query);
            if (gathered.HasError())
                return gathered;
            if (!CurrentCandidates(scene, snapshot, query, gathered.Value()))
                return Result<PerceptionSpatialResult>::Failure(MakeError(AIErrors::PerceptionSpatialStale));
            return gathered;
        }

        /** @brief Applies validated binary outcomes to a private memory candidate, preserving atomic failure. */
        Result<void> Stage(AIPerceptionMemory &memory, const PerceptionSenseBinding &binding, const PerceptionSenseContract &contract,
                           const PerceptionSenseInput &input, const PerceptionSenseResult &result) {
            auto candidate = memory;
            if (const auto advanced = candidate.AdvanceTo(input.tick); advanced.HasError())
                return advanced;
            for (std::size_t index = 0; index < result.count; ++index) {
                const auto &sample = result.samples[index];
                const auto &source = input.candidates[sample.candidate];
                const PerceptionMemoryKey key{binding.listenerType, contract.sense.identity, contract.stimulus.identity, source.source};
                const auto staged = sample.sensed ? candidate.Observe({.key = key, .position = source.position}, input.tick)
                                                  : candidate.MarkLost(key, input.tick);
                if (staged.HasError())
                    return staged;
            }
            memory = std::move(candidate);
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc RunPerceptionSenseAtSensePoll */
    Result<PerceptionSenseSceneResult> RunPerceptionSenseAtSensePoll(const Runtime::RuntimeSceneView &scene,
                                                                     const PerceptionSpatialBroadphase &spatial,
                                                                     PerceptionSenseInstance &instance, const PerceptionSpatialQuery &query,
                                                                     const std::uint64_t tick, AIPerceptionMemory &memory,
                                                                     AiWorkBudget &agentBudget, AiWorkBudget &globalBudget) {
        const auto admitted = AdmitScene(scene, spatial, instance, query, memory);
        if (admitted.HasError())
            return Result<PerceptionSenseSceneResult>::Failure(admitted.ErrorValue());
        const auto &binding = instance.Binding();
        const auto &contract = instance.Contract();
        const auto &snapshot = admitted.Value();
        if (!ReserveQuery(contract, agentBudget, globalBudget))
            return Result<PerceptionSenseSceneResult>::Failure(MakeError(AIErrors::SchedulerBudgetExhausted));
        const auto gathered = Gather(scene, *snapshot, query);
        if (gathered.HasError())
            return Result<PerceptionSenseSceneResult>::Failure(gathered.ErrorValue());
        const auto position = snapshot->ListenerPosition(query.listener);
        if (position.HasError())
            return Result<PerceptionSenseSceneResult>::Failure(position.ErrorValue());
        std::array<PerceptionSenseCandidate, PerceptionSenseResult::MaximumCandidates> facts{};
        for (std::size_t index = 0; index < gathered.Value().count; ++index) {
            const auto &source = gathered.Value().candidates[index];
            facts[index] = {ProjectPerceptionSource(source.entity), source.position};
        }
        const PerceptionSenseInput input{binding.agent,
                                         binding.listener,
                                         position.Value(),
                                         tick,
                                         snapshot->Revision(),
                                         std::span{facts}.first(gathered.Value().count),
                                         {}};
        const auto result = instance.Evaluate(input, agentBudget, globalBudget);
        if (result.HasError())
            return Result<PerceptionSenseSceneResult>::Failure(result.ErrorValue());
        if (!CurrentWindow(scene, spatial, snapshot, query, gathered.Value()))
            return Result<PerceptionSenseSceneResult>::Failure(MakeError(AIErrors::PerceptionSpatialStale));
        if (const auto staged = Stage(memory, binding, contract, input, result.Value()); staged.HasError())
            return Result<PerceptionSenseSceneResult>::Failure(staged.ErrorValue());
        return Result<PerceptionSenseSceneResult>::Success({gathered.Value().count, result.Value().count, gathered.Value().truncated});
    }
}  // namespace Horo::AI
