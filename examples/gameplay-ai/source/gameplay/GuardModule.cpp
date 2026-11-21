#include "Horo/Gameplay/GameModule.h"
#include "Horo/Gameplay/NativeDecisionNode.h"

#include <algorithm>
#include <limits>
#include <new>
#include <stdexcept>

namespace {
    using namespace Horo;
    using namespace Horo::AI;
    using namespace Horo::Gameplay;

    constexpr std::uint64_t GuardProvider = 83001;
    constexpr std::uint64_t HoldTicksProperty = 1;
    constexpr std::int64_t DefaultHoldTicks = 30;
    constexpr std::int64_t MaximumHoldTicks = 600;

    /** @brief Waits at a reached patrol waypoint using the committed simulation clock. */
    class HoldWaypoint final : public IBehaviorTreeExecutor {
    public:
        static DecisionNodeContract DescribeDecisionNode() {
            DecisionNodeContract contract;
            contract.descriptor.origin = {DecisionDescriptorSourceKind::Native, DecisionProviderId::Create(GuardProvider).Value(), 1};
            contract.properties.emplace_back(BehaviorTreePropertyId::Create(HoldTicksProperty).Value(), "Hold ticks",
                                             BehaviorTreePropertyKind::SignedInteger, DefaultHoldTicks);
            return contract;
        }

        explicit HoldWaypoint(const std::span<const BehaviorTreeProperty> properties) {
            const auto id = BehaviorTreePropertyId::Create(HoldTicksProperty).Value();
            const auto configured = std::ranges::find(properties, id, &BehaviorTreeProperty::id);
            if (configured == properties.end())
                return;
            const auto *ticks = std::get_if<std::int64_t>(&configured->value);
            if (!ticks || *ticks < 1 || *ticks > MaximumHoldTicks)
                throw std::invalid_argument("Hold ticks must be an integer between 1 and 600.");
            duration_ = static_cast<std::uint64_t>(*ticks);
        }

        Result<AiTaskState> Start(const BehaviorTreeEvaluationContext &context, const AiTaskOperationContext &) noexcept override {
            if (context.tick > std::numeric_limits<std::uint64_t>::max() - duration_)
                return Result<AiTaskState>::Failure(MakeError(AIErrors::TaskContextInvalid));
            endTick_ = context.tick + duration_;
            return Result<AiTaskState>::Success(AiTaskState::Running);
        }

        Result<AiTaskState> Resume(const BehaviorTreeEvaluationContext &context, const AiTaskOperationContext &) noexcept override {
            return Result<AiTaskState>::Success(context.tick >= endTick_ ? AiTaskState::Succeeded : AiTaskState::Running);
        }

        void Abort(const AiTaskOperationContext &, AiTaskCancellationReason) noexcept override {}

        void Cleanup(const AiTaskOperationContext &, const AiTaskTerminalResult &) noexcept override {
            endTick_ = 0;
        }

        Result<bool> Check(const BehaviorTreeEvaluationContext &) noexcept override {
            return Result<bool>::Failure(MakeError(AIErrors::TaskContextInvalid));
        }

        Result<void> Service(const BehaviorTreeEvaluationContext &) noexcept override {
            return Result<void>::Failure(MakeError(AIErrors::TaskContextInvalid));
        }

    private:
        std::uint64_t duration_{DefaultHoldTicks};
        std::uint64_t endTick_{};
    };

    /** @brief The host discovers the native task from the generated descriptor bundle. */
    class GuardModule final : public IGameModule {
    public:
        Result<void> Register(GameRegistrationContext &) override {
            return Result<void>::Success();
        }

        Result<void> Start(GameRuntimeContext &) override {
            return Result<void>::Success();
        }

        void Stop(GameRuntimeContext &) noexcept override {}
    };
}  // namespace

HORO_AI_NODE(HoldWaypoint, "83002")

extern "C" HORO_GAME_EXPORT Horo::Gameplay::IGameModule *CreateGameModule() noexcept {
    return new (std::nothrow) GuardModule;
}

extern "C" HORO_GAME_EXPORT void DestroyGameModule(Horo::Gameplay::IGameModule *instance) noexcept {
    delete instance;
}
