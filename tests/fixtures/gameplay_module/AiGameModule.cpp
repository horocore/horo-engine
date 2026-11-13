#include "Horo/Gameplay/GameplayAiContext.h"
#include "Horo/Gameplay/NativeDecisionNode.h"
#include "Horo/Gameplay/NativePerceptionSense.h"

#include <memory>
#include <stdexcept>
#include <vector>

namespace {
    using namespace Horo;
    using namespace Horo::Gameplay;

    /** @brief Actual native image that registers inert tasks and retains only its host-issued immutable AI scope. */
    class AiModule final : public IGameModule {
    public:
        Result<void> Register(GameRegistrationContext &context) override {
            if (auto first = context.ai.Register({AI::TaskId::Create(411).Value(), {}, AI::AiTaskAuthoring::Native}); first.HasError())
                return first;
            return context.ai.Register({AI::TaskId::Create(412).Value(), {}, AI::AiTaskAuthoring::Script});
        }

        Result<void> Start(GameRuntimeContext &context) override {
            auto acquired = AcquireGameplayAi(context);
            if (acquired.HasError())
                return Result<void>::Success();
            retained_ = std::move(acquired).Value();
            // Mutable module-facing state cannot replace the host's authoritative revocation holder.
            context.ai.reset();
            return Result<void>::Success();
        }

        Result<GameModuleReloadSnapshot> PrepareReload(GameRuntimeContext &) override {
            const bool revoked = !retained_ || retained_->ResolveAgent(AI::AgentId::Create(1).Value()).HasError();
            return Result<GameModuleReloadSnapshot>::Success({1, {revoked ? std::byte{1} : std::byte{0}}});
        }

        void Stop(GameRuntimeContext &) noexcept override {
            retained_.reset();
        }

    private:
        std::shared_ptr<const GameplayAiContext> retained_;
    };
}  // namespace

namespace {
    std::uint32_t *decisionLifecycleCounters{};

    /** @brief Identifies a deliberate rejection by the unloadable native fixture factory. */
    class FixtureFactoryRejection final : public std::runtime_error {
    public:
        using std::runtime_error::runtime_error;
    };

    /** @brief Real generated decision-node callbacks live in the unloadable fixture image. */
    class FixtureDecisionNode final : public Horo::AI::IBehaviorTreeExecutor {
    public:
        static Horo::AI::DecisionNodeContract DescribeDecisionNode() {
            using namespace Horo::AI;
            DecisionNodeContract contract;
            contract.descriptor.origin = {DecisionDescriptorSourceKind::Native, DecisionProviderId::Create(501).Value(), 1};
            contract.properties = {{BehaviorTreePropertyId::Create(9).Value(), "throw", BehaviorTreePropertyKind::Boolean, false}};
            return contract;
        }

        explicit FixtureDecisionNode(std::span<const Horo::AI::BehaviorTreeProperty> properties) {
            if (std::get<bool>(properties.front().value))
                throw FixtureFactoryRejection("fixture factory rejection");
        }

        Horo::Result<Horo::AI::AiTaskState> Start(const Horo::AI::BehaviorTreeEvaluationContext &,
                                                  const Horo::AI::AiTaskOperationContext &) noexcept override {
            if (decisionLifecycleCounters)
                ++decisionLifecycleCounters[0];
            return Horo::Result<Horo::AI::AiTaskState>::Success(Horo::AI::AiTaskState::Running);
        }

        Horo::Result<Horo::AI::AiTaskState> Resume(const Horo::AI::BehaviorTreeEvaluationContext &,
                                                   const Horo::AI::AiTaskOperationContext &) noexcept override {
            return Horo::Result<Horo::AI::AiTaskState>::Success(Horo::AI::AiTaskState::Succeeded);
        }

        void Abort(const Horo::AI::AiTaskOperationContext &, Horo::AI::AiTaskCancellationReason) noexcept override {
            if (decisionLifecycleCounters)
                ++decisionLifecycleCounters[1];
        }

        void Cleanup(const Horo::AI::AiTaskOperationContext &, const Horo::AI::AiTaskTerminalResult &) noexcept override {
            if (decisionLifecycleCounters)
                ++decisionLifecycleCounters[2];
        }

        Horo::Result<bool> Check(const Horo::AI::BehaviorTreeEvaluationContext &) noexcept override {
            return Horo::Result<bool>::Success(true);
        }

        Horo::Result<void> Service(const Horo::AI::BehaviorTreeEvaluationContext &) noexcept override {
            return Horo::Result<void>::Success();
        }
    };
}  // namespace
HORO_AI_NODE(FixtureDecisionNode, "101")

namespace {
    std::uint32_t *perceptionLifecycleCounters{};
    bool perceptionMetadataFailure{};

