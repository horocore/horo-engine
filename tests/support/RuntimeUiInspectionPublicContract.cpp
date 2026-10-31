#include "Horo/Runtime/Ui/UiLayoutInspector.h"
#include "Horo/Runtime/Ui/UiLayoutWork.h"
#include "Horo/Runtime/Ui/UiPresentationProfiles.h"
#include "Horo/Runtime/Ui/UiTheme.h"

#include <type_traits>
static_assert(std::is_copy_constructible_v<Horo::Runtime::Ui::UiLayoutInspectionSnapshot>);
static_assert(std::is_move_constructible_v<Horo::Runtime::Ui::UiPresentationProfileTable>);
static_assert(Horo::Runtime::Ui::MaximumUiPresentationProfiles == 32);
static_assert(std::is_copy_constructible_v<Horo::Runtime::Ui::UiLayoutWork>);

using Horo::Runtime::Ui::UiDocument;
using Horo::Runtime::Ui::UiElementTree;
static_assert(requires(const UiDocument &document, const UiElementTree &tree, Horo::Runtime::Ui::UiCanvasId canvas) {
    Horo::Runtime::Ui::UiPresentationProfileTable::CreateForAuthoring(document, tree, canvas);
});

static_assert(requires(const Horo::Runtime::Ui::UiComputedStyleSnapshot &computed, const Horo::Runtime::Ui::UiThemeSurfaceInput &input,
                       const Horo::Runtime::Ui::UiThemeSurfaceProperties &properties) {
    Horo::Runtime::Ui::ProjectUiThemeSurface(computed, input, properties);
});
