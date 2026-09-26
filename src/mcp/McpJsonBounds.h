#pragma once

#include "Horo/Mcp/McpSession.h"

#include <string_view>

namespace Horo::Mcp {
    /** @brief Rejects frame size, nesting, string, and token bombs before JSON parsing allocates nodes. */
    [[nodiscard]] bool FrameWithinBounds(std::string_view frame, const McpSessionLimits &limits) noexcept;

    /** @brief Checks an already materialized JSON tree without serializing the embedded path. */
    [[nodiscard]] bool JsonWithinBounds(const nlohmann::json &value, const McpSessionLimits &limits, std::size_t maximumEncodedBytes);

    /** @brief Validates one bounded JSON-RPC request identity shared by both adapters. */
    [[nodiscard]] bool ValidRequestId(const nlohmann::json &id, const McpSessionLimits &limits) noexcept;
}  // namespace Horo::Mcp
