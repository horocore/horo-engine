#pragma once

/** @file EditorWorkspaceInputRouting.h
 * @brief Workspace shortcut policy, shared by the actual screen and its input regressions.
 */
#include "EditorWorkspaceViewModel.h"
#include "Horo/Runtime/Input.h"

namespace Horo::Editor {
    /** @brief Frame-local native GUI capture facts, independent from the source surface's explicit ownership carrier. */
    struct WorkspaceInputCapture final {
        bool textInputRequested{};
        bool popupOpen{};
    };

    /** @brief Routes eligible workspace input, preserving application Save before text/popup scene-action suppression.
     * @param router Canonical action router. @param context Exact live workspace context. @param model Current document/selection
     * projection.
     * @param command Frame command containing the current source ownership carrier. @param capture Current GUI capture facts.
     */
    void RouteWorkspaceInputAction(Input::InputRouter &router, const Input::InputContextToken &context,
                                   const EditorWorkspaceViewModel &model, EditorWorkspaceViewCommandData &command,
                                   WorkspaceInputCapture capture);
}  // namespace Horo::Editor
