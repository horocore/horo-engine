#pragma once

/**
 * @file McpSession.h
 * @brief Transport-neutral, bounded MCP sessions shared by local and embedded clients.
 */

#include "Horo/Foundation/CancellationToken.h"
#include "Horo/Foundation/Result.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>

namespace Horo::Mcp {
    /** @brief Explicit finite budgets for one host-owned MCP session service. */
    struct McpSessionLimits {
        std::size_t maximumSessions{16};
        std::size_t maximumInFlightPerSession{8};
        std::size_t maximumFrameBytes{1U << 20U};
        std::size_t maximumInputBytes{256U << 10U};
        std::size_t maximumResultBytes{256U << 10U};
        std::size_t maximumDepth{32};
        std::size_t maximumNodes{4096};
        std::size_t maximumStringBytes{64U << 10U};
        std::size_t maximumIdentityBytes{256};
        std::size_t maximumCapabilities{64};
        std::chrono::milliseconds requestTimeout{30'000};
        std::chrono::milliseconds shutdownDrainTimeout{5'000};
    };

    /** @brief Identity and authority supplied by an approving host, never inferred from frame content. */
    struct McpSessionAdmission {
        std::string clientIdentity;
        std::vector<std::string> capabilities;
        std::optional<std::string> projectIdentity;
        std::uint64_t authorizationRevision{1};
        std::uint64_t registryRevision{1};
    };

    /** @brief Monotonic session identity with a generation that changes on project replacement. */
    struct McpSessionHandle {
        std::uint64_t id{};
        std::uint64_t generation{};

        [[nodiscard]] bool IsValid() const noexcept {
            return id != 0 && generation != 0;
        }

        [[nodiscard]] bool operator==(const McpSessionHandle &) const noexcept = default;
    };

    /** @brief Decoded request envelope; embedded callers pass values without wire serialization. */
    struct McpRequest {
        nlohmann::json id;
        std::string method;
        nlohmann::json params = nlohmann::json::object();
    };

    /** @brief Immutable authority and lifetime view for one accepted controller callback. */
    struct McpRequestContext {
        McpSessionHandle session;
        std::string clientIdentity;
        std::vector<std::string> capabilities;
        std::optional<std::string> projectIdentity;
        std::uint64_t authorizationRevision{};
        std::uint64_t registryRevision{};
        CancellationToken cancellation;
        std::chrono::steady_clock::time_point deadline;

        /** @brief Reports cooperative cancellation or an elapsed deadline. */
        [[nodiscard]] bool IsStopRequested() const noexcept {
            return cancellation.IsCancellationRequested() || std::chrono::steady_clock::now() >= deadline;
        }
    };

    /** @brief Application-owned controller seam; later registry/tool tickets provide the concrete controller. */
    class IMcpRequestController {
    public:
        virtual ~IMcpRequestController() = default;

        /**
         * @brief Dispatches an admitted request through the host's one controller instance.
         * @param request Bounded decoded request.
         * @param context Immutable session authority, cancellation, and deadline snapshot.
         * @return Typed application outcome; failures retain their original Horo error identity.
         */
        [[nodiscard]] virtual Result<nlohmann::json> Dispatch(const McpRequest &request, const McpRequestContext &context) = 0;
    };

    /** @brief Concurrent session admission barrier and callback lifetime owner shared by both adapters. */
    class McpSessionManager final {
    public:
        /**
         * @brief Constructs a manager only when its controller and every finite limit are valid.
         * @param controller Shared controller lease that remains alive through admitted callbacks.
         * @param limits Host-declared session and request budgets.
         * @return A manager or a typed configuration failure.
         */
        [[nodiscard]] static Result<std::shared_ptr<McpSessionManager>> Create(std::shared_ptr<IMcpRequestController> controller,
                                                                               McpSessionLimits limits = {});

        /**
         * @brief Opens one explicitly approved local or embedded session.
         * @param admission Immutable caller capability and project admission snapshot.
         * @return Generation-checked session handle or a typed admission/capacity failure.
         */
        [[nodiscard]] Result<McpSessionHandle> Open(McpSessionAdmission admission);

        /**
         * @brief Executes through the common controller without holding the session lock during the callback.
         * @param session Current admitted session handle.
         * @param request Decoded request with a string or integer JSON-RPC identity.
         * @return Controller outcome or a typed admission, cancellation, or budget failure.
         */
        [[nodiscard]] Result<nlohmann::json> Dispatch(McpSessionHandle session, const McpRequest &request);

        /**
         * @brief Requests cooperative cancellation for one active request in the current generation.
         * @param session Current session handle.
         * @param requestId Original string or integer JSON-RPC request identity.
         * @return Success when an active request was cancelled, otherwise a typed failure.
         */
        [[nodiscard]] Result<void> Cancel(McpSessionHandle session, const nlohmann::json &requestId);

        /**
         * @brief Revokes old project-bound work and advances the session generation atomically.
         * @param session Current session handle.
         * @param projectIdentity New host-approved project identity, or no project.
         * @return New handle; old handles cannot dispatch or cancel new work.
         */
        [[nodiscard]] Result<McpSessionHandle> SwitchProject(McpSessionHandle session, std::optional<std::string> projectIdentity);

        /**
         * @brief Closes one session, cancelling every accepted request without waiting on a callback.
         * @param session Current or already closed handle.
         */
        void Close(McpSessionHandle session) noexcept;

        /**
         * @brief Closes admission, cancels sessions, and waits at most the declared drain budget.
         * @return Success after all callbacks drain, or a typed timeout. A timed-out callback retains its controller lease.
         */
        [[nodiscard]] Result<void> Shutdown();

        /** @brief Returns the number of currently admitted sessions. */
        [[nodiscard]] std::size_t ActiveSessions() const noexcept;

        /** @brief Returns the finite budgets shared by local and in-process adapters. */
        [[nodiscard]] const McpSessionLimits &Limits() const noexcept;

    private:
        struct State;
        explicit McpSessionManager(std::shared_ptr<State> state) noexcept;
        std::shared_ptr<State> state_;
    };
}  // namespace Horo::Mcp
