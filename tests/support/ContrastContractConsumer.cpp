#include "Horo/Accessibility/Contrast.h"

int main() {
    const auto contrast = Horo::Accessibility::EvaluateTextContrast({1, 1, 1}, {});
    return contrast.HasValue() && contrast.Value().meetsAAA ? 0 : 1;
}
