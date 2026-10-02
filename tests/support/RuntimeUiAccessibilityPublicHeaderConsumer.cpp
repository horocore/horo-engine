#include "Horo/Runtime/Ui/UiAccessibilityChanges.h"

int main() {
    const Horo::Runtime::Ui::UiAccessibilityChangeLimits limits;
    return limits.IsValid() ? 0 : 1;
}
