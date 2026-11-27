#pragma once

/** @file LspProcess.h @brief Owned worker and bounded process queues for an editor LSP connection. */

#include "Horo/Platform/ExternalProcess.h"
#include "LspConnection.h"

#include <deque>
#include <mutex>
#include <thread>

namespace Horo::Editor::Language {
    /** @brief Runs one language process and exposes its protocol only on the workspace owner lane.
     * @details The runner and source session outlive this owner. Use Shutdown and Poll to retire
     * a project connection without waiting on the editor lane. Destruction cancels and joins any
     * remaining worker during host teardown. Restart constructs a new owner for the same source session.
     */
    class LspProcess final : private ILspTransport, private IProcessByteChannel {
    public:
        using Clock = LspConnection::Clock;

        /** @brief Binds the host-provided runner and the workspace source session. */
        LspProcess(IExternalProcessRunner &runner, SourceLanguageSession &source);
        /** @brief Cancels and joins the owned worker before releasing the queues. */
        ~LspProcess() override;

        LspProcess(const LspProcess &) = delete;
        LspProcess &operator=(const LspProcess &) = delete;

        /** @brief Admits initialization and starts work on the owned worker.
         * @param request Host-selected executable, arguments and process policy. I/O callbacks are owned here.
         * @param rootUri Project URI. @param hostPid Optional editor PID. @param now Monotonic time.
         * @return Queue or worker admission. Asynchronous launch errors are delivered by Poll.
         */
        [[nodiscard]] Result<void> Start(ExternalProcessRequest request, std::string_view rootUri, std::optional<std::int32_t> hostPid,
                                         Clock::time_point now);

        /** @brief Advances a bounded quantum of stdout and delivers at most one feature message or terminal error.
         * @param now Monotonic time. @return One incoming feature message, no work, or a typed failure.
         */
        [[nodiscard]] Result<std::optional<LspIncoming>> Poll(Clock::time_point now);

        /** @brief Returns the protocol owner for source sync, requests and graceful shutdown. */
        [[nodiscard]] LspConnection &Connection() noexcept {
            return connection_;
        }

        /** @brief Checks whether the owner observed final process completion or could not start. */
        [[nodiscard]] bool Finished() const noexcept {
            return finished_;
        }

        /** @brief Takes the bounded stderr capture accumulated since the preceding read. */
        [[nodiscard]] std::string TakeStandardError();

    private:
        using PollResult = Result<std::optional<LspIncoming>>;
        using Terminal = Result<ExternalProcessResult>;

        /** @copydoc ILspTransport::Enqueue */
        [[nodiscard]] Result<void> Enqueue(std::string frame) override;
        /** @copydoc ILspTransport::Cancel */
        void Cancel() noexcept override;
        /** @copydoc IProcessByteChannel::TakeInput */
        std::size_t TakeInput(std::span<char> destination) noexcept override;
        /** @copydoc IProcessByteChannel::AcceptOutput */
        bool AcceptOutput(std::span<const char> bytes) noexcept override;

        /** @brief Runs the process and publishes completion after the last stdout chunk. */
        void Run(ExternalProcessRequest request);
        /** @brief Posts one stderr line while retaining the fixed capture budget. */
        void RecordStandardError(const ProcessOutputLine &line);
        /** @brief Moves a new stdout chunk to the owner lane after the last chunk was fully consumed. */
        [[nodiscard]] bool NextChunk();
        /** @brief Delivers process completion once the remaining stdout has been processed or retired. */
        [[nodiscard]] PollResult ObserveStop(bool cancelled);

        IExternalProcessRunner &runner_;
        CancellationSource cancellation_;
        // Guards queue contents, cursors, diagnostics and the terminal receipt. The worker
        // never holds it during native I/O. The owner never holds it while decoding messages.
        std::mutex mutex_;
        std::deque<std::string> outgoing_;
        std::deque<std::string> incoming_;
        std::size_t outgoingBytes_{};
        std::size_t outgoingOffset_{};
        std::size_t incomingBytes_{};
        std::string stderr_;
        std::optional<Terminal> terminal_;

        LspConnection connection_;
        std::string chunk_;
        std::size_t chunkOffset_{};
        bool started_{};
        bool finished_{};
        std::jthread worker_;
    };
}  // namespace Horo::Editor::Language
