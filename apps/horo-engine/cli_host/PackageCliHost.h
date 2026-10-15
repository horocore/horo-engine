#pragma once
/** @file PackageCliHost.h @brief Invocation-owned headless package composition and cancellation drainage. */
#include "Horo/Cli/PackageCommands.h"
#include "McpServe.h"
#include "PackageHostPolicy.h"
#include "PackageProjectHost.h"

namespace Horo::Application::Internal {
    /** @brief Concrete services for one CLI/MCP process invocation, retained until all package callbacks are acknowledged. */
    class PackageCliHost final {
    public:
        PackageCliHost();
        ~PackageCliHost() noexcept;
        /** @brief Resolves only explicit host path options and prepares the project under the admitted invocation stop policy. */
        [[nodiscard]] Result<std::shared_ptr<PackageApplication>> Bind(const Cli::CliCommandRequest &request,
                                                                       Cli::CliExecutionContext &context);
        /** @brief Prepares one explicit project and issues a non-secret exact-generation MCP binding. */
        [[nodiscard]] Result<McpPackageBinding> BindMcp(const Cli::CliCommandRequest &request, Cli::CliExecutionContext &context);
        /** @brief Pumps the actual headless quiescent composition owner; clients cannot select this boundary. */
        [[nodiscard]] Result<Packages::PackageActivationBoundary> Boundary();
        /** @brief Provides the exact prepared host binding for MCP publication. */
        [[nodiscard]] PackageProjectHost *Project() noexcept;

    private:
        JobSystem jobs_;
        NativeDurableFileSystem files_;
        SteadyClock clock_;
        SystemWallClock wallClock_;
        UnconfiguredPackageTrust trust_;
        std::unique_ptr<PackageProjectHost> project_;
        std::filesystem::path projectRoot_, cacheRoot_, temporaryRoot_;
    };
}  // namespace Horo::Application::Internal
