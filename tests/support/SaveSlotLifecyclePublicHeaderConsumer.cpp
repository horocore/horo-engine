#include "Horo/Runtime/Save/SaveSlotLifecycle.h"
#include "Horo/Runtime/Save/SaveSlotRetention.h"

#include <type_traits>

static_assert(std::is_move_constructible_v<Horo::Runtime::SaveSlotLifecycle>);
static_assert(!std::is_copy_constructible_v<Horo::Runtime::SaveSlotLifecycle>);
static_assert(std::is_same_v<decltype(&Horo::Runtime::SaveSlotLifecycle::RetentionSnapshot),
                             Horo::Result<Horo::Runtime::SaveSlotRetentionSnapshot> (Horo::Runtime::SaveSlotLifecycle::*)(
                                 const Horo::Runtime::SaveNamespaceAccessRequest &) const>);
using IoObserver = Horo::Runtime::ISaveSlotLifecycleIoObserver;
using LifecycleHost = Horo::Runtime::ISaveSlotLifecycleHost;
static_assert(
    std::is_same_v<decltype(&IoObserver::Before), Horo::Result<void> (IoObserver::*)(Horo::Runtime::SaveSlotLifecycleIoStage,
                                                                                     Horo::Runtime::SaveSlotLifecycleFileKind) noexcept>);
static_assert(
    std::is_same_v<decltype(&LifecycleHost::Recycle), Horo::Result<void> (LifecycleHost::*)(const Horo::Runtime::SaveSlotCatalogEntry &,
                                                                                            Horo::Runtime::ImmutableSaveArchive) noexcept>);

int main() {
    const Horo::Runtime::SaveSlotLifecyclePolicy policy;
    return !policy.retention.enabled && policy.retention.kinds[0].backupGenerations >= 1 && policy.capabilities == 0 &&
                   policy.signature == Horo::Runtime::SaveSignaturePolicy::Required
               ? 0
               : 1;
}
