#include "LspConnection.h"

#include <utility>

namespace Horo::Editor::Language {
    namespace {
        const ErrorCodeDescriptor UnexpectedStop{ErrorDomainId{"editor.language"},
                                                 ErrorCode{"editor.language.unexpected_stop"},
                                                 ErrorSeverity::Error,
                                                 "The language server stopped unexpectedly.",
                                                 {}};
    }  // namespace

    /** @copydoc LspConnection::LspConnection */
    LspConnection::LspConnection(SourceLanguageSession &source, ILspTransport &transport) noexcept
        : source_(source), transport_(transport) {}

    /** @copydoc LspConnection::~LspConnection */
    LspConnection::~LspConnection() {
        Abort();
    }

    /** @copydoc LspConnection::Start */
    Result<std::uint64_t> LspConnection::Start(const std::string_view rootUri, const std::optional<std::int32_t> processId,
                                               const Clock::time_point now) {
        if (stopped_ || lifecycle_.State() != LspLifecycleState::Idle)
            return Result<std::uint64_t>::Failure(MakeError(SourceDocumentErrors::Closed));
        auto session = source_.Start();
        if (session.HasError()) {
            Abort();
            return session;
        }
        stamp_ = session.Value();
        auto frame = lifecycle_.Start(rootUri, processId, now);
        if (frame.HasError()) {
            Abort();
            return Result<std::uint64_t>::Failure(std::move(frame).ErrorValue());
        }
        auto sent = Send(std::move(frame).Value());
        if (sent.HasError())
            return Result<std::uint64_t>::Failure(std::move(sent).ErrorValue());
        return session;
    }

    /** @copydoc LspConnection::Read */
    LspConnection::Incoming LspConnection::Read(std::string_view &bytes, const Clock::time_point now) {
        if (stopped_ || lifecycle_.State() == LspLifecycleState::Idle)
            return Incoming::Failure(MakeError(SourceDocumentErrors::Closed));
        auto frame = reader_.Consume(bytes);
        if (frame.HasError()) {
            Abort();
            return Incoming::Failure(std::move(frame).ErrorValue());
        }
        if (!frame.Value())
            return Incoming::Success(std::nullopt);
        auto message = DecodeLspMessage(*frame.Value());
        if (message.HasError()) {
            Abort();
            return Incoming::Failure(std::move(message).ErrorValue());
        }
        return Dispatch(std::move(message).Value(), now);
    }

    /** @copydoc LspConnection::Dispatch */
    LspConnection::Incoming LspConnection::Dispatch(LspMessage message, const Clock::time_point now) {
        auto result = lifecycle_.Receive(message, now);
        if (result.HasError()) {
            Abort();
            return Incoming::Failure(std::move(result).ErrorValue());
        }
        auto dispatch = std::move(result).Value();
        if (dispatch.outgoing) {
            auto sent = Send(std::move(*dispatch.outgoing));
            if (sent.HasError())
                return Incoming::Failure(std::move(sent).ErrorValue());
            if (lifecycle_.State() == LspLifecycleState::Ready) {
                auto ready = source_.MarkReady(stamp_);
                if (ready.HasError()) {
                    Abort();
                    return Incoming::Failure(std::move(ready).ErrorValue());
                }
            }
        }
        if (!dispatch.handled && lifecycle_.State() == LspLifecycleState::Ready)
            return Incoming::Success(std::move(message));
        return Incoming::Success(std::nullopt);
    }

    /** @copydoc LspConnection::Shutdown */
    Result<void> LspConnection::Shutdown(const Clock::time_point now) {
        if (stopped_)
            return Result<void>::Success();
        if (lifecycle_.State() == LspLifecycleState::Initializing) {
            Abort();
            return Result<void>::Success();
        }
        if (lifecycle_.State() != LspLifecycleState::Ready)
            return Result<void>::Success();
        auto frame = lifecycle_.Shutdown(now);
        RetireSource();
        if (frame.HasError()) {
            Abort();
            return Result<void>::Failure(std::move(frame).ErrorValue());
        }
        return Send(std::move(frame).Value());
    }

    /** @copydoc LspConnection::Expire */
    bool LspConnection::Expire(const Clock::time_point now) noexcept {
        if (stopped_ || !lifecycle_.Expire(now))
            return false;
        Abort();
        return true;
    }

    /** @copydoc LspConnection::TransportStopped */
    Result<void> LspConnection::TransportStopped(const bool cleanExit) {
        if (stopped_)
            return Result<void>::Success();
        stopped_ = true;
        RetireSource();
        auto framing = reader_.EndOfStream();
        lifecycle_.TransportStopped(cleanExit && framing.HasValue());
        if (framing.HasError())
            return framing;
        if (lifecycle_.State() != LspLifecycleState::Stopped)
            return Result<void>::Failure(MakeError(UnexpectedStop));
        return Result<void>::Success();
    }

    /** @copydoc LspConnection::Send */
    Result<void> LspConnection::Send(std::string frame) {
        auto sent = transport_.Enqueue(std::move(frame));
        if (sent.HasError())
            Abort();
        return sent;
    }

    /** @copydoc LspConnection::Abort */
    void LspConnection::Abort() noexcept {
        if (stopped_)
            return;
        stopped_ = true;
        RetireSource();
        transport_.Cancel();
    }

    /** @copydoc LspConnection::RetireSource */
    void LspConnection::RetireSource() noexcept {
        if (stamp_ != 0)
            source_.Stop(std::exchange(stamp_, 0));
    }
}  // namespace Horo::Editor::Language
