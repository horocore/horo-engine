#pragma once

#include "Horo/Runtime/Scene/SaveContentRequirements.h"

namespace Horo::Runtime::SaveContentDetail {
    /** @brief Restores schema-owned canonical owner/kind/identity order after an admitted asset remap. */
    Result<void> SortRequirements(std::vector<SaveContentRequirement> &requirements);
}  // namespace Horo::Runtime::SaveContentDetail
