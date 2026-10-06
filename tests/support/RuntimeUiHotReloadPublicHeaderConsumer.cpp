#include "Horo/Runtime/Ui/UiHotReload.h"

#include <type_traits>

static_assert(std::is_move_constructible_v<Horo::Runtime::Ui::UiHotReload>);
static_assert(!std::is_copy_constructible_v<Horo::Runtime::Ui::UiHotReload>);
static_assert(std::is_copy_constructible_v<Horo::Runtime::Ui::UiReloadLease>);
static_assert(std::is_nothrow_move_constructible_v<Horo::Runtime::Ui::UiHotReload::Prepared>);
static_assert(std::is_nothrow_move_assignable_v<Horo::Runtime::Ui::UiHotReload::Prepared>);
static_assert(!std::is_copy_constructible_v<Horo::Runtime::Ui::UiHotReload::Prepared>);
static_assert(!std::is_copy_assignable_v<Horo::Runtime::Ui::UiHotReload::Prepared>);

template <typename Owner>
concept ConstOwnerAdmitsShutdown = requires(const Owner &owner) { owner.Shutdown(); };
template <typename Owner>
concept ConstOwnerAdmitsCollection = requires(const Owner &owner) { owner.CollectRetired(); };
template <typename Owner>
concept ConstOwnerAdmitsPrepare =
    requires(const Owner &owner, Horo::Runtime::Ui::UiReloadGeneration generation) { owner.Prepare(std::move(generation)); };
template <typename Owner>
concept ConstOwnerAdmitsCommit = requires(const Owner &owner, Horo::Runtime::Ui::UiHotReload::Prepared &prepared) {
    owner.Commit(prepared, Horo::Runtime::Ui::UiStructuralCommitPoint::ApplyQueuedOwnerThreadCommands);
};
template <typename Owner>
concept ConstOwnerAdmitsPresentation =
    requires(const Owner &owner, Horo::Runtime::Ui::UiCanvasId canvas, const Horo::Runtime::Ui::UiPresentationReceipt &receipt) {
        owner.ApplyPresentation(canvas, receipt);
    };
static_assert(!ConstOwnerAdmitsPrepare<Horo::Runtime::Ui::UiHotReload>);
static_assert(!ConstOwnerAdmitsCommit<Horo::Runtime::Ui::UiHotReload>);
static_assert(!ConstOwnerAdmitsPresentation<Horo::Runtime::Ui::UiHotReload>);
static_assert(!ConstOwnerAdmitsShutdown<Horo::Runtime::Ui::UiHotReload>);
static_assert(!ConstOwnerAdmitsCollection<Horo::Runtime::Ui::UiHotReload>);
static_assert(!std::is_copy_constructible_v<Horo::Runtime::Ui::UiReloadGeneration>);

int main() {
    using namespace Horo::Runtime::Ui;
    auto allocator = UiElementSlotAllocator::Create(UiOwnershipGeneration::Create(704).Value(), 100);
    UiReloadLease empty;
    return allocator.HasValue() && !empty.Get() && UiHotReloadLimits{}.IsValid() ? 0 : 1;
}
