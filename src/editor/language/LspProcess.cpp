#include "LspProcess.h"

#include "Horo/Platform/PlatformErrors.h"

#include <algorithm>
#include <new>
#include <system_error>
#include <utility>

namespace Horo::Editor::Language {
    namespace {
        constexpr std::size_t QueueBytes = 8 * 1024 * 1024;
        constexpr std::size_t OutgoingFrames = 128;
        constexpr std::size_t IncomingChunks = 2048;
        constexpr std::size_t PollChunks = 16;
        constexpr std::size_t StderrBytes = 16 * 1024;
    }  // namespace

    /** @copydoc LspProcess::LspProcess */
    LspProcess::LspProcess(IExternalProcessRunner &runner, SourceLanguageSession &source) : runner_(runner), connection_(source, *this) {}

    /** @copydoc LspProcess::~LspProcess */
    LspProcess::~LspProcess() {
        Cancel();
    }

    /** @copydoc LspProcess::Start */
    Result<void> LspProcess::Start(ExternalProcessRequest request, const std::string_view rootUri,
                                   const std::optional<std::int32_t> hostPid, const Clock::time_point now) {
        if (started_ || finished_)
            return Result<void>::Failure(MakeError(SourceDocumentErrors::Closed));
        if (request.executable.empty() || request.maintenanceLease != nullptr) {
            finished_ = true;
            return Result<void>::Failure(MakeError(PlatformErrors::ProcessLaunchFailed));
        }
        auto session = connection_.Start(rootUri, hostPid, now);
        if (session.HasError()) {
            finished_ = true;
            return Result<void>::Failure(std::move(session).ErrorValue());
        }
        try {
            request.byteChannel = this;
            request.maximumOutputBytes = StderrBytes;
            request.maximumLineBytes = 2048;
            request.onOutput = [this](const ProcessOutputLine &line) {
                RecordStandardError(line);
            };
            worker_ = std::jthread{[this, request = std::move(request)]() mutable {
                Run(std::move(request));
            }};
            started_ = true;
            return Result<void>::Success();
        } catch (const std::system_error &) {
            Cancel();
        } catch (const std::bad_alloc &) {
            Cancel();
        }
        finished_ = true;
        static_cast<void>(connection_.TransportStopped(false));
        return Result<void>::Failure(MakeError(PlatformErrors::ProcessLaunchFailed));
    }

    /** @copydoc LspProcess::Poll */
    LspProcess::PollResult LspProcess::Poll(const Clock::time_point now) {
        if (!started_ || finished_)
            return PollResult::Success(std::nullopt);
        if (connection_.Expire(now))
            return PollResult::Failure(MakeError(PlatformErrors::ProcessIoFailed, "Language server handshake timed out."));
        if (cancellation_.Token().IsCancellationRequested())
            return ObserveStop(true);
        for (std::size_t count = 0; count < PollChunks; ++count) {
            if (!NextChunk())
                return ObserveStop(false);
            auto bytes = std::string_view{chunk_}.substr(chunkOffset_);
            auto message = connection_.Read(bytes, now);
            chunkOffset_ = chunk_.size() - bytes.size();
            if (message.HasError() || message.Value())
                return message;
        }
        return PollResult::Success(std::nullopt);
    }

    /** @copydoc LspProcess::TakeStandardError */
    std::string LspProcess::TakeStandardError() {
        const std::scoped_lock lock(mutex_);
        return std::exchange(stderr_, {});
    }

    /** @copydoc LspProcess::Enqueue */
    Result<void> LspProcess::Enqueue(std::string frame) {
        const std::scoped_lock lock(mutex_);
        if (terminal_ || cancellation_.Token().IsCancellationRequested())
            return Result<void>::Failure(MakeError(SourceDocumentErrors::Closed));
        if (outgoing_.size() == OutgoingFrames || frame.size() > QueueBytes - outgoingBytes_)
            return Result<void>::Failure(MakeError(SourceDocumentErrors::Busy));
        outgoing_.push_back(std::move(frame));
        outgoingBytes_ += outgoing_.back().size();
        return Result<void>::Success();
    }

