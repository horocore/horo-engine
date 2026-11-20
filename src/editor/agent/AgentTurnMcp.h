#pragma once

#include "AgentContext.h"
#include "AgentTurn.h"
#include "Horo/Mcp/McpInProcessAdapter.h"

namespace Horo::Agent {
    /** @brief Captured by the host alongside the exact authenticated project-bound MCP session. */
    struct AgentContextProjectAuthority final {
        std::string identity;
        std::uint64_t generation{};
    };

    /** @brief Editor-host adapter over one already authenticated in-process MCP session generation. */
    class AgentTurnMcp final : public IAgentTurnTools {
    public:
        /** @brief Captures the session generation before any model-derived operation is admitted.
         * @param adapter Exact authenticated MCP session.
         * @param project Same host-approved project binding as the authenticated session, or absence for turn-only use. */
        explicit AgentTurnMcp(std::shared_ptr<const Mcp::McpInProcessAdapter> adapter, AgentContextProjectAuthority project = {});

        /** @copydoc IAgentTurnTools::ValidArguments */
        [[nodiscard]] bool ValidArguments(std::string_view arguments) const override;

        /** @copydoc IAgentTurnTools::Run */
        [[nodiscard]] AgentTurnStatus Run(std::uint64_t turnId, std::size_t callOrdinal, const ModelToolIntent &intent,
                                          std::chrono::steady_clock::time_point deadline, std::stop_token stop,
                                          std::string &result) const override;

        /** @brief Captures one explicit diagnostic or build-status query through the authenticated MCP controller.
         * @param requestId Host-issued preview request identity.
         * @param itemOrdinal Unique item ordinal in that preview request.
         * @param query Typed explicit read-only context query.
         * @param context Borrowed project fence, stop state and deadline.
         * @return Typed project evidence or admission/stop failure.
         * @pre Host runs this on a worker while the registered MCP owner pumps query work. */
        [[nodiscard]] Result<Application::CodeQueryPage> ReadContextQuery(std::uint64_t requestId, std::size_t itemOrdinal,
                                                                          const Application::CodeQueryRequest &query,
                                                                          const Application::CodeQueryContext &context) const;

    private:
        /** @brief Submits and settles one session-fenced controller operation with its scoped cancellation. */
        [[nodiscard]] Result<nlohmann::json> CallTool(std::string id, std::string_view name, nlohmann::json arguments,
                                                      std::chrono::steady_clock::time_point deadline,
                                                      const std::function<bool()> &stopped) const;
        std::shared_ptr<const Mcp::McpInProcessAdapter> adapter_;
        Mcp::McpSessionHandle session_;
        AgentContextProjectAuthority project_;
    };
}  // namespace Horo::Agent
