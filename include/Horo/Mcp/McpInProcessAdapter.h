#pragma once

/**
 * @file McpInProcessAdapter.h
 * @brief Embedded MCP caller adapter that shares session/controller semantics without framing.
 */

#include "Horo/Mcp/McpSession.h"

#include <memory>
#include <mutex>
#include <optional>

namespace Horo::Mcp {
    /** @brief RAII embedded-session adapter over the same manager used by local byte streams. */
    class McpInProcessAdapter final {
    public:
        /**
         * @brief Starts one explicitly admitted embedded session.
         * @param manager Host-owned shared session service.
         * @param admission Host-approved identity, capabilities, and project binding.
         * @return Owning adapter or typed admission failure.
         */
        [[nodiscard]] static Result<std::shared_ptr<McpInProcessAdapter>> Start(std::shared_ptr<McpSessionManager> manager,
                                                                                McpSessionAdmission admission);

        ~McpInProcessAdapter();

        McpInProcessAdapter(const McpInProcessAdapter &) = delete;
        McpInProcessAdapter &operator=(const McpInProcessAdapter &) = delete;

        /**
         * @brief Submits a value request directly, with no JSON serialization or transport buffer.
         * @param request Bounded decoded request.
         * @return Same typed controller outcome and limits as the local adapter.
         */
        [[nodiscard]] Result<nlohmann::json> Call(const McpRequest &request) const;

        /**
         * @brief Cooperatively cancels an active request by its original identity.
         * @param requestId String or integer request identity.
         * @return Success only for an active request in this generation.
         */
        [[nodiscard]] Result<void> Cancel(const nlohmann::json &requestId) const;

        /**
         * @brief Revokes old project-bound work and adopts a new host-approved project binding.
         * @param projectIdentity New project identity or no project.
         * @return New current session handle or typed failure.
         */
        [[nodiscard]] Result<McpSessionHandle> SwitchProject(std::optional<std::string> projectIdentity);

        /** @brief Idempotently closes and cancels the owned session. */
        void Disconnect() noexcept;

        /** @brief Returns the current handle or an invalid handle after disconnect. */
        [[nodiscard]] McpSessionHandle Session() const noexcept;

    private:
        McpInProcessAdapter(std::shared_ptr<McpSessionManager> manager, McpSessionHandle session) noexcept;

        std::shared_ptr<McpSessionManager> manager_;
        mutable std::mutex mutex_;
        McpSessionHandle session_;
    };
}  // namespace Horo::Mcp
