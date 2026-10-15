#include "Horo/Navigation/NavigationCharacterAdapter.h"

#include <type_traits>

static_assert(!std::is_copy_constructible_v<Horo::Navigation::NavigationCharacterAdapter>);
static_assert(std::is_nothrow_move_constructible_v<Horo::Navigation::NavigationCharacterAdapter>);
static_assert(std::is_copy_constructible_v<Horo::Navigation::NavigationCharacterMotion>);

int main() {
    Horo::Navigation::NavigationCharacterAdapter adapter;
    const Horo::Navigation::NavigationCharacterMotion inert;
    return adapter.Submit(inert, {}).HasError() && adapter.ObserveFeedback().HasError() ? 0 : 1;
}
