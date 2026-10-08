#include "Horo/Runtime/Ui/UiTextLayout.h"
#include "Horo/Runtime/Ui/UiTextUnicode.h"

#include <type_traits>

static_assert(!std::is_copy_constructible_v<Horo::Runtime::Ui::UiTextUnicodeRuntime>);
static_assert(!std::is_copy_constructible_v<Horo::Runtime::Ui::UiTextUnicodeAnalyzer>);
static_assert(std::is_nothrow_move_constructible_v<Horo::Runtime::Ui::UiTextUnicodeRuntime>);
static_assert(std::is_copy_constructible_v<Horo::Runtime::Ui::UiTextUnicodeAnalysis>);
static_assert(std::is_standard_layout_v<Horo::Runtime::Ui::UiTextLayoutCluster>);
