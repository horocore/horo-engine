#pragma once

#include "Horo/Runtime/Input.h"
#include "Horo/Runtime/Ui/UiControls.h"

#include <optional>

namespace Horo::Runtime::Ui::InputAdapter {
    /**
     * @brief Delivers one focused Input snapshot to an exact Runtime UI text control.
     * @param router Input owner holding the committed frame.
     * @param context Exact text-focused Input context, not a gameplay context.
     * @param control Focused Runtime UI text control belonging to @p source.
     * @param source Exact owner and element evidence for the focused control.
     * @param sequence Non-zero owner-ordered sequence used only when committed text exists.
     * @return No value if no text/pre-edit is eligible, or the delivered text/pre-edit.
     *         A control rejection is a typed error and never routes to another control.
     * @details Native text-input enablement and candidate-window positioning remain host-owned.
     */
    [[nodiscard]] Result<std::optional<Input::TextInputDelivery>> DeliverFocusedText(Input::InputRouter &router,
                                                                                     const Input::InputContextToken &context,
                                                                                     UiControlStateMachine &control,
                                                                                     const UiActionSource &source, std::uint64_t sequence);
}  // namespace Horo::Runtime::Ui::InputAdapter
