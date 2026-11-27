#pragma once

/** @file EnvironmentQueryDocumentFence.h @brief Exact workspace revision of one authored EQS source. */

#include "Horo/Editor/EditorSurfaceIdentity.h"

#include <compare>
#include <cstdint>

namespace Horo::Editor {
    /** @brief Correlates a typed action or close choice to the saved query's current workspace draft. */
    struct EnvironmentQueryDocumentFence final {
        DocumentInstanceId instance;
        std::uint64_t revision{};
        auto operator<=>(const EnvironmentQueryDocumentFence &) const = default;
    };
}  // namespace Horo::Editor
