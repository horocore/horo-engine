#pragma once

/** @file WorkspaceMcpControls.h @brief Local editor controls for active approved MCP requests. */

#include "Horo/Foundation/Result.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace Horo::Editor {
    /** @brief Redacted correlation that identifies exactly one authenticated session generation. */
    struct WorkspaceMcpSessionKey {
        std::uint64_t principal{};
        std::uint64_t session{};
        std::uint64_t generation{};

        [[nodiscard]] bool operator==(const WorkspaceMcpSessionKey &) const = default;
    };

    /** @brief Exact logged operation identity. Used only for local history presentation. */
    struct WorkspaceMcpAuditReference {
        WorkspaceMcpSessionKey session;
        std::uint64_t operation{};

        [[nodiscard]] bool operator==(const WorkspaceMcpAuditReference &) const = default;
    };

    /** @brief Active requests revision within the exact session. */
    struct WorkspaceMcpRevision {
        WorkspaceMcpSessionKey session;
        std::uint64_t requests{};
    };

    /** @brief A controller-owned request that may still be cancelled. */
    struct WorkspaceMcpRequest {
        std::uint64_t id{};
        std::string tool;
        bool running{};
    };

    /** @brief Bounded local projection. A zero principal means no live session. */
    struct WorkspaceMcpStatus {
        WorkspaceMcpRevision revision;
        std::vector<std::string> capabilities;
        std::vector<WorkspaceMcpRequest> requests;
    };

    /** @brief Owner-lane controls borrowed by the local editor, independent of audit persistence. */
    class IWorkspaceMcpControls {
    public:
        virtual ~IWorkspaceMcpControls() = default;

        /** @brief Refreshes the authorized local view.
         * @param known Last displayed session and request revision.
         * @return A new owned view, a session retirement update, or no update.
         */
        [[nodiscard]] virtual std::optional<WorkspaceMcpStatus> ReadMcpStatus(WorkspaceMcpRevision known) const = 0;

        /** @brief Cancels an exact owned request. @param session Displayed authority. @param request Owner-issued request ID.
         * @return Result of the cancellation attempt.
         */
        [[nodiscard]] virtual Result<void> CancelMcpRequest(WorkspaceMcpSessionKey session, std::uint64_t request) = 0;

        /** @brief Revokes an exact session and its workspace command leases. @param session Displayed authority.
         * @return Result of closing and draining the active session.
         */
        [[nodiscard]] virtual Result<void> RevokeMcpSession(WorkspaceMcpSessionKey session) = 0;
    };
}  // namespace Horo::Editor
