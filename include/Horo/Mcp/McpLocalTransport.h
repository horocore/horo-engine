#pragma once

/**
 * @file McpLocalTransport.h
 * @brief Bounded local newline-delimited JSON-RPC framing without a network listener.
 */

#include "Horo/Mcp/McpSession.h"

#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Horo::Mcp {
    /**
     * @brief Host-driven local byte-stream adapter for stdio or another approved local pipe.
     * @note The executable owns reading/writing and never routes logs to protocol output. This adapter opens no socket or thread.
     */
    class McpLocalTransport final {
    public:
        /**
         * @brief Opens an approved local session before the host starts feeding byte chunks.
         * @param manager Shared session/controller owner.
         * @param admission Host-approved caller identity and authority.
         * @return Owning adapter or typed admission failure.
         */
        [[nodiscard]] static Result<std::shared_ptr<McpLocalTransport>> Start(std::shared_ptr<McpSessionManager> manager,
                                                                              McpSessionAdmission admission);

        ~McpLocalTransport();

        McpLocalTransport(const McpLocalTransport &) = delete;
        McpLocalTransport &operator=(const McpLocalTransport &) = delete;

        /**
         * @brief Consumes a bounded byte chunk and returns complete newline-terminated JSON-RPC responses.
         * @param bytes Untrusted local bytes, possibly ending with a partial frame.
         * @return Zero or more complete frames; malformed and oversized frames produce bounded protocol errors.
         * @note A caller must not emit returned frames after disconnect. Concurrent calls may complete out of order by request ID.
         */
        [[nodiscard]] Result<std::vector<std::string>> Feed(std::string_view bytes);

        /**
         * @brief Drops a partial old-project frame and advances the shared session generation.
         * @param projectIdentity New host-approved project identity or no project.
         * @return New handle or typed failure.
         */
        [[nodiscard]] Result<McpSessionHandle> SwitchProject(std::optional<std::string> projectIdentity);

        /** @brief Idempotently cancels accepted work and discards partial framing on local disconnect. */
        void Disconnect() noexcept;

        /** @brief Returns the current session handle, or an invalid handle after disconnect. */
        [[nodiscard]] McpSessionHandle Session() const noexcept;

    private:
        struct CompleteFrame {
            std::string text;
            bool oversized{};
        };

        McpLocalTransport(std::shared_ptr<McpSessionManager> manager, McpSessionHandle session) noexcept;
        [[nodiscard]] std::optional<std::string> ProcessFrame(const CompleteFrame &frame, McpSessionHandle session) const;

        std::shared_ptr<McpSessionManager> manager_;
        mutable std::mutex mutex_;
        McpSessionHandle session_;
        std::string partial_;
        bool discarding_{};
    };
}  // namespace Horo::Mcp
