#include "Horo/Accessibility/VisualAccessibility.h"

int main() {
    return Horo::Accessibility::PrepareColorAccessibilityTransform({}).HasValue() ? 0 : 1;
}
