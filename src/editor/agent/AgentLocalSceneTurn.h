#pragma once

#include "AgentTurn.h"
#include "Horo/Security/SecureMemory.h"

namespace Horo::Agent {
    /** @brief Runs a local worker turn with one explicitly approved, immutable scene object count.
     * @param config Loopback endpoint configuration. @param request Turn without preexisting tool declarations.
     * @param turnId Exact host turn identifier. @param objectCount Value approved by the workspace user.
     * @param entropy Native host entropy for the temporary MCP session. @param stop Turn cancellation.
     * @param observeTool Optional lifecycle observer for the admitted scene tool.
     * @return Terminal turn result. The worker owns the mediator and retains no workspace reference. */
    [[nodiscard]] AgentTurnResult RunApprovedLocalSceneTurn(ModelProviderConfig config, ModelRequest request, std::uint64_t turnId,
                                                            std::size_t objectCount, std::shared_ptr<Security::SecureRandomSource> entropy,
                                                            std::stop_token stop, const AgentToolActivityObserver &observeTool = {});
}  // namespace Horo::Agent
