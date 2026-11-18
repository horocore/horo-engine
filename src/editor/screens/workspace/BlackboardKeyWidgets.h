#pragma once

#include "Horo/AI/BlackboardSchema.h"

#include <string>

namespace Horo::Editor {
    struct EditorGuiContext;
    struct EditorWorkspaceViewCommandData;

    /** @brief Presents the same typed descriptor and default controls for a draft without issuing a document edit. */
    void DrawBlackboardDraftKey(const EditorGuiContext &context, AI::BlackboardKeyDescriptor &key, std::string &numericDefault,
                                EditorWorkspaceViewCommandData &command);
}  // namespace Horo::Editor
