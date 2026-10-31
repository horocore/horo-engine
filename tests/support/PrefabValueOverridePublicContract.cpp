#include "Horo/Prefab/PrefabDebugSnapshot.h"
#include "Horo/Prefab/PrefabValueOverrides.h"

#include <type_traits>
static_assert(!std::is_aggregate_v<Horo::Prefab::PrefabValueSchemaSnapshot>);
static_assert(!std::is_default_constructible_v<Horo::Prefab::PrefabValueSchemaSnapshot>);
static_assert(!std::is_default_constructible_v<Horo::Prefab::PrefabDebugSnapshot>);
static_assert(!std::is_aggregate_v<Horo::Prefab::PrefabDebugSnapshot>);
static_assert(std::is_copy_constructible_v<Horo::Prefab::PrefabDebugSnapshot>);
static_assert(std::is_copy_constructible_v<Horo::Prefab::PrefabValueOverrideSet>);
static_assert(std::is_nothrow_swappable_v<std::optional<Horo::Prefab::PrefabValueOverrideSet>>);
