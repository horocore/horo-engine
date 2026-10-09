#include "Horo/Runtime/Ui/UiOverlayLifecycle.h"

#include <type_traits>

using Horo::Runtime::Ui::UiOverlayLayerSnapshot;
using Horo::Runtime::Ui::UiOverlayLifecycle;
static_assert(!std::is_copy_constructible_v<UiOverlayLifecycle>);
static_assert(std::is_nothrow_move_constructible_v<UiOverlayLifecycle>);
static_assert(std::is_copy_constructible_v<UiOverlayLayerSnapshot>);

/** @brief Compile/link the backend-neutral lifecycle through its registered owning public boundary. */
void VerifyRuntimeUiOverlayPublicContract() {
    auto created = UiOverlayLifecycle::Create({});
    if (created.HasValue()) {
        auto owner = std::move(created).Value();
        (void)owner.InputStatus({});
        (void)owner.Snapshot({});
        owner.Shutdown();
        (void)owner.CollectRetired();
    }
}
