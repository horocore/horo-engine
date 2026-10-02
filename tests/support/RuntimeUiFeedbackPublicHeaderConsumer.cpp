#include "Horo/Runtime/Ui/UiFeedback.h"
#include "Horo/Runtime/Ui/UiLayout.h"

#include <type_traits>

static_assert(std::is_same_v<decltype(&Horo::Runtime::Ui::UiFocusGraph::UpdateLayout),
                             Horo::Result<void> (Horo::Runtime::Ui::UiFocusGraph::*)(const Horo::Runtime::Ui::UiLayoutSnapshot &)>);

int main() {
    const Horo::Runtime::Ui::UiFeedbackRealization realization{};
    return realization.IsValid() ? 0 : 1;
}
