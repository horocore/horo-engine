#include "Horo/Runtime/Ui/UiHudAssociation.h"

#include <type_traits>

using namespace Horo::Runtime::Ui;
static_assert(std::is_nothrow_move_constructible_v<UiHudAssociation>);
static_assert(!std::is_move_assignable_v<UiHudAssociation>);
static_assert(!std::is_copy_constructible_v<UiHudAssociation>);
static_assert(std::is_copy_constructible_v<UiHudFrame>);
static_assert(!std::is_default_constructible_v<UiHudFrame>);
static_assert(!std::is_same_v<UiHudCameraId, UiRenderViewId>);
static_assert(!std::is_same_v<UiOverlayViewportId, UiFocusPlayerId>);
static_assert(std::is_same_v<decltype(std::declval<UiHudAssociation &>().Shutdown(std::declval<UiHotReload &>())), Horo::Result<void>>);
static_assert(std::is_default_constructible_v<UiReloadLease>);
static_assert(std::is_same_v<decltype(std::declval<const UiReloadLease &>().Get()), const UiReloadGeneration *>);

int main() {
    UiHudAssociationDescriptor descriptor;
    return descriptor.enabled && descriptor.visible && !descriptor.interactive && descriptor.providers.size() == MaximumUiBindingProviders
               ? 0
               : 1;
}
