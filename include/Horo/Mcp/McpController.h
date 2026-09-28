#pragma once

/**
 * @file McpController.h
 * @brief Bounded owner-context dispatch and transport-neutral MCP operation lifecycle.
 */

#include "Horo/Mcp/McpToolRegistry.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <thread>

namespace Horo::Mcp {
    /** @brief Finite admission and retention budgets for one host-owned controller. */
    struct McpControllerLimits final {
        std::size_t maximumPending{64};
        std::size_t maximumActive{128};
        std::size_t maximumRecent{128};
        std::size_t maximumPumpBatch{8};
        std::chrono::milliseconds shutdownDrainTimeout{5'000};
    };

    /**
     * @brief One registry-backed controller shared by local and in-process sessions.
     * @details Host composition binds each owner context to its scheduler by calling Pump on
     * that context's owning thread. Editor/runtime adapters must start slow application
     * operations and return promptly; background/build pumps may run on host workers.
     * The controller never creates a worker or invokes an adapter on a transport thread.
     */
    class McpController final : public IMcpRequestController {
        struct State;

        struct ConstructionKey final {
        private:
            friend class McpController;
            ConstructionKey() = default;

        public:
            ConstructionKey(const ConstructionKey &) = default;
        };

    public:
        /** @brief Constructs a controller over one host registry. @param registry Registry lease. @param limits Finite budgets.
         * @return Controller or typed configuration failure. */
        [[nodiscard]] static Result<std::shared_ptr<McpController>> Create(std::shared_ptr<McpToolRegistry> registry,
                                                                           McpControllerLimits limits = {});

        /** @brief Construction seam restricted by the private key to Create. */
        explicit McpController(ConstructionKey, std::shared_ptr<State> state) noexcept;

        /** @copydoc IMcpRequestController::Dispatch */
        [[nodiscard]] Result<nlohmann::json> Dispatch(const McpRequest &request, const McpRequestContext &context) override;
        /** @copydoc IMcpRequestController::CancelAccepted */
        [[nodiscard]] Result<void> CancelAccepted(McpSessionHandle session, const nlohmann::json &requestId) override;

        /** @brief Binds an execution context to the current thread once, before accepting its work.
         * @param owner Context whose application state this thread owns. @return Success or typed owner conflict. */
        [[nodiscard]] Result<void> BindOwner(McpOwnerContext owner) const;

        /** @brief Executes up to maximumPumpBatch queued calls on the bound owner thread.
         * @param owner Context to pump. @return Number executed or typed wrong-thread failure. */
        [[nodiscard]] Result<std::size_t> Pump(McpOwnerContext owner) const;

        /** @brief Closes admission, finalizes queued work, cancels running work and drains bounded callbacks and owner pumps.
         * @return Success when callbacks and pumps drained, or typed timeout while callback state leases stay alive.
         * @note On timeout the host must retain application owners until their operations complete; the controller cannot
         * forcibly terminate an application-owned job or process. */
        [[nodiscard]] Result<void> Shutdown() const;

        /** @brief Best-effort shutdown fallback; hosts must call Shutdown and handle a drain timeout before destroying application owners.
         */
        ~McpController() noexcept override;
        McpController(const McpController &) = delete;
        McpController &operator=(const McpController &) = delete;

    private:
        /** @brief Lists capability-visible tools from the session's registry generation. */
        [[nodiscard]] Result<nlohmann::json> DispatchList(const McpRequestContext &context) const;
        /** @brief Queries or cancels one operation owned by the session generation. */
        [[nodiscard]] Result<nlohmann::json> DispatchOperation(const McpRequest &request, const McpRequestContext &context) const;
        /** @brief Validates a call and queues it for the declared owner. */
        [[nodiscard]] Result<nlohmann::json> DispatchCall(const McpRequest &request, const McpRequestContext &context) const;
        /** @brief Processes one queue entry; empty means the owner queue is drained. */
        [[nodiscard]] std::optional<bool> PumpOne(McpOwnerContext owner) const;
        std::shared_ptr<State> state_;
    };
}  // namespace Horo::Mcp
