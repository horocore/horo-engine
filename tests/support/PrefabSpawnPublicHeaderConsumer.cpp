#include "Horo/Gameplay/BehaviorRuntime.h"
#include "Horo/Prefab/PrefabSpawnService.h"

#include <concepts>
#include <type_traits>

static_assert(!std::is_copy_constructible_v<Horo::Prefab::PrefabSpawnService>);
static_assert(std::is_copy_constructible_v<Horo::Prefab::PrefabOperation>);
static_assert(std::is_copy_constructible_v<Horo::Prefab::PrefabInstance>);
static_assert(!std::is_same_v<Horo::Prefab::PrefabInitializationId, Horo::Prefab::PrefabPropertyId>);
static_assert(!std::is_same_v<Horo::Prefab::PrefabInitializationId, Horo::Prefab::CookedPrefabEntitySlot>);
static_assert(!std::is_convertible_v<Horo::Prefab::PrefabPropertyId, Horo::Prefab::PrefabInitializationId>);

static_assert(!std::is_constructible_v<Horo::Gameplay::GameplayPrefabContext, std::shared_ptr<Horo::Prefab::Detail::PrefabSpawnState>,
                                       Horo::Prefab::GameplayPrefabBinding, std::uint64_t>);
using AcquireMethod = Horo::Result<std::shared_ptr<Horo::Gameplay::GameplayPrefabContext>> (Horo::Prefab::PrefabSpawnService::*)(
    Horo::Prefab::GameplayPrefabBinding) const;
using AdvanceMethod = Horo::Result<void> (Horo::Prefab::PrefabSpawnService::*)(std::uint64_t) const;
static_assert(std::is_same_v<decltype(&Horo::Prefab::PrefabSpawnService::Acquire), AcquireMethod>);
static_assert(std::is_same_v<decltype(&Horo::Prefab::PrefabSpawnService::Advance), AdvanceMethod>);

namespace {
    using namespace Horo;
    using namespace Horo::Gameplay;
    using RuntimeResult = Result<std::unique_ptr<BehaviorRuntime>>;

    template <typename Owner>
    concept AcceptsRuntimeBindings =
        requires(Owner &owner, const BehaviorRegistry &registry, BehaviorRuntimeLimits limits, BehaviorRuntimeServiceBindings services) {
            { BehaviorRuntime::Create(owner, registry, limits, services) } -> std::same_as<RuntimeResult>;
        };

    template <typename Owner>
    concept AcceptsPrefabScope = requires(Owner &owner, const BehaviorRegistry &registry, BehaviorRuntimeLimits limits,
                                          BehaviorRuntimeServiceBindings services, std::shared_ptr<const GameplayPrefabContext> prefabs) {
        { BehaviorRuntime::Create(owner, registry, limits, services, prefabs) } -> std::same_as<RuntimeResult>;
    };

    template <typename Owner>
    concept AcceptsLegacyRuntimeArguments =
        requires(Owner &owner, const BehaviorRegistry &registry, BehaviorRuntimeLimits limits,
                 std::shared_ptr<const GameplayPhysicsContext> physics, std::shared_ptr<const GameplayNavigationContext> navigation,
                 std::shared_ptr<const GameplayCharacterContext> character, std::shared_ptr<const Runtime::Ui::RuntimeUiClient> runtimeUi) {
            BehaviorRuntime::Create(owner, registry, limits, physics, navigation, character, runtimeUi);
        };

    template <typename Owner>
    concept AcceptsLegacySceneArguments =
        requires(Owner &owner, const BehaviorRegistry &registry, BehaviorRuntimeLimits limits,
                 std::shared_ptr<const GameplayPhysicsContext> physics, std::shared_ptr<const GameplayPrefabContext> prefabs,
                 std::shared_ptr<const GameplayNavigationContext> navigation, std::shared_ptr<const GameplayCharacterContext> character,
                 std::shared_ptr<const Runtime::Ui::RuntimeUiClient> runtimeUi) {
            BehaviorRuntime::Create(owner, registry, limits, physics, prefabs, navigation, character, runtimeUi);
        };

    static_assert(std::is_aggregate_v<BehaviorRuntimeServiceBindings>);
    static_assert(std::is_copy_constructible_v<BehaviorRuntimeServiceBindings>);
    static_assert(AcceptsRuntimeBindings<Runtime::RuntimeScene>);
    static_assert(AcceptsRuntimeBindings<Runtime::RuntimeSceneService>);
    static_assert(AcceptsPrefabScope<Runtime::RuntimeSceneService>);
    static_assert(!AcceptsPrefabScope<Runtime::RuntimeScene>);
    static_assert(!AcceptsLegacyRuntimeArguments<Runtime::RuntimeScene>);
    static_assert(!AcceptsLegacySceneArguments<Runtime::RuntimeSceneService>);
}  // namespace

int main() {
    Horo::Prefab::PrefabOperation empty;
    return empty.State() == Horo::Prefab::PrefabSpawnState::Failed && empty.Spawned().HasError() ? 0 : 1;
}
