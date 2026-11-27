#pragma once

#include "AgentTurn.h"
#include "Horo/Security/SecureMemory.h"

namespace Horo::Agent {
    /** @brief Host-registered tool presented by the editor's scene count approval. */
    inline constexpr std::string_view LocalSceneCountTool = "scene.inspect";

    /** @brief Controller-issued redacted audit identity for the admitted scene tool. */
    struct LocalSceneAuditReference final {
        std::uint64_t principal{};
        std::uint64_t session{};
        std::uint64_t generation{};
        std::uint64_t operation{};
    };

    using LocalSceneAuditObserver = std::function<void(LocalSceneAuditReference)>;

    /** @brief Runs a local worker turn with one explicitly approved, immutable scene object count.
     * @param config Loopback endpoint configuration. @param request Turn without preexisting tool declarations.
     * @param turnId Exact host turn identifier. @param objectCount Value approved by the workspace user.
     * @param entropy Native host entropy for the temporary MCP session. @param stop Turn cancellation.
     * @param observeTool Optional lifecycle observer for the admitted scene tool.
     * @param observeAudit Optional observer for the exact accepted operation's redacted audit identity.
     * @return Terminal turn result. The worker owns the mediator and retains no workspace reference. */
    [[nodiscard]] AgentTurnResult RunApprovedLocalSceneTurn(ModelProviderConfig config, ModelRequest request, std::uint64_t turnId,
                                                            std::size_t objectCount, std::shared_ptr<Security::SecureRandomSource> entropy,
                                                            std::stop_token stop, const AgentToolActivityObserver &observeTool = {},
                                                            const LocalSceneAuditObserver &observeAudit = {});
}  // namespace Horo::Agent
