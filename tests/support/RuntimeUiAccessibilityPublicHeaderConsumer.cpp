#include "Horo/Runtime/Ui/UiAccessibilityChanges.h"

int main() {
    using namespace Horo::Runtime::Ui;
    const Horo::Runtime::Ui::UiAccessibilityChangeLimits limits;
    const auto reading = &UiAccessibilitySnapshot::ReadingOrder;
    const auto focus = &UiAccessibilitySnapshot::FocusOrder;
    const auto state = &UiAccessibilitySnapshot::FocusState;
    const auto delivery = &UiAccessibilityChangePublisher::Announcements;
    const auto acknowledge = &UiAccessibilityChangePublisher::Acknowledge;
    return limits.IsValid() && reading && focus && state && delivery && acknowledge ? 0 : 1;
}