    /** @brief Real module-owned bounded sense used to observe construction, invocation and destruction across retirement. */
    class FixturePerceptionSense final : public Horo::AI::IPerceptionSenseProvider {
    public:
        static Horo::AI::PerceptionSenseContract DescribePerceptionSense() {
            using namespace Horo::AI;
            if (perceptionMetadataFailure)
                throw std::runtime_error("fixture sense metadata failure");
            PerceptionSenseContract contract;
            contract.sense.origin = {PerceptionDescriptorSourceKind::Native, PerceptionProviderId::Create(601).Value(), 1};
            contract.sense.displayName = "fixture spatial sense";
            contract.sense.requiredCapabilities = PerceptionCapabilitySet::Of(PerceptionCapability::SpatialCandidateQuery);
            contract.stimulus = {StimulusTypeId::Create(602).Value(), contract.sense.origin, "fixture spatial observation", 1, {}};
            contract.workUnits = 4;
            contract.maximumCandidates = 2;
            return contract;
        }

        FixturePerceptionSense() {
            if (perceptionLifecycleCounters)
                ++perceptionLifecycleCounters[0];
        }

        ~FixturePerceptionSense() override {
            if (perceptionLifecycleCounters)
                ++perceptionLifecycleCounters[2];
        }

        /** @copydoc Horo::AI::IPerceptionSenseProvider::Evaluate */
        Horo::Result<std::size_t> Evaluate(const Horo::AI::PerceptionSenseInput &input,
                                           const std::span<Horo::AI::PerceptionSenseSample> output,
                                           Horo::AI::AiWorkBudget &budget) override {
            if (perceptionLifecycleCounters)
                ++perceptionLifecycleCounters[1];
            for (std::size_t index = 0; index < input.candidates.size(); ++index) {
                if (!budget.Consume())
                    return Horo::Result<std::size_t>::Failure(Horo::MakeError(Horo::AI::AIErrors::SchedulerBudgetExhausted));
                output[index] = {static_cast<std::uint32_t>(index), input.candidates[index].position.Millimeters()[0] < 1500};
            }
            return Horo::Result<std::size_t>::Success(input.candidates.size());
        }
    };
}  // namespace
HORO_AI_SENSE(FixturePerceptionSense, "102")

/** @brief Installs caller-owned test counters before descriptor capture or native provider invocation. */
extern "C" HORO_GAME_EXPORT void HoroTestObservePerceptionSenseLifecycle(std::uint32_t *counters, const bool failMetadata) noexcept {
    perceptionLifecycleCounters = counters;
    perceptionMetadataFailure = failMetadata;
}

extern "C" HORO_GAME_EXPORT void HoroTestObserveDecisionNodeLifecycle(std::uint32_t *counters) noexcept {
    decisionLifecycleCounters = counters;
}

extern "C" HORO_GAME_EXPORT Horo::Gameplay::IGameModule *CreateGameModule() noexcept {
    if (perceptionLifecycleCounters)
        ++perceptionLifecycleCounters[3];
    return std::make_unique<AiModule>().release();
}

extern "C" HORO_GAME_EXPORT void DestroyGameModule(Horo::Gameplay::IGameModule *instance) noexcept {
    const std::unique_ptr<Horo::Gameplay::IGameModule> owned{instance};
}

/** @brief Exercises the module-emitted frontend while its diagnostic allocator remains host-issued. */
extern "C" HORO_GAME_EXPORT bool HoroTestStartAiTaskWithDiagnostics(const std::shared_ptr<const Horo::Gameplay::GameplayAiContext> &context,
                                                                    Horo::AI::AgentHandle agent, bool isolated, bool includeFields,
                                                                    Horo::AI::TaskHandle &task,
                                                                    std::shared_ptr<const Horo::Telemetry::OperationContext> &captured) {
    Horo::Telemetry::OperationContext diagnostics;
    diagnostics.operationId = 741;
    diagnostics.parentOperationId = 740;
    std::vector<Horo::Log::MdcField> fields;
    if (includeFields)
        fields.emplace_back("module.source", "native-ai-image");
    diagnostics.diagnosticContext =
        isolated ? Horo::Log::LogContextSnapshot::Isolated(std::move(fields)) : Horo::Log::LogContextSnapshot{std::move(fields)};
    auto started = context->StartTask(agent, Horo::AI::TaskId::Create(411).Value(), diagnostics);
    if (started.HasError())
        return false;
    auto snapshot = context->InspectTask(agent, started.Value());
    if (snapshot.HasError() || !snapshot.Value().diagnostics)
        return false;
    task = started.Value();
    captured = snapshot.Value().diagnostics;
    return true;
}
