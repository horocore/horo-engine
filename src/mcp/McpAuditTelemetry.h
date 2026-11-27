#pragma once

/** @file McpAuditTelemetry.h @brief Redacted MCP audit ingress shared by session, policy, and dispatch owners. */

#include "Horo/Mcp/McpSession.h"

#include <chrono>
#include <string_view>

namespace Horo::Mcp {
    /** @brief Host-generated audit metadata. String values come only from canonical event/state names or a published registry ID. */
    struct McpAuditEvent {
        std::string_view event;
        std::uint64_t principal{};
        McpSessionHandle session;
        std::uint64_t operation{};
        std::uint64_t approval{};
        std::string_view tool;
        std::string_view status;
        std::chrono::milliseconds elapsed{};
    };

    /** @brief Offers a bounded audit record to the asynchronous process logger. @param event Typed redacted metadata.
     * @details Called under the owner's lifecycle lock. Never runs consumers inline or affects the command result.
     */
    void EmitMcpAudit(const McpAuditEvent &event) noexcept;
}  // namespace Horo::Mcp
