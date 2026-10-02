#include "Horo/Runtime/Ui/UiAsyncActions.h"
#include "Horo/Runtime/Ui/UiControls.h"
#include "Horo/Runtime/Ui/UiFeedback.h"
#include "Horo/Runtime/Ui/UiScreenStack.h"

int main() {
    const Horo::Runtime::Ui::UiFeedbackRealization realization{};
    const Horo::Runtime::Ui::UiAsyncActionCancellation cancellation{};
    return realization.IsValid() && cancellation.IsCancellationRequested() ? 0 : 1;
}
