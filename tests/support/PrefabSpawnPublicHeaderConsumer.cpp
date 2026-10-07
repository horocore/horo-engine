#include "Horo/Gameplay/BehaviorRuntime.h"
#include "Horo/Prefab/PrefabSpawnService.h"

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

int main() {
    Horo::Prefab::PrefabOperation empty;
    return empty.State() == Horo::Prefab::PrefabSpawnState::Failed && empty.Spawned().HasError() ? 0 : 1;
}
