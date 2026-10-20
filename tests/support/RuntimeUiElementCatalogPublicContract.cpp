#include "Horo/Runtime/Ui/UiElementCatalog.h"
#include "Horo/Runtime/Ui/UiElementPresentation.h"

#include <type_traits>

static_assert(std::is_same_v<decltype(Horo::Runtime::Ui::UiElementPresentation{}.layout), Horo::Runtime::Ui::UiLayoutStyle>);
static_assert(
    std::is_same_v<decltype(Horo::Runtime::Ui::UiElementPresentation{}.inlineStyle), std::vector<Horo::Runtime::Ui::UiStyleAssignment>>);
static_assert(static_cast<std::uint16_t>(Horo::Runtime::Ui::UiElementKind::Canvas) == 1);
static_assert(static_cast<std::uint16_t>(Horo::Runtime::Ui::UiElementKind::CollectionView) == 14);
