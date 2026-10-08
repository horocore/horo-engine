#include "Horo/Runtime/Ui/UiTheme.h"

#include <type_traits>

static_assert(!std::is_copy_constructible_v<Horo::Runtime::Ui::UiThemeRuntime>);
static_assert(std::is_nothrow_move_constructible_v<Horo::Runtime::Ui::UiThemeRuntime>);
static_assert(std::is_copy_constructible_v<Horo::Runtime::Ui::UiThemeSnapshot>);

int main() {
    const Horo::Runtime::Ui::UiThemeSurfaceProperties properties;
    return properties.fill.IsValid() ? 1 : 0;
}
