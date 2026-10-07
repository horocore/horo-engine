#pragma once
/** @file McpServe.h @brief Private headless MCP lifecycle and exclusive byte-channel composition. */
#include "Horo/Foundation/Result.h"
#include "Horo/Mcp/McpToolRegistry.h"

#include <functional>
#include <span>
#include <string>
#include <string_view>

namespace Horo::Cli {
    class CliExecutionContext;
}

namespace Horo::Application {
    class HostObservabilitySession;
}

namespace Horo::Application::Internal {
    /** @brief One bounded channel poll; empty bytes without disconnect permit owner pumping. */
    struct McpChannelRead final {
        std::string bytes;
        bool disconnected{};
    };

    /** @brief Invocation-owned channel operations. Reads and writes must observe stop and remain bounded. */
    struct McpServeChannel final {
        std::function<Result<McpChannelRead>()> read;
        std::function<Result<void>(std::string_view)> write;
        std::function<bool()> stopped;
    };

    /** @brief Serves one approved local caller through MCP-owned registry/controller/session behavior.
     * @param registry Host-published tools and application-owner leases.
     * @param channel Exclusive bounded protocol channel, borrowed until shutdown completes.
     * @param capabilities Explicit host-approved application grants for this local caller.
     * @return Original lifecycle error, cancellation, or success after disconnect and reverse shutdown.
     */
    [[nodiscard]] Result<void> ServeMcp(std::shared_ptr<Mcp::McpToolRegistry> registry, const McpServeChannel &channel,
                                        std::span<const std::string> capabilities = {});
    /** @brief Serves process stdio with bounded polling and process interrupt handling.
     * @param context Admitted invocation stop and deadline view.
     * @param session Application observability owner retained by tool adapters through drainage.
     * @return Typed outcome after restoring native process state.
     */
    [[nodiscard]] Result<void> ServeNativeMcp(const Cli::CliExecutionContext &context, std::shared_ptr<HostObservabilitySession> session);
}  // namespace Horo::Application::Internal
