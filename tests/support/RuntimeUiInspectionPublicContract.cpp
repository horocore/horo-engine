#include "Horo/Runtime/Ui/UiLayoutInspector.h"
#include "Horo/Runtime/Ui/UiLayoutWork.h"
#include "Horo/Runtime/Ui/UiPresentationProfiles.h"

#include <type_traits>
static_assert(std::is_copy_constructible_v<Horo::Runtime::Ui::UiLayoutInspectionSnapshot>);
static_assert(std::is_move_constructible_v<Horo::Runtime::Ui::UiPresentationProfileTable>);
static_assert(Horo::Runtime::Ui::MaximumUiPresentationProfiles == 32);
static_assert(std::is_copy_constructible_v<Horo::Runtime::Ui::UiLayoutWork>);
