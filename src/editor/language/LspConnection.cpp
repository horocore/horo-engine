#include "LspConnection.h"

#include "LspDocumentSync.h"

#include <new>
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
        try {
            rootUri_ = rootUri;
        } catch (const std::bad_alloc &) {
            Abort();
            return Result<std::uint64_t>::Failure(MakeError(SourceDocumentErrors::TooLarge));
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
                features_ = ReadLspFeatures(message.envelope.Get().at("result").at("capabilities"));
            }
        }
        if (!dispatch.handled && lifecycle_.State() == LspLifecycleState::Ready)
            return ReadFeature(std::move(message));
        return Incoming::Success(std::nullopt);
    }

    /** @copydoc LspConnection::ReadFeature */
    LspConnection::Incoming LspConnection::ReadFeature(LspMessage message) {
        if (message.kind != LspMessageKind::Result && message.kind != LspMessageKind::Error)
            return Incoming::Success(LspIncoming{std::move(message), std::nullopt, stamp_});
        const auto id = ReadLspRequestId(message);
        if (!id)
            return Incoming::Success(std::nullopt);
        auto reply = source_.Complete({stamp_, *id});
        if (reply.HasError())
            return Incoming::Success(std::nullopt);
        return Incoming::Success(LspIncoming{std::move(message), std::move(reply).Value(), stamp_});
    }

    /** @copydoc LspConnection::SyncSource */
    Result<void> LspConnection::SyncSource(const SourceDocumentSnapshot &snapshot, const std::string_view languageId) {
        if (stopped_ || lifecycle_.State() != LspLifecycleState::Ready || !snapshot.IsCurrent())
            return Result<void>::Failure(MakeError(SourceDocumentErrors::Stale));
        try {
            const auto previous = source_.SynchronizedSource(stamp_, snapshot.Identity().instance);
            if (previous && previous->SharesStateWith(snapshot))
                return Result<void>::Success();
            auto outgoing = BuildLspDocumentUpdate(rootUri_, languageId, snapshot, previous, lifecycle_.TextSync());
            if (outgoing.HasError())
                return Result<void>::Failure(std::move(outgoing).ErrorValue());
            auto sync = source_.Sync(stamp_, snapshot);
            if (sync.HasError())
                return Result<void>::Failure(std::move(sync).ErrorValue());
            return SendUpdate(std::move(outgoing).Value());
        } catch (const std::bad_alloc &) {
            return Result<void>::Failure(MakeError(SourceDocumentErrors::TooLarge));
        }
    }

    /** @copydoc LspConnection::CloseSource */
    Result<void> LspConnection::CloseSource(const DocumentInstanceId instance) {
        const auto source = source_.SynchronizedSource(stamp_, instance);
        if (!source)
            return Result<void>::Success();
        try {
            auto closing = BuildLspDocumentClose(rootUri_, source->Identity().key.source);
            if (closing.HasError()) {
                Abort();
                return Result<void>::Failure(std::move(closing).ErrorValue());
            }
            source_.Close(stamp_, instance);
            return Send(std::move(closing).Value());
        } catch (const std::bad_alloc &) {
            Abort();
            return Result<void>::Failure(MakeError(SourceDocumentErrors::TooLarge));
        }
    }

    /** @copydoc LspConnection::Request */
    Result<SourceLanguageRequestId> LspConnection::Request(const SourceDocumentSnapshot &snapshot, const SourceLanguageRequestKind kind,
                                                           const LspSourceQuery query) {
        using Output = Result<SourceLanguageRequestId>;
        if (!SupportsLspRequest(features_, kind))
            return Output::Failure(MakeError(SourceDocumentErrors::Invalid, "Language server does not offer this document request."));
        auto id = source_.Request(stamp_, kind, snapshot);
        if (id.HasError())
            return id;
        auto frame = BuildLspRequest(rootUri_, snapshot, kind, id.Value().request, query);
        if (frame.HasError()) {
            source_.Cancel(id.Value());
            return Output::Failure(std::move(frame).ErrorValue());
        }
        auto sent = Send(std::move(frame).Value());
        if (sent.HasError())
            return Output::Failure(std::move(sent).ErrorValue());
        return id;
    }

    /** @copydoc LspConnection::CancelRequest */
    Result<void> LspConnection::CancelRequest(const SourceLanguageRequestId id) {
        if (id.session != stamp_ || !source_.Cancel(id))
            return Result<void>::Success();
        auto frame = BuildLspCancellation(id.request);
        if (frame.HasError())
            return Result<void>::Failure(std::move(frame).ErrorValue());
        return Send(std::move(frame).Value());
    }

    /** @copydoc LspConnection::ProjectReply */
    Result<LanguageResult> LspConnection::ProjectReply(const LspIncoming &incoming) const {
        if (stamp_ == 0 || incoming.session != stamp_)
            return Result<LanguageResult>::Failure(MakeError(SourceDocumentErrors::Stale));
        return ProjectLspReply(incoming, rootUri_);
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
        try {
            auto sent = transport_.Enqueue(std::move(frame));
            if (sent.HasError())
                Abort();
            return sent;
        } catch (const std::bad_alloc &) {
            Abort();
            return Result<void>::Failure(MakeError(SourceDocumentErrors::TooLarge));
        }
    }

    /** @copydoc LspConnection::SendUpdate */
    Result<void> LspConnection::SendUpdate(LspDocumentUpdate update) {
        if (update.close) {
            auto sent = Send(std::move(*update.close));
            if (sent.HasError())
                return sent;
        }
        return Send(std::move(update.update));
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
