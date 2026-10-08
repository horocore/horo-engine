#include "Horo/Prefab/PrefabTemplateProvider.h"

#include <type_traits>

static_assert(!std::is_copy_constructible_v<Horo::Prefab::PrefabTemplateLoadHandle>);
static_assert(std::is_nothrow_move_constructible_v<Horo::Prefab::PrefabTemplateLoadHandle>);
static_assert(std::is_nothrow_move_assignable_v<Horo::Prefab::PrefabTemplateLoadHandle>);
static_assert(std::is_copy_constructible_v<Horo::Prefab::PrefabTemplateLease>);
static_assert(!std::is_constructible_v<Horo::Prefab::PrefabTemplateLease, Horo::Assets::AssetPayloadLease>);

using PreparedGroupMethod = Horo::Result<std::vector<Horo::Runtime::DeferredEntity>> (Horo::Prefab::PrefabTemplateProvider::*)(
    const Horo::Prefab::PrefabTemplateLease &, std::vector<Horo::Runtime::RuntimeComponentSet>, const Horo::CancellationToken &,
    Horo::Prefab::PrefabPreparedGroupOptions);
static_assert(std::is_same_v<decltype(&Horo::Prefab::PrefabTemplateProvider::QueuePreparedGroup), PreparedGroupMethod>);

int main() {
    const Horo::Prefab::PrefabTemplateLease lease;
    return lease.Template() || !lease.Dependencies().empty() || !lease.Artifact().Bytes().empty() ? 1 : 0;
}
