#pragma once
/** @file PackageTools.h @brief Thin project-bound package MCP adapters over the application operation owner. */
#include "Horo/Application/PackageApplication.h"
#include "Horo/Mcp/McpToolRegistry.h"

namespace Horo::Mcp {
    /** @brief Creates inert registrations; no package operation runs during construction.
     * @param application Explicit application owner retained by adapters; its borrowed authorities outlive them.
     * @param projectIdentity Exact non-secret identity already admitted by the host's MCP project policy.
     * @param projectGeneration Exact application project generation.
     * @param currentGeneration Host-owned current generation projection; zero means closed.
     * @param owner Actual bound host owner lane; never a freely scheduled worker context.
     * @return Bounded restore/install/activate/disable/query/cancel tools or typed invalid configuration.
     * The composition root pumps the application at genuine safe points and closes/drains it after MCP callbacks.
     */
    [[nodiscard]] Result<std::vector<McpToolRegistration>> MakePackageTools(std::shared_ptr<Application::PackageApplication> application,
                                                                            std::string projectIdentity, std::uint64_t projectGeneration,
                                                                            std::function<std::uint64_t()> currentGeneration,
                                                                            McpOwnerContext owner = McpOwnerContext::Runtime);
}  // namespace Horo::Mcp
