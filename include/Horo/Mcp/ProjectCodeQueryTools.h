#pragma once

/** @file ProjectCodeQueryTools.h
 * @brief Explicit MCP query registrations over the neutral application code-query capability.
 */

#include "Horo/Application/ProjectCodeQuery.h"
#include "Horo/Mcp/McpToolRegistry.h"

namespace Horo::Mcp {
    /** @brief Builds inert registrations without invoking queries or publishing a registry.
     * @param capability Root-bound application owner retained by all adapters.
     * @param projectIdentity Exact identity admitted in MCP sessions for this project.
     * @param projectGeneration Exact application project-session revision.
     * @param currentGeneration Retained read-only owner projection; zero means closed. Must be safe on the declared owner context.
     * @param owner Explicit host scheduler; Background for disk reads, Editor only for short existing-snapshot providers.
     * @return Seven Query registrations requiring horo.project.code.query, or a typed malformed composition failure.
     * The host publishes these through its existing registry/authorization, grants access explicitly and
     * drains MCP callbacks before releasing the application's providers. There is no implicit host installation.
     */
    [[nodiscard]] Result<std::vector<McpToolRegistration>> MakeProjectCodeQueryTools(
        std::shared_ptr<Application::ProjectCodeQuery> capability, std::string projectIdentity, std::uint64_t projectGeneration,
        std::function<std::uint64_t()> currentGeneration, McpOwnerContext owner = McpOwnerContext::Background);
}  // namespace Horo::Mcp
