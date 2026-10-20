#pragma once
/** @file CliHostCommands.h @brief Private typed adapters for existing host operations. */
#include "Horo/Application/HostObservability.h"
#include "Horo/Cli/CliDispatcher.h"

namespace Horo::Application::Internal {
    class PackageCliHost;
    class NavigationCliHost;
    /** @brief Executes the shared application observability smoke operation, used by CLI and MCP.
     * @return Success after recording the declared metrics and structured host event.
     */
    [[nodiscard]] Result<void> EmitHostObservabilitySmoke();
    /** @brief Builds inert metadata for the real host command inventory. @return Owned descriptors. */
    [[nodiscard]] std::vector<Cli::CliCommandDescriptor> DescribeHostCommands();
    /** @brief Creates host adapters over explicit invocation-owned application services.
     * @param descriptors Canonical inert command metadata retained by the dispatcher registry.
     * @param session Shared observability owner retained through tool callback drainage.
     * @param help Generated host help presentation.
     * @param packages Optional package binder; its owner outlives adapters and accepted work.
     * @param navigation Optional navigation binder; its owner outlives adapters and accepted work.
     * @return Owned registrations; discovery and construction start no application operation.
     */
    [[nodiscard]] std::vector<Cli::CliCommandAdapterRegistration> HostCommandAdapters(
        std::span<const Cli::CliCommandDescriptor> descriptors, std::shared_ptr<HostObservabilitySession> session, std::string help,
        PackageCliHost *packages = nullptr, NavigationCliHost *navigation = nullptr);
}  // namespace Horo::Application::Internal
