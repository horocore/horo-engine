#include "AISceneRuntimeDetail.h"

#include <algorithm>
#include <new>

namespace Horo::AI {
    namespace {
        /** @brief Retains the typed diagnostic identity without copying author or provider text. */
        AiInspectionFailure FailureIdentity(const Error &failure) {
            return {failure.domain, failure.code};
        }

        /** @brief Reads only keys the host explicitly admitted, honoring snapshot permission checks. */
        std::vector<AiInspectionBlackboardRow> InspectBlackboard(const BlackboardInstance &instance, const BlackboardSchema &schema,
                                                                 std::span<const BlackboardKeyId> disclosedKeys) {
            std::vector<AiInspectionBlackboardRow> rows;
            rows.reserve(schema.Keys().size());
            for (const auto &key : schema.Keys()) {
                AiInspectionBlackboardRow row{key.key, key.kind, key.cardinality, AiInspectionValueStatus::Redacted, {}};
                if (std::ranges::find(disclosedKeys, key.key) != disclosedKeys.end()) {
                    auto read = instance.ReadAtSafePoint(key.key);
                    if (read.HasError())
                        row.status = AiInspectionValueStatus::Unavailable;
                    else if (!read.Value())
                        row.status = AiInspectionValueStatus::Unset;
                    else if (!std::holds_alternative<BlackboardOpaqueValue>(*read.Value())) {
                        row.status = AiInspectionValueStatus::Present;
                        row.value = std::move(read).Value();
                    }
                }
                rows.push_back(std::move(row));
            }
            return rows;
        }

        /** @brief Projects the current directly owned task without exporting its native context. */
        std::optional<AiInspectionTask> InspectTask(const AiTaskLifecycle *task) {
            if (!task || !task->Context())
                return std::nullopt;
            const auto &context = *task->Context();
            AiInspectionTask row{context.taskDefinition, context.task, task->State(), {}};
            if (const auto *terminal = task->TerminalResult(); terminal && terminal->failure)
                row.failure = FailureIdentity(terminal->failure->cause);
            return row;
        }

        /** @brief Reads only failures explicitly attributed to the selected agent generation. */
        std::optional<AiInspectionFailure> InspectDecision(const AiSchedulingReport *report, const AgentHandle agent) {
            if (!report)
                return std::nullopt;
            if (report->firstFailure && report->firstFailureOrigin && report->firstFailureOrigin->agent == agent)
                return FailureIdentity(*report->firstFailure);
            if (report->firstAdvisoryFailure && report->firstAdvisoryFailure->origin.agent == agent)
                return FailureIdentity(report->firstAdvisoryFailure->error);
            return std::nullopt;
        }

        /** @brief Retains only the immutable identity and age of memory entries. */
        void InspectMemory(AiAgentInspectionSnapshot &result, const PerceptionMemorySnapshot &memory) {
            result.stimuli.reserve(memory.count);
            for (const auto &entry : std::span{memory.entries}.first(memory.count))
                result.stimuli.push_back(
                    {entry.key, entry.firstSensedTick, entry.lastSensedTick, entry.ageSeconds, entry.isCurrentlySensed});
        }

        /** @brief Copies contracts from instantiated senses without invoking provider code. */
        void InspectSenseProviders(AiAgentInspectionSnapshot &result, const Detail::SceneAgentPerception &sensing) {
            result.senseProviders.reserve(sensing.senses.size());
            for (const auto &listener : sensing.senses) {
                const auto &instance = *listener.instance;
                const auto &contract = instance.Contract();
                result.senseProviders.push_back({contract.sense.identity, contract.sense.origin,
                                                 contract.sense.requiredCapabilities.Union(contract.stimulus.requiredCapabilities),
                                                 !sensing.suspension && instance.CanEvaluate()});
            }
        }
    }  // namespace

    /** @copydoc AiSceneRuntime::CaptureAgentInspectionAtSafePoint */
    Result<AiAgentInspectionSnapshot> AiSceneRuntime::CaptureAgentInspectionAtSafePoint(
        const Runtime::RuntimeSceneView &scene, const AgentHandle agent, const std::span<const BlackboardKeyId> disclosedKeys) {
        using Snapshot = Result<AiAgentInspectionSnapshot>;
        if (auto ready = CapabilityAtSafePoint().ValidateAtSafePoint(); ready.HasError())
            return Snapshot::Failure(ready.ErrorValue());
        if (disclosedKeys.size() > MaximumBlackboardKeys)
            return Snapshot::Failure(MakeError(AIErrors::BlackboardSchemaInvalid));
        const auto *selected = Detail::FindAgent(*active_, agent);
        if (!selected || selected->record.state != AiAgentActivationState::Active || !selected->blackboard || !scene.IsCurrent() ||
            scene.RuntimeId() != active_->binding.scene || !scene.Get(selected->record.owner).HasValue())
            return Snapshot::Failure(MakeError(AIErrors::HandleInvalid));
        try {
            const auto &board = *selected->blackboard;
            const auto schema = board.SchemaAtSafePoint();
            if (!schema)
                return Snapshot::Failure(MakeError(AIErrors::HandleInvalid));
            const auto *report = active_->scheduler ? active_->scheduler->CompletedReportAtSafePoint() : nullptr;
            AiAgentInspectionSnapshot result{.agent = agent,
                                             .owner = selected->record.owner,
                                             .sceneRevision = scene.StructuralRevision(),
                                             .decisionTick = report ? report->tick : 0,
                                             .blackboard = board.Binding(),
                                             .blackboardRevision = board.RevisionAtSafePoint(),
                                             .keys = InspectBlackboard(board, *schema, disclosedKeys),
                                             .task = InspectTask(selected->task.get()),
                                             .decisionFailure = InspectDecision(report, agent)};
            if (report && selected->cookedPlan) {
                std::array<DecisionActiveTask, AiAgentInspectionSnapshot::MaximumDecisionTasks> tasks{};
                const auto total = CopyCookedDecisionTasks(agent, selected->cookedPlan, tasks);
                const auto retained = std::span{tasks}.first(std::min(total, tasks.size()));
                result.decisionTasks.assign(retained.begin(), retained.end());
                result.omittedDecisionTasks = total - retained.size();
            }
            if (selected->sensing) {
                InspectSenseProviders(result, *selected->sensing);
                const auto memory = ReadPerceptionAtSafePoint(scene, result.owner);
                if (memory.HasError())
                    result.perceptionFailure = FailureIdentity(memory.ErrorValue());
                else
                    InspectMemory(result, memory.Value());
            }
            return Snapshot::Success(std::move(result));
        } catch (const std::bad_alloc &) {
            return Snapshot::Failure(MakeError(AIErrors::DecisionAssetStorageUnavailable));
        }
    }
}  // namespace Horo::AI
