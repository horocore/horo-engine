#pragma once

#include "AgentTurn.h"
#include "Horo/Mcp/McpInProcessAdapter.h"

namespace Horo::Agent {
    /** @brief Editor-host adapter over one already authenticated in-process MCP session generation. */
    class AgentTurnMcp final : public IAgentTurnTools {
    public:
        /** @brief Captures the session generation before any model-derived operation is admitted. */
        explicit AgentTurnMcp(std::shared_ptr<const Mcp::McpInProcessAdapter> adapter);

        /** @copydoc IAgentTurnTools::ValidArguments */
        [[nodiscard]] bool ValidArguments(std::string_view arguments) const override;

        /** @copydoc IAgentTurnTools::Run */
        [[nodiscard]] AgentTurnStatus Run(std::uint64_t turnId, std::size_t callOrdinal, const ModelToolIntent &intent,
                                          std::chrono::steady_clock::time_point deadline, std::stop_token stop,
                                          std::string &result) const override;

    private:
        std::shared_ptr<const Mcp::McpInProcessAdapter> adapter_;
        Mcp::McpSessionHandle session_;
    };
}  // namespace Horo::Agent