    /** @copydoc LspProcess::Cancel */
    void LspProcess::Cancel() noexcept {
        cancellation_.RequestCancellation();
    }

    /** @copydoc LspProcess::TakeInput */
    std::size_t LspProcess::TakeInput(const std::span<char> destination) noexcept {
        const std::scoped_lock lock(mutex_);
        if (outgoing_.empty())
            return 0;
        const auto &frame = outgoing_.front();
        const auto bytes = std::string_view{frame}.substr(outgoingOffset_, destination.size());
        std::ranges::copy(bytes, destination.begin());
        outgoingOffset_ += bytes.size();
        if (outgoingOffset_ == frame.size()) {
            outgoingBytes_ -= frame.size();
            outgoing_.pop_front();
            outgoingOffset_ = 0;
        }
        return bytes.size();
    }

    /** @copydoc LspProcess::AcceptOutput */
    bool LspProcess::AcceptOutput(const std::span<const char> bytes) noexcept {
        try {
            const std::scoped_lock lock(mutex_);
            if (incoming_.size() == IncomingChunks || bytes.size() > QueueBytes - incomingBytes_)
                return false;
            incoming_.emplace_back(bytes.data(), bytes.size());
            incomingBytes_ += bytes.size();
            return true;
        } catch (const std::bad_alloc &) {
            return false;
        }
    }

    /** @copydoc LspProcess::Run */
    void LspProcess::Run(ExternalProcessRequest request) {
        std::optional<Terminal> result;
        try {
            result.emplace(runner_.Run(request, cancellation_.Token()));
        } catch (...) {
            result.emplace(Terminal::Failure(MakeError(PlatformErrors::ProcessIoFailed)));
        }
        const std::scoped_lock lock(mutex_);
        terminal_ = std::move(result);
    }

    /** @copydoc LspProcess::RecordStandardError */
    void LspProcess::RecordStandardError(const ProcessOutputLine &line) {
        try {
            const std::scoped_lock lock(mutex_);
            const auto remaining = StderrBytes - stderr_.size();
            if (remaining == 0)
                return;
            stderr_.append(std::string_view{line.text}.substr(0, remaining - 1));
            stderr_ += '\n';
        } catch (const std::bad_alloc &) {
            // Diagnostics are best effort. They must not interrupt process monitoring.
        }
    }

    /** @copydoc LspProcess::NextChunk */
    bool LspProcess::NextChunk() {
        if (chunkOffset_ < chunk_.size())
            return true;
        const std::scoped_lock lock(mutex_);
        if (incoming_.empty())
            return false;
        incomingBytes_ -= incoming_.front().size();
        chunk_ = std::move(incoming_.front());
        incoming_.pop_front();
        chunkOffset_ = 0;
        return true;
    }

    /** @copydoc LspProcess::ObserveStop */
    LspProcess::PollResult LspProcess::ObserveStop(const bool cancelled) {
        std::optional<Terminal> terminal;
        {
            const std::scoped_lock lock(mutex_);
            if (!terminal_ || (!cancelled && !incoming_.empty()))
                return PollResult::Success(std::nullopt);
            terminal = std::move(terminal_);
        }
        finished_ = true;
        if (terminal->HasError()) {
            static_cast<void>(connection_.TransportStopped(false));
            return PollResult::Failure(std::move(*terminal).ErrorValue());
        }
        const auto &process = terminal->Value();
        auto retired = connection_.TransportStopped(process.reason == ProcessTerminationReason::Exited && process.exitCode == 0);
        if (retired.HasError())
            return PollResult::Failure(std::move(retired).ErrorValue());
        return PollResult::Success(std::nullopt);
    }
}  // namespace Horo::Editor::Language
