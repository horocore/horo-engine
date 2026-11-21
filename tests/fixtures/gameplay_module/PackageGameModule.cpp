#include "Horo/Gameplay/GameEventRegistry.h"
#include "Horo/Gameplay/GameModule.h"
#include "Horo/Gameplay/GameServiceRegistry.h"
#include "Horo/Gameplay/NativeDecisionNode.h"
#include "Horo/Gameplay/NativePerceptionSense.h"

#include <memory>

namespace {
    using namespace Horo;
    using namespace Horo::Gameplay;

    /** @brief Actual native service whose destruction remains owned by its module image. */
    class PackageService final : public IGameplayService {
    public:
        Result<void> Start(const GameplayServiceContext &) override {
            return Result<void>::Success();
        }

        void Stop(const GameplayServiceContext &) noexcept override {
            // This stateless service has no work or resources to retire.
        }
    };

    /** @brief Minimal real SDK module for package activation, callback pinning and native retirement tests. */
    class PackageModule final : public IGameModule {
    public:
        Result<void> Register(GameRegistrationContext &context) override {
            if (auto registered = context.events.Register({90, 1, 91, 1, BorrowedCallbackContext{this}, &Invoke}); registered.HasError())
                return registered;
            return context.services.Register({
                .descriptor = {.id = GameplayServiceId::Parse("game.tests.package_service").Value(),
                               .scope = GameplayServiceScope::Project,
                               .affinity = GameplayThreadAffinity::RuntimeOwner,
                               .sceneReplacement = GameplaySceneReplacementPolicy::Preserve,
                               .observabilityCategory = "game.tests.packages"},
                .factory = {.create = &CreateService, .destroy = &DestroyService},
            });
        }

        Result<void> Start(GameRuntimeContext &) override {
            return Result<void>::Success();
        }

        void Stop(GameRuntimeContext &) noexcept override {
            // Registrations and service retirement remain owned by the native module host.
        }

    private:
        /** @brief Keeps an actual module-resident callback behind the host's generation lease. */
        static GameplayEventOutcome Invoke(const BorrowedCallbackContext &, const GameplayEventRequest &) {
            return GameplayEventOutcome::Accepted;
        }

        /** @brief Constructs the package's only module-owned service. */
        static IGameplayService *CreateService(void *) {  // NOSONAR(cpp:S5008) The SDK factory ABI requires an opaque, unused context.
            return std::make_unique<PackageService>().release();
        }

        /** @brief Destroys the service while its owning native image is still retained. */
        static void DestroyService(
            void *, IGameplayService *service) noexcept {  // NOSONAR(cpp:S5008) The SDK factory ABI requires an opaque context.
            const std::unique_ptr<IGameplayService> owned{service};
        }
    };
}  // namespace

namespace {
    class PackageDecisionNode final : public Horo::AI::IBehaviorTreeExecutor {
    public:
        static Horo::AI::DecisionNodeContract DescribeDecisionNode() {
            using namespace Horo::AI;
            DecisionNodeContract contract;
            contract.descriptor.origin = {DecisionDescriptorSourceKind::Package, DecisionProviderId::Create(701).Value(), 1};
            contract.role = DecisionNodeRole::Condition;
            return contract;
        }

        explicit PackageDecisionNode(std::span<const Horo::AI::BehaviorTreeProperty>) {}

        Horo::Result<Horo::AI::AiTaskState> Start(const Horo::AI::BehaviorTreeEvaluationContext &,
                                                  const Horo::AI::AiTaskOperationContext &) noexcept override {
            return Horo::Result<Horo::AI::AiTaskState>::Success(Horo::AI::AiTaskState::Succeeded);
        }

        Horo::Result<Horo::AI::AiTaskState> Resume(const Horo::AI::BehaviorTreeEvaluationContext &,
                                                   const Horo::AI::AiTaskOperationContext &) noexcept override {
            return Horo::Result<Horo::AI::AiTaskState>::Success(Horo::AI::AiTaskState::Succeeded);
        }

        void Abort(const Horo::AI::AiTaskOperationContext &, Horo::AI::AiTaskCancellationReason) noexcept override {}

        void Cleanup(const Horo::AI::AiTaskOperationContext &, const Horo::AI::AiTaskTerminalResult &) noexcept override {}

        Horo::Result<bool> Check(const Horo::AI::BehaviorTreeEvaluationContext &) noexcept override {
            return Horo::Result<bool>::Success(true);
        }

        Horo::Result<void> Service(const Horo::AI::BehaviorTreeEvaluationContext &) noexcept override {
            return Horo::Result<void>::Success();
        }
    };
}  // namespace
HORO_AI_NODE(PackageDecisionNode, "201")

namespace {
    /** @brief Native custom sense carrying actual package provenance from the verified package role. */
    class PackagePerceptionSense final : public Horo::AI::IPerceptionSenseProvider {
    public:
        static Horo::AI::PerceptionSenseContract DescribePerceptionSense() {
            using namespace Horo::AI;
            PerceptionSenseContract contract;
            contract.sense.origin = {PerceptionDescriptorSourceKind::Package, PerceptionProviderId::Create(701).Value(), 1};
            contract.sense.displayName = "package spatial sense";
            contract.sense.requiredCapabilities = PerceptionCapabilitySet::Of(PerceptionCapability::SpatialCandidateQuery);
            contract.stimulus = {StimulusTypeId::Create(702).Value(), contract.sense.origin, "package spatial observation", 1, {}};
            contract.workUnits = 4;
            contract.maximumCandidates = 2;
            return contract;
        }

        /** @copydoc Horo::AI::IPerceptionSenseProvider::Evaluate */
        Horo::Result<std::size_t> Evaluate(const Horo::AI::PerceptionSenseInput &input,
                                           const std::span<Horo::AI::PerceptionSenseSample> output,
                                           Horo::AI::AiWorkBudget &budget) override {
            for (std::size_t index = 0; index < input.candidates.size(); ++index) {
                if (!budget.Consume())
                    return Horo::Result<std::size_t>::Failure(Horo::MakeError(Horo::AI::AIErrors::SchedulerBudgetExhausted));
                output[index] = {static_cast<std::uint32_t>(index), true};
            }
            return Horo::Result<std::size_t>::Success(input.candidates.size());
        }
    };
}  // namespace
HORO_AI_SENSE(PackagePerceptionSense, "202")

/** @brief Constructs the real package fixture module through the supported native SDK boundary. */
extern "C" HORO_GAME_EXPORT Horo::Gameplay::IGameModule *CreateGameModule() noexcept {
    return std::make_unique<PackageModule>().release();
}

/** @brief Releases the package fixture module before its native image is unloaded. */
extern "C" HORO_GAME_EXPORT void DestroyGameModule(Horo::Gameplay::IGameModule *gameModule) noexcept {
    const std::unique_ptr<Horo::Gameplay::IGameModule> owned{gameModule};
}
