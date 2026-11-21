#include "Horo/Editor/SourceLanguageSession.h"

#include <limits>
#include <new>
#include <utility>

namespace Horo::Editor {
    namespace {
        template <class T> Result<T> Failure(const ErrorCodeDescriptor &code) {
            return Result<T>::Failure(MakeError(code));
        }
    }  // namespace

    /** @copydoc SourceLanguageSession::Start */
    Result<std::uint64_t> SourceLanguageSession::Start() {
        if (state_ != SourceLanguageSessionState::Disconnected)
            return Failure<std::uint64_t>(SourceDocumentErrors::Busy);
        if (session_ == std::numeric_limits<std::uint64_t>::max())
            return Failure<std::uint64_t>(SourceDocumentErrors::TooLarge);
        state_ = SourceLanguageSessionState::Initializing;
        nextRequest_ = 0;
        return Result<std::uint64_t>::Success(++session_);
    }

    /** @copydoc SourceLanguageSession::MarkReady */
    Result<void> SourceLanguageSession::MarkReady(const std::uint64_t session) {
        if (session != session_ || state_ != SourceLanguageSessionState::Initializing)
            return Failure<void>(SourceDocumentErrors::Stale);
        state_ = SourceLanguageSessionState::Ready;
        return Result<void>::Success();
    }

    /** @copydoc SourceLanguageSession::Stop */
    bool SourceLanguageSession::Stop(const std::uint64_t session) noexcept {
        if (session != session_ || state_ == SourceLanguageSessionState::Disconnected)
            return false;
        pending_.clear();
        documents_.clear();
        state_ = SourceLanguageSessionState::Disconnected;
        return true;
    }

    /** @copydoc SourceLanguageSession::IsReady */
    bool SourceLanguageSession::IsReady(const std::uint64_t session) const noexcept {
        return session == session_ && state_ == SourceLanguageSessionState::Ready;
    }

    /** @copydoc SourceLanguageSession::Retire */
    void SourceLanguageSession::Retire(const DocumentInstanceId instance) noexcept {
        std::erase_if(pending_, [instance](const auto &entry) {
            return entry.second.instance == instance;
        });
    }

    /** @copydoc SourceLanguageSession::Sync */
    Result<SourceLanguageSync> SourceLanguageSession::Sync(const std::uint64_t session, const SourceDocumentSnapshot &source) {
        using Output = Result<SourceLanguageSync>;
        if (!IsReady(session) || !source.IsCurrent())
            return Failure<SourceLanguageSync>(SourceDocumentErrors::Stale);
        try {
            const auto identity = source.Identity();
            const auto existing = documents_.find(identity.instance);
            if (existing == documents_.end()) {
                if (documents_.size() >= SourceDocumentLimits{}.maximumDocuments)
                    return Failure<SourceLanguageSync>(SourceDocumentErrors::TooLarge);
                documents_.emplace(identity.instance, source);
                return Output::Success({SourceLanguageSyncKind::Opened, {}});
            }
            if (existing->second.SharesStateWith(source))
                return Output::Success({SourceLanguageSyncKind::Unchanged, {}});
            if (existing->second.IsCurrent() || source.Revision() <= existing->second.Revision())
                return Failure<SourceLanguageSync>(SourceDocumentErrors::Stale);
            auto previous = existing->second.Identity().key.source;
            auto outcome = previous == identity.key.source ? Output::Success({SourceLanguageSyncKind::Changed, {}})
                                                           : Output::Success({SourceLanguageSyncKind::Rekeyed, std::move(previous)});
            Retire(identity.instance);
            existing->second = source;
            return outcome;
        } catch (const std::bad_alloc &) {
            return Failure<SourceLanguageSync>(SourceDocumentErrors::TooLarge);
        }
    }

    /** @copydoc SourceLanguageSession::SynchronizedSource */
    std::optional<SourceDocumentSnapshot> SourceLanguageSession::SynchronizedSource(const std::uint64_t session,
                                                                                    const DocumentInstanceId instance) const noexcept {
        if (!IsReady(session))
            return std::nullopt;
        const auto found = documents_.find(instance);
        if (found == documents_.end())
            return std::nullopt;
        return found->second;
    }

    /** @copydoc SourceLanguageSession::Close */
    bool SourceLanguageSession::Close(const std::uint64_t session, const DocumentInstanceId instance) noexcept {
        if (!IsReady(session) || documents_.erase(instance) == 0)
            return false;
        Retire(instance);
        return true;
    }

    /** @copydoc SourceLanguageSession::Request */
    Result<SourceLanguageRequestId> SourceLanguageSession::Request(const std::uint64_t session, const SourceLanguageRequestKind kind,
                                                                   const SourceDocumentSnapshot &source) {
        using Output = Result<SourceLanguageRequestId>;
        if (!IsReady(session) || !source.IsCurrent())
            return Failure<SourceLanguageRequestId>(SourceDocumentErrors::Stale);
        if (kind > SourceLanguageRequestKind::CodeActions)
            return Failure<SourceLanguageRequestId>(SourceDocumentErrors::Invalid);
        try {
            const auto instance = source.Identity().instance;
            const auto document = documents_.find(instance);
            if (document == documents_.end() || !document->second.SharesStateWith(source))
                return Failure<SourceLanguageRequestId>(SourceDocumentErrors::Stale);
            if (pending_.size() >= MaximumPendingRequests || nextRequest_ == std::numeric_limits<std::uint64_t>::max())
                return Failure<SourceLanguageRequestId>(SourceDocumentErrors::TooLarge);
            const auto request = nextRequest_ + 1;
            pending_.emplace(request, Pending{kind, instance, source.Revision()});
            nextRequest_ = request;
            return Output::Success({session, request});
        } catch (const std::bad_alloc &) {
            return Failure<SourceLanguageRequestId>(SourceDocumentErrors::TooLarge);
        }
    }

    /** @copydoc SourceLanguageSession::Cancel */
    bool SourceLanguageSession::Cancel(const SourceLanguageRequestId id) noexcept {
        return IsReady(id.session) && pending_.erase(id.request) != 0;
    }

    /** @copydoc SourceLanguageSession::Complete */
    Result<SourceLanguageReply> SourceLanguageSession::Complete(const SourceLanguageRequestId id) {
        if (!IsReady(id.session))
            return Failure<SourceLanguageReply>(SourceDocumentErrors::Stale);
        const auto pending = pending_.find(id.request);
        if (pending == pending_.end())
            return Failure<SourceLanguageReply>(SourceDocumentErrors::Stale);
        const auto request = pending->second;
        pending_.erase(pending);
        const auto document = documents_.find(request.instance);
        if (document == documents_.end() || document->second.Revision() != request.revision || !document->second.IsCurrent())
            return Failure<SourceLanguageReply>(SourceDocumentErrors::Stale);
        return Result<SourceLanguageReply>::Success({request.kind, document->second});
    }
}  // namespace Horo::Editor
