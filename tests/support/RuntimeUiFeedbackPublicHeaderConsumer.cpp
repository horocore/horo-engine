#include "Horo/Runtime/Ui/UiAsyncActions.h"
#include "Horo/Runtime/Ui/UiControls.h"
#include "Horo/Runtime/Ui/UiFeedback.h"
#include "Horo/Runtime/Ui/UiLayout.h"
#include "Horo/Runtime/Ui/UiScreenStack.h"

#include <type_traits>

namespace {
    using namespace Horo::Runtime::Ui;
    static_assert(!std::is_invocable_v<decltype(&UiActionRouter::Enqueue), const UiActionRouter &, UiActionSource, UiActionCommand>);
    static_assert(!std::is_invocable_v<decltype(&UiAsyncActionProducer::Complete), const UiAsyncActionProducer &, UiActionPayload>);
    static_assert(!std::is_invocable_v<decltype(&UiAsyncActionStore::Cancel), const UiAsyncActionStore &, const UiAsyncActionKey &,
                                       UiActionCancellationReason>);
    static_assert(std::is_invocable_v<decltype(&UiAsyncActionStore::Snapshot), const UiAsyncActionStore &, const UiAsyncActionKey &>);
    static_assert(std::is_same_v<decltype(&UiFocusGraph::UpdateLayout), Horo::Result<void> (UiFocusGraph::*)(const UiLayoutSnapshot &)>);
}  // namespace

int main() {
    const Horo::Runtime::Ui::UiFeedbackRealization realization{};
    const Horo::Runtime::Ui::UiAsyncActionCancellation cancellation{};
    return realization.IsValid() && cancellation.IsCancellationRequested() ? 0 : 1;
}
