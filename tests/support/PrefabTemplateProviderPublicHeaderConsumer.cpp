#include "Horo/Prefab/PrefabTemplateProvider.h"

#include <type_traits>
#include <utility>

static_assert(std::is_same_v<decltype(std::declval<const Horo::Prefab::PrefabTemplateProvider &>().LimitProfile()),
                             const Horo::Prefab::PrefabLimitProfile &>);

#include <type_traits>

static_assert(!std::is_copy_constructible_v<Horo::Prefab::PrefabTemplateLoadHandle>);
static_assert(std::is_nothrow_move_constructible_v<Horo::Prefab::PrefabTemplateLoadHandle>);
static_assert(std::is_nothrow_move_assignable_v<Horo::Prefab::PrefabTemplateLoadHandle>);
static_assert(std::is_copy_constructible_v<Horo::Prefab::PrefabTemplateLease>);
static_assert(!std::is_constructible_v<Horo::Prefab::PrefabTemplateLease, Horo::Assets::AssetPayloadLease>);

using PreparedGroupMethod = Horo::Result<std::vector<Horo::Runtime::DeferredEntity>> (Horo::Prefab::PrefabTemplateProvider::*)(
    const Horo::Prefab::PrefabTemplateLease &, std::vector<Horo::Runtime::RuntimeComponentSet>, const Horo::CancellationToken &,
    Horo::Prefab::PrefabPreparedGroupOptions);
static_assert(std::is_same_v<decltype(static_cast<PreparedGroupMethod>(&Horo::Prefab::PrefabTemplateProvider::QueuePreparedGroup)),
                             PreparedGroupMethod>);

using PreparedSpawnGroupMethod = Horo::Result<std::vector<Horo::Runtime::DeferredEntity>> (Horo::Prefab::PrefabTemplateProvider::*)(
    const Horo::Prefab::PreparedPrefabSpawn &, std::vector<Horo::Runtime::RuntimeComponentSet>, const Horo::CancellationToken &,
    Horo::Prefab::PrefabPreparedGroupOptions);
static_assert(std::is_same_v<decltype(static_cast<PreparedSpawnGroupMethod>(&Horo::Prefab::PrefabTemplateProvider::QueuePreparedGroup)),
                             PreparedSpawnGroupMethod>);

int main() {
    const Horo::Prefab::PrefabTemplateLease lease;
    return lease.Template() || !lease.Dependencies().empty() || !lease.Artifact().Bytes().empty() ? 1 : 0;
}
