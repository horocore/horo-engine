#include "Horo/Gameplay/GameModuleHost.h"
#include "Horo/Gameplay/GameplayAiScriptBridge.h"

int main() {
    Horo::Gameplay::GameRuntimeContext context;
    const auto missing = Horo::Gameplay::AcquireGameplayAi(context);
    if (!missing.HasError() || missing.ErrorValue().code.Value() != Horo::AI::AIErrors::CapabilityUnavailable.code.Value())
        return 1;
    Horo::AI::AiCapabilityDescriptorRegistry registry;
    const auto task = Horo::AI::TaskId::Create(1);
    if (task.HasError() || registry.Register({task.Value()}).HasError())
        return 2;
    const auto frozen = registry.Freeze();
    if (!frozen.Find(task.Value()))
        return 3;
    Horo::Gameplay::GameModuleHost legacy({});
    Horo::Gameplay::GameModuleHost typed{Horo::Gameplay::GameModuleHostOptions{{}, {}}};
    const auto descriptor = Horo::Gameplay::MakeGameplayAiScriptDescriptor("game.consumer", {"game.ai"});
    return descriptor.functions.size() == 8 ? 0 : 4;
}
