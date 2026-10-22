#include "Horo/Runtime/Ui/UiElementCatalog.h"
#include "Horo/Runtime/Ui/UiElementPresentation.h"

#include <concepts>
#include <type_traits>

static_assert(std::is_same_v<decltype(Horo::Runtime::Ui::UiElementPresentation{}.layout), Horo::Runtime::Ui::UiLayoutStyle>);
static_assert(
    std::is_same_v<decltype(Horo::Runtime::Ui::UiElementPresentation{}.inlineStyle), std::vector<Horo::Runtime::Ui::UiStyleAssignment>>);
static_assert(static_cast<std::uint16_t>(Horo::Runtime::Ui::UiElementKind::Canvas) == 1);
static_assert(static_cast<std::uint16_t>(Horo::Runtime::Ui::UiElementKind::CollectionView) == 14);

static_assert(std::equality_comparable<Horo::Runtime::Ui::UiLocalizedText>);
static_assert(std::is_same_v<decltype(Horo::Runtime::Ui::UiElementPresentation{}.state), Horo::Runtime::Ui::UiElementAuthoredState>);
static_assert(
    std::is_same_v<decltype(Horo::Runtime::Ui::UiElementPresentation{}.localizedText), std::optional<Horo::Runtime::Ui::UiLocalizedText>>);
static_assert(std::is_same_v<decltype(Horo::Runtime::Ui::UiElementPresentation{}.accessibility),
                             std::optional<Horo::Runtime::Ui::UiElementAccessibility>>);
