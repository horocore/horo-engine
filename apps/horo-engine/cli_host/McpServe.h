#pragma once
/** @file McpServe.h @brief Private headless MCP lifecycle and exclusive byte-channel composition. */
#include "Horo/Foundation/Result.h"
#include "Horo/Mcp/McpAuthorization.h"
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
    class PackageApplication;
    class NavigationBakeApplication;
}  // namespace Horo::Application

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
        std::function<Result<void>()> pumpApplication; /**< Optional explicit owner safe-point, never selected by protocol inputs. */
    };

    /** @brief Host-authenticated caller and the exact policy shared by registry, controller and sessions. */
    struct McpServeAdmission final {
        std::shared_ptr<Mcp::McpAuthorization> authorization;
        Mcp::McpSessionAdmission session;
    };

    /** @brief Serves one approved local caller through MCP-owned registry/controller/session behavior.
     * @param registry Host-published tools and application-owner leases.
     * @param channel Exclusive bounded protocol channel, borrowed until shutdown completes.
     * @param admission Explicit host-issued principal and its retained policy.
     * @return Original lifecycle error, cancellation, or success after disconnect and reverse shutdown.
     */
    [[nodiscard]] Result<void> ServeMcp(std::shared_ptr<Mcp::McpToolRegistry> registry, const McpServeChannel &channel,
                                        const McpServeAdmission &admission);

    /** @brief Optional explicit project package binding issued by the executable, not by MCP arguments. */
    struct McpPackageBinding final {
        std::shared_ptr<PackageApplication> application;
        std::string projectIdentity;
        std::uint64_t generation{};
        std::function<std::uint64_t()> currentGeneration;
        std::function<Result<void>()> pump;
    };

    /** @brief Optional native navigation owner bound before protocol admission. */
    struct McpNavigationBinding final {
        std::shared_ptr<NavigationBakeApplication> application;
        std::string projectIdentity;
        std::uint64_t generation{};
        std::function<std::uint64_t()> currentGeneration;
        std::function<Result<void>()> pump;
    };

    /** @brief Serves process stdio with bounded polling and process interrupt handling.
     * @param context Admitted invocation stop and deadline view.
     * @param session Application observability owner retained by tool adapters through drainage.
     * @param packages Optional explicit host-approved project package owner and safe-point pump.
     * @param navigation Optional explicit navigation owner; protocol arguments cannot select a replacement project.
     * @return Typed outcome after restoring native process state.
     */
    [[nodiscard]] Result<void> ServeNativeMcp(const Cli::CliExecutionContext &context, std::shared_ptr<HostObservabilitySession> session,
                                              McpPackageBinding packages = {}, McpNavigationBinding navigation = {});
}  // namespace Horo::Application::Internal
