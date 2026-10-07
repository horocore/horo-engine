#include "Horo/Gameplay/BehaviorRuntime.h"
#include "Horo/Prefab/PrefabSpawnService.h"

#include <type_traits>

static_assert(!std::is_copy_constructible_v<Horo::Prefab::PrefabSpawnService>);
static_assert(std::is_copy_constructible_v<Horo::Prefab::PrefabOperation>);
static_assert(std::is_copy_constructible_v<Horo::Prefab::PrefabInstance>);
static_assert(!std::is_same_v<Horo::Prefab::PrefabInitializationId, Horo::Prefab::PrefabPropertyId>);
static_assert(!std::is_same_v<Horo::Prefab::PrefabInitializationId, Horo::Prefab::CookedPrefabEntitySlot>);
static_assert(!std::is_convertible_v<Horo::Prefab::PrefabPropertyId, Horo::Prefab::PrefabInitializationId>);

int main() {
    Horo::Prefab::PrefabOperation empty;
    return empty.State() == Horo::Prefab::PrefabSpawnState::Failed && empty.Spawned().HasError() ? 0 : 1;
}
