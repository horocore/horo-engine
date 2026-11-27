#pragma once

/** @file AgentEditorDiagnosticsSnapshot.h @brief Owned editor diagnostics offered for explicit local-model consent. */

#include "Horo/Foundation/CancellationToken.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace Horo::Editor {
    /** @brief Bounded report text and revocation witness from the active authored document. */
    struct AgentEditorDiagnosticsSnapshot final {
        static constexpr std::size_t MaximumMessages = 8;
        static constexpr std::size_t MaximumMessageBytes = 3U << 10U;
        std::uint64_t document{};    /**< Exact document occurrence, never a file-system path. */
        std::uint64_t observation{}; /**< Owner-issued validation capture sequence. */
        std::vector<std::string> messages;
        bool truncated{};
        CancellationToken lifetime; /**< Revoked when the selected document or its findings change. */
    };
}  // namespace Horo::Editor
