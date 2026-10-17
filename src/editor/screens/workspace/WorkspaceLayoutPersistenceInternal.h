#pragma once
#include <cstddef>

namespace Horo::Editor {
    /** @brief Shared on-disk envelope bound, enforced before write and throughout streaming reads. */
    inline constexpr std::size_t MaximumWorkspaceDocumentBytes = 8U * 1024U * 1024U;
}  // namespace Horo::Editor
