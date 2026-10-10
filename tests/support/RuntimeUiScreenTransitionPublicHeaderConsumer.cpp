#include "Horo/Runtime/Ui/UiScreenTransition.h"

#include <type_traits>

using Horo::Runtime::Ui::UiScreenTransition;
static_assert(std::is_nothrow_move_constructible_v<UiScreenTransition>);
static_assert(!std::is_copy_constructible_v<UiScreenTransition>);
static_assert(std::is_trivially_copyable_v<Horo::Runtime::Ui::UiScreenTransitionProgress>);

int main() {
    return Horo::Runtime::Ui::UiScreenTransitionLimits{}.maximumRetiredScreens == 4 ? 0 : 1;
}
