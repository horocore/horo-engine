#pragma once

/** @file NavigationTools.h @brief Project-bound navigation operation adapters for MCP. */
#include "Horo/Application/NavigationBakeApplication.h"
#include "Horo/Mcp/McpToolRegistry.h"

namespace Horo::Mcp {
    /** @brief Creates inert bake/query/cancel registrations over one already authorized project owner.
     * @param application Shared owner retained until accepted work drains.
     * @param projectIdentity Exact host-admitted non-secret project identity.
     * @param projectGeneration Exact opening generation, never supplied by protocol arguments.
     * @param currentGeneration Owned live generation projection; zero denotes closed authority.
     * @param owner Actual application owner lane.
     * @return Bounded tools requiring horo.navigation.bake, with normal mutation approval/trust policy.
     * The composition root pumps this application between protocol requests and drains it on shutdown.
     */
    [[nodiscard]] Result<std::vector<McpToolRegistration>> MakeNavigationTools(
        std::shared_ptr<Application::NavigationBakeApplication> application, std::string projectIdentity, std::uint64_t projectGeneration,
        std::function<std::uint64_t()> currentGeneration, McpOwnerContext owner = McpOwnerContext::Runtime);
}  // namespace Horo::Mcp
