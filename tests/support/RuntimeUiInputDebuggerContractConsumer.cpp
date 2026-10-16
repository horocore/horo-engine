#include "Horo/Runtime/Ui/UiInputDebugger.h"

#include <type_traits>
#include <utility>

using namespace Horo::Runtime::Ui;
static_assert(std::is_move_constructible_v<UiInputDebugger>);
static_assert(!std::is_copy_constructible_v<UiInputDebugger>);
static_assert(std::is_copy_constructible_v<UiInputDebugSnapshot>);
static_assert(std::is_same_v<decltype(std::declval<const UiInputDebugSnapshot &>().Candidates()), std::span<const UiHitTestCandidate>>);
static_assert(std::is_same_v<decltype(std::declval<const UiInputDebugSnapshot &>().Phases()), std::span<const UiEventPhaseObservation>>);
static_assert(
    std::is_same_v<decltype(std::declval<const UiInputDebugSnapshot &>().Captures()), std::span<const UiPointerCaptureInspection>>);
static_assert(std::is_same_v<decltype(std::declval<const UiFocusGraph &>().PreviewMove(UiNavigationDirection::Next)),
                             Horo::Result<UiFocusNavigationPreview>>);

int main() {
    UiInputDebuggerDescriptor invalid;
    return invalid.IsValid() ? 1 : 0;
}
