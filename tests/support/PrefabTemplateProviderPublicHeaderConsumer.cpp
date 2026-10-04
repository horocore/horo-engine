#include "Horo/Prefab/PrefabTemplateProvider.h"

#include <type_traits>

static_assert(!std::is_copy_constructible_v<Horo::Prefab::PrefabTemplateLoadHandle>);
static_assert(std::is_nothrow_move_constructible_v<Horo::Prefab::PrefabTemplateLoadHandle>);
static_assert(std::is_nothrow_move_assignable_v<Horo::Prefab::PrefabTemplateLoadHandle>);
static_assert(std::is_copy_constructible_v<Horo::Prefab::PrefabTemplateLease>);
static_assert(!std::is_constructible_v<Horo::Prefab::PrefabTemplateLease, Horo::Assets::AssetPayloadLease>);

int main() {
    const Horo::Prefab::PrefabTemplateLease lease;
    return lease.Template() || !lease.Dependencies().empty() || !lease.Artifact().Bytes().empty() ? 1 : 0;
}
