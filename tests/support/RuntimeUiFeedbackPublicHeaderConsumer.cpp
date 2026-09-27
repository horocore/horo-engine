#include "Horo/Runtime/Ui/UiFeedback.h"

int main() {
    const Horo::Runtime::Ui::UiFeedbackRealization realization{};
    return realization.IsValid() ? 0 : 1;
}
