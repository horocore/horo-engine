#include "Horo/Prefab/PrefabPersistentCodec.h"
#include "Horo/Prefab/PrefabPersistentState.h"
#include "Horo/Prefab/PrefabRestoreEntities.h"

#include <type_traits>

static_assert(std::is_final_v<Horo::Prefab::PrefabRestoreEntities>);
static_assert(std::is_copy_constructible_v<Horo::Prefab::PrefabPersistentState>);
static_assert(!std::is_copy_constructible_v<Horo::Runtime::SceneRestoreCandidateCommands>);

int main() {
    const Horo::Prefab::PrefabInstance empty;
    return empty.Root().IsValid() || empty.PersistentId().has_value() ? 1 : 0;
}
