#pragma once

#include "Horo/Runtime/Ui/UiRenderSnapshot.h"

namespace Horo::Runtime::Ui::Test {
    /** @brief Derives test extraction identity from the real tree, preserving explicit interaction/view/limit inputs. */
    [[nodiscard]] inline UiRenderSnapshotDescriptor SnapshotDescriptor(const UiElementTree &tree,
                                                                       const UiInteractionRevision interactionRevision,
                                                                       const UiRenderSnapshotRevision snapshotRevision,
                                                                       const UiRenderViewId view, const UiRenderSnapshotLimits limits) {
        return {.instance = tree.Instance(),
                .canvas = tree.Canvas(),
                .document = tree.SourceDocument(),
                .documentRevision = tree.SourceDocumentRevision(),
                .treeRevision = tree.Revision(),
                .interactionRevision = interactionRevision,
                .snapshotRevision = snapshotRevision,
                .view = view,
                .limits = limits};
    }
}  // namespace Horo::Runtime::Ui::Test
