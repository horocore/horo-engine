#include "Horo/Gameplay/GameplayAiContext.h"

#include <memory>

namespace {
    using namespace Horo;
    using namespace Horo::Gameplay;

    /** @brief Actual native image that registers inert tasks and retains only its host-issued immutable AI scope. */
    class AiModule final : public IGameModule {
    public:
        Result<void> Register(GameRegistrationContext &context) override {
            auto first = context.ai.Register({AI::TaskId::Create(411).Value(), {}, AI::AiTaskAuthoring::Native});
            if (first.HasError())
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

extern "C" HORO_GAME_EXPORT Horo::Gameplay::IGameModule *CreateGameModule() noexcept {
    return std::make_unique<AiModule>().release();
}

extern "C" HORO_GAME_EXPORT void DestroyGameModule(Horo::Gameplay::IGameModule *module) noexcept {
    const std::unique_ptr<Horo::Gameplay::IGameModule> owned{module};
}

/** @brief Exercises the module-emitted frontend while its diagnostic allocator remains host-issued. */
extern "C" HORO_GAME_EXPORT bool HoroTestStartAiTaskWithDiagnostics(const std::shared_ptr<const Horo::Gameplay::GameplayAiContext> &context,
                                                                    Horo::AI::AgentHandle agent, Horo::AI::TaskHandle &task,
                                                                    std::shared_ptr<const Horo::Telemetry::OperationContext> &captured) {
    Horo::Telemetry::OperationContext diagnostics;
    diagnostics.operationId = 741;
    diagnostics.parentOperationId = 740;
    diagnostics.diagnosticContext = Horo::Log::LogContextSnapshot{{{"module.source", "native-ai-image"}}};
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
