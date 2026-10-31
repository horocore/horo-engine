#pragma once

#include "Horo/Editor/AnimationGraphDocument.h"

namespace Horo::Editor::AnimationDocumentInternal {
    /** @brief Checks prospective source collection bounds before copying a command's untrusted vectors.
     * @param candidate Already bounded intermediate source. @param command Borrowed typed command.
     * @param limits Captured project-lowered limits. @return Success or stable graph-capacity failure.
     * Full semantic validation remains with Animation's sole source validator at final commit.
     */
    [[nodiscard]] Result<void> CheckCommandBounds(const Animation::AnimationGraphData &candidate,
                                                  const AnimationGraphDocumentCommand &command, const AnimationGraphDocumentLimits &limits);
}  // namespace Horo::Editor::AnimationDocumentInternal
