#include "Horo/Runtime/InputAccessibility.h"

int main() {
    auto processor = Horo::Input::DigitalAffordance::Create({});
    return processor.HasValue() && !processor.Value().Cancel().button.down ? 0 : 1;
}
