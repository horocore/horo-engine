#pragma once

#include "Horo/Editor/EditorGuiContext.h"
#include "Horo/Editor/PartitionedWorldWorkspace.h"

namespace Horo::Editor {
    /** @brief Edits shape-specific fields in a detached source draft using shared controls.
     * Positions remain available through canonical viewport handles. No document/history mutation occurs.
     * @param source Detached authored draft. @param context Shared theme/localization.
     * @return Complete shape validation; invalid intermediate fields remain visible until fixed or cancelled. */
    [[nodiscard]] Result<void> DrawWorldSourceShapeControls(PartitionedWorldSource &source, const EditorGuiContext &context);
}  // namespace Horo::Editor
