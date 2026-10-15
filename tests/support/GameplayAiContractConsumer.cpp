#include "Horo/Gameplay/GameModuleHost.h"
#include "Horo/Gameplay/GameplayAiScriptBridge.h"

#include <type_traits>

static_assert(Horo::Gameplay::GameplaySdkBoundaryVersion == 20);
static_assert(std::is_same_v<decltype(Horo::Gameplay::GameRuntimeContext::ai), std::shared_ptr<const Horo::Gameplay::GameplayAiContext>>);
static_assert(std::is_same_v<decltype(Horo::Gameplay::GameRuntimeContext::character),
                             std::shared_ptr<const Horo::Gameplay::GameplayCharacterContext>>);
static_assert(std::is_same_v<decltype(Horo::Gameplay::GameRuntimeContext::navigation),
                             std::shared_ptr<const Horo::Gameplay::GameplayNavigationContext>>);
static_assert(
    std::is_same_v<decltype(Horo::Gameplay::GameRuntimeContext::physics), std::shared_ptr<const Horo::Gameplay::GameplayPhysicsContext>>);

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
