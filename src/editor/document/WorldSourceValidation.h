#pragma once

#include "Horo/Editor/PartitionedWorldWorkspace.h"

namespace Horo::Editor::Detail {
    /** @brief Validates bounded source identities, policy, color and shapes without publication.
     * @param document Complete proposed metadata. @param limits Already-admitted owner ceilings.
     * @return Original typed shape failure or an Editor invalid/unsupported/conflict/capacity rejection. */
    [[nodiscard]] Result<void> ValidateWorldDocumentSources(const PartitionedWorldDocument &document, const WorldDocumentLimits &limits);
    /** @brief Checks changed source revisions against the previous canonical root.
     * @param document Complete proposal. @param previous Current metadata.
     * @return Success or typed stale revision rejection. */
    [[nodiscard]] Result<void> ValidateWorldSourceSuccessors(const PartitionedWorldDocument &document,
                                                             const PartitionedWorldDocument &previous);
}  // namespace Horo::Editor::Detail
