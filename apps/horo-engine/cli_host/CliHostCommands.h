#pragma once
/** @file CliHostCommands.h @brief Private typed adapters for existing host operations. */
#include "Horo/Application/HostObservability.h"
#include "Horo/Cli/CliDispatcher.h"

namespace Horo::Application::Internal {
    /** @brief Executes the shared application observability smoke operation, used by CLI and MCP.
     * @return Success after recording the declared metrics and structured host event.
     */
    [[nodiscard]] Result<void> EmitHostObservabilitySmoke();
    /** @brief Builds inert metadata for the real host command inventory. @return Owned descriptors. */
    [[nodiscard]] std::vector<Cli::CliCommandDescriptor> DescribeHostCommands();
    /** @brief Binds typed adapters to the existing session; adapters retain their application session owner.
     * @param descriptors Accepted command declarations. @param session Existing observability session.
     * @param help Generated registry help, copied into its adapter.
     * @return Owned adapter registrations.
     */
    [[nodiscard]] std::vector<Cli::CliCommandAdapterRegistration> HostCommandAdapters(
        std::span<const Cli::CliCommandDescriptor> descriptors, std::shared_ptr<HostObservabilitySession> session, std::string help);
}  // namespace Horo::Application::Internal
