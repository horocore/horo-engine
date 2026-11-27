#pragma once
/** @file PCGPreviewWorkspace.h @brief Explicit host preview input composition, separate from authoritative Scene state. */
#include "Horo/Editor/PCGPreviewSession.h"

namespace Horo::Editor {
    /** @brief Immutable provider capture and finite CPU admission supplied on the workspace owner thread. */
    struct PCGPreviewInputs final {
        PCG::PCGSpatialSnapshot spatial;
        std::shared_ptr<const PCG::PCGPointSchema> schema;
        PCG::PCGCpuEvaluationLimits evaluation;
        std::size_t maximumPointsPerOutput{4096};
    };

    /** @brief Composes an explicit disposable sandbox: grid 1, box 2 and surface 3, never production Scene data.
     * @return Owning captured values/schema/content digest, or typed composition failure. */
    [[nodiscard]] Result<PCGPreviewInputs> CreatePCGPreviewSandbox();
}  // namespace Horo::Editor
