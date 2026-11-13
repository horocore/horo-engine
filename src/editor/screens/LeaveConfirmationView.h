#pragma once
/** @file LeaveConfirmationView.h
 * @brief Themed presentation of one host-owned leave requirement without navigation side effects.
 */
#include "Horo/Editor/GuiScreen.h"

#include <optional>

namespace Horo::Editor {
    struct EditorGuiContext;
    /** @brief Draws a blocking confirmation using shared modal chrome.
     * @param requirement Exact allowed actions owned by the screen host.
     * @param context Borrowed theme and localization services.
     * @return Chosen allowed action; closing the title-bar X returns Stay only when allowed.
     */
    [[nodiscard]] std::optional<LeaveAction> DrawLeaveConfirmation(const LeaveRequirement &requirement, const EditorGuiContext &context);
}  // namespace Horo::Editor
