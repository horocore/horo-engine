#pragma once

/** @file LspConnection.h @brief Owner-lane glue between a language process transport and source revisions. */

#include "Horo/Editor/SourceLanguageSession.h"
#include "LspLifecycle.h"

namespace Horo::Editor::Language {
    /** @brief Transport operations used on the editor lane. Native process ownership stays with the host. */
    class ILspTransport {
    public:
        virtual ~ILspTransport() = default;

        /** @brief Offers a frame to the bounded transport queue without blocking.
         * @param frame Complete owned bytes. @return Success only if the transport took the whole frame.
         */
        [[nodiscard]] virtual Result<void> Enqueue(std::string frame) = 0;

        /** @brief Requests cancellation of the connection without blocking or invoking callbacks inline. */
        virtual void Cancel() noexcept = 0;
    };

    /** @brief Owns the protocol state for exactly one process connection.
     * @details Call on the workspace owner lane. The borrowed source owner and transport outlive this
     * object. Transport callbacks must be routed by incarnation before calling this object. Enqueue
     * and Cancel must not call back into it.
     */
    class LspConnection final {
    public:
        using Clock = LspLifecycle::Clock;

        /** @brief Borrows the project source session and one fresh transport. */
        LspConnection(SourceLanguageSession &source, ILspTransport &transport) noexcept;
        /** @brief Retires any live source work and requests asynchronous transport cancellation. */
        ~LspConnection();

        LspConnection(const LspConnection &) = delete;
        LspConnection &operator=(const LspConnection &) = delete;

        /** @brief Starts protocol negotiation on a fresh connected transport.
         * @param rootUri Workspace URI produced by the host. @param processId Optional host PID.
         * @param now Monotonic observation time. @return The admitted source session stamp or an error.
         */
        [[nodiscard]] Result<std::uint64_t> Start(std::string_view rootUri, std::optional<std::int32_t> processId, Clock::time_point now);

        /** @brief Processes bytes through at most one envelope, leaving the suffix available to the caller.
         * @param bytes Unconsumed transport bytes. @param now Monotonic observation time.
         * @return A validated feature message, no feature work, or an error that retires the connection.
         */
        [[nodiscard]] Result<std::optional<LspMessage>> Read(std::string_view &bytes, Clock::time_point now);

        /** @brief Stops new source work and queues graceful shutdown for a ready connection.
         * @param now Monotonic observation time. @return Queue admission or an error.
         */
        [[nodiscard]] Result<void> Shutdown(Clock::time_point now);

        /** @brief Applies handshake deadlines even when no bytes arrive. @param now Monotonic observation time.
         * @return True if this call retired an expired connection.
         */
        bool Expire(Clock::time_point now) noexcept;

        /** @brief Retires the session when the process owner reports terminal completion.
         * @param cleanExit True if the process exited successfully. @return An error for unexpected or truncated streams.
         */
        [[nodiscard]] Result<void> TransportStopped(bool cleanExit);

    private:
        using Incoming = Result<std::optional<LspMessage>>;

        /** @brief Consumes lifecycle responses before forwarding any ready feature message. */
        [[nodiscard]] Incoming Dispatch(LspMessage message, Clock::time_point now);
        /** @brief Queues an owned frame, retiring the connection on backpressure or failure. */
        [[nodiscard]] Result<void> Send(std::string frame);
        /** @brief Retires the connection once and requests asynchronous cancellation. */
        void Abort() noexcept;
        /** @brief Releases only this connection's source session stamp. */
        void RetireSource() noexcept;

        SourceLanguageSession &source_;
        ILspTransport &transport_;
        LspFrameReader reader_;
        LspLifecycle lifecycle_;
        std::uint64_t stamp_{};
        bool stopped_{};
    };
}  // namespace Horo::Editor::Language
