#pragma once

/** @file LspLifecycle.h @brief Opening and closing handshake for one language server connection. */

#include "LspProtocol.h"

#include <chrono>

namespace Horo::Editor::Language {
    /** @brief Client lifecycle on a single transport incarnation. */
    enum class LspLifecycleState {
        Idle,
        Initializing,
        Ready,
        ShuttingDown,
        Exiting,
        Stopped,
        Failed
    };

    /** @brief Negotiated strategy for writable source documents. */
    enum class LspTextSync {
        Disabled,
        Full,
        Incremental
    };

    /** @brief Reports whether a message belonged to the handshake and the next frame, if any. */
    struct LspLifecycleDispatch final {
        bool handled{};
        std::optional<std::string> outgoing;
    };

    /** @brief Orders LSP handshake frames and bounds the time spent waiting for the peer.
     * @details The transport owner drives this object on one lane, delivers messages only from its
     * own connection, and enqueues returned frames in order before dispatching document requests.
     * A send failure requires retiring the connection. Restart uses a new lifecycle object.
     */
    class LspLifecycle final {
    public:
        LspLifecycle() = default;
        LspLifecycle(const LspLifecycle &) = delete;
        LspLifecycle &operator=(const LspLifecycle &) = delete;

        using Clock = std::chrono::steady_clock;
        static constexpr auto InitializeTimeout = std::chrono::seconds{10};
        static constexpr auto ShutdownTimeout = std::chrono::seconds{2};
        static constexpr std::size_t MaximumRootUriBytes = 16 * 1024;

        /** @brief Returns the current stage for this transport. */
        [[nodiscard]] LspLifecycleState State() const noexcept {
            return state_;
        }

        /** @brief Returns the server's negotiated document synchronization strategy. */
        [[nodiscard]] LspTextSync TextSync() const noexcept {
            return textSync_;
        }

        /** @brief Creates the first request for the connection.
         * @param rootUri URI produced by workspace composition. Empty means no workspace.
         * @param processId Host PID, when supplied by the process capability.
         * @param now Monotonic observation time.
         * @return Complete initialize frame or a typed refusal. A successful call begins negotiation.
         */
        [[nodiscard]] Result<std::string> Start(std::string_view rootUri, std::optional<std::int32_t> processId, Clock::time_point now);

        /** @brief Creates the shutdown request. Feature work must have stopped before this call.
         * @param now Monotonic observation time.
         * @return Complete shutdown frame or a refusal if the connection is not ready.
         */
        [[nodiscard]] Result<std::string> Shutdown(Clock::time_point now);

        /** @brief Consumes reserved lifecycle responses before feature dispatch.
         * @param message Decoded incoming envelope from this transport.
         * @param now Monotonic observation time.
         * @return A handled flag, an optional follow-up frame, or a terminal protocol error.
         */
        [[nodiscard]] Result<LspLifecycleDispatch> Receive(const LspMessage &message, Clock::time_point now);

        /** @brief Faults a handshake whose waiting period expired, even if the peer sends nothing.
         * @param now Monotonic observation time.
         * @return True on the call that expired the handshake, requesting transport retirement.
         */
        bool Expire(Clock::time_point now) noexcept;

        /** @brief Finalizes the lifecycle once the transport reports its terminal status.
         * @param cleanExit True for a clean process exit after delivery of the exit notification.
         */
        void TransportStopped(bool cleanExit) noexcept;

    private:
        /** @brief Completes the initialize response or faults the transport. */
        [[nodiscard]] Result<LspLifecycleDispatch> HandleInitialize(const LspMessage &message);
        /** @brief Completes the shutdown response, then allows only exit. */
        [[nodiscard]] Result<LspLifecycleDispatch> HandleShutdown(const LspMessage &message, Clock::time_point now);

        LspLifecycleState state_{LspLifecycleState::Idle};
        LspTextSync textSync_{LspTextSync::Disabled};
        std::optional<Clock::time_point> deadline_;
    };
}  // namespace Horo::Editor::Language
