#include "Horo/Runtime/Ui/UiPointerInput.h"
#include "Horo/Runtime/Ui/UiPointerInteraction.h"

#include <type_traits>

static_assert(!std::is_copy_constructible_v<Horo::Runtime::Ui::UiPointerInteraction>);
static_assert(std::is_nothrow_move_constructible_v<Horo::Runtime::Ui::UiPointerInteraction>);
static_assert(!std::is_move_constructible_v<Horo::Runtime::Ui::UiPointerInput>);
static_assert(std::is_base_of_v<Horo::Runtime::Ui::UiPointerInteractionHost, Horo::Runtime::Ui::UiAnimationOwner>);

int main() {
    // A descriptor is inert: invalid identities cannot create capture, publication or host authority.
    return Horo::Runtime::Ui::UiPointerInteraction::Create({}, {}).HasError() ? 0 : 1;
}
