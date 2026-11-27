#include "Horo/PlatformServices/PlatformFriendsCoordinator.h"

#include "Horo/Foundation/Utf8.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace Horo::PlatformServices {
    namespace FriendsCoordinatorErrors {
        namespace {
            const ErrorDomainId Domain{"horo.platform.friends"};
        }

        const ErrorCodeDescriptor InvalidConfiguration{Domain,
                                                       ErrorCode{"platform.friends.invalid_configuration"},
                                                       ErrorSeverity::Error,
                                                       "Friends composition or limits are invalid.",
                                                       "Use exact Friends capability and finite presentation limits.",
                                                       false,
                                                       false};
        const ErrorCodeDescriptor InvalidPage{Domain,
                                              ErrorCode{"platform.friends.invalid_page"},
                                              ErrorSeverity::Error,
                                              "Friends presentation is malformed or exceeds its bounds.",
                                              "Discard the entire provider page.",
                                              false,
                                              false};
        const ErrorCodeDescriptor InvalidQuery{Domain,
                                               ErrorCode{"platform.friends.invalid_query"},
                                               ErrorSeverity::Error,
                                               "Friends query exceeds its finite bounds.",
                                               "Use an advertised finite page size and offset.",
                                               false,
                                               false};
        const ErrorCodeDescriptor Busy{Domain,
                                       ErrorCode{"platform.friends.busy"},
                                       ErrorSeverity::Error,
                                       "A friends query is already pending.",
                                       "Complete or cancel the pending query first.",
                                       false,
                                       true};
        const ErrorCodeDescriptor StaleQuery{Domain,
                                             ErrorCode{"platform.friends.stale_query"},
                                             ErrorSeverity::Error,
                                             "The friends query no longer has publication authority.",
                                             "Discard the result and retire native work privately.",
                                             false,
                                             false};
        const ErrorCodeDescriptor Closed{Domain,
                                         ErrorCode{"platform.friends.closed"},
                                         ErrorSeverity::Error,
                                         "Friends admission is closed.",
                                         "Use the active session coordinator.",
                                         false,
                                         false};
    }  // namespace FriendsCoordinatorErrors

    /** @copydoc PlatformSocialSubjectHandle::IsValid */
    bool PlatformSocialSubjectHandle::IsValid() const noexcept {
        return requester_.IsValid() && accessRevision_.IsValid() && PlatformSubjectNonce{nonce_}.IsValid();
    }

    /** @copydoc PlatformSocialSubjectHandle::IsCurrent */
    bool PlatformSocialSubjectHandle::IsCurrent(const PlatformSessionSnapshot &session) const noexcept {
        return IsValid() && session.Subject() == requester_ && session.AccessRevision() == accessRevision_ &&
               session.Phase() == PlatformSessionPhase::Active &&
               session.Capabilities().Access(PlatformServiceKind::Friends) == PlatformSessionAccessState::Granted;
    }

    /** @copydoc BuildPlatformSocialSubjectHandle */
    Result<PlatformSocialSubjectHandle> BuildPlatformSocialSubjectHandle(const PlatformSessionSnapshot &session,
                                                                         const PlatformSubjectNonce &nonce) {
        if (!nonce.IsValid())
            return Result<PlatformSocialSubjectHandle>::Failure(MakeError(PlatformSessionErrors::InvalidSnapshot));
        if (!session.Subject())
            return Result<PlatformSocialSubjectHandle>::Failure(MakeError(PlatformSessionErrors::NoSubject));
        if (auto access =
                ValidatePlatformSessionAccess(session, *session.Subject(), session.AccessRevision(), PlatformServiceKind::Friends);
            access.HasError())
            return Result<PlatformSocialSubjectHandle>::Failure(access.ErrorValue());
        PlatformSocialSubjectHandle handle;
        handle.requester_ = *session.Subject();
        handle.accessRevision_ = session.AccessRevision();
        handle.nonce_ = nonce.bytes;
        return Result<PlatformSocialSubjectHandle>::Success(handle);
    }

    /** @copydoc PlatformFriendsCoordinator::Create */
    Result<PlatformFriendsCoordinator> PlatformFriendsCoordinator::Create(PlatformServiceCapabilitySnapshot capabilities,
                                                                          PlatformSessionSnapshot session,
                                                                          PlatformFriendsCoordinatorConfig config) {
        if (ValidatePlatformServiceCapabilitySnapshot(capabilities, {}).HasError() ||
            capabilities.providerGeneration != session.ProviderGeneration() || config.maximumPageEntries == 0 ||
            config.maximumPageEntries > 128 || config.maximumDisplayNameBytes == 0 || config.maximumDisplayNameBytes > 1'024 ||
            config.maximumPageBytes == 0 || config.maximumPageBytes > 131'072 || config.retention <= std::chrono::milliseconds::zero() ||
            config.retention > std::chrono::minutes{5})
            return Result<PlatformFriendsCoordinator>::Failure(MakeError(FriendsCoordinatorErrors::InvalidConfiguration));
        const auto capability = std::ranges::find_if(capabilities.services, [](const auto &entry) {
            return entry.service == PlatformServiceKind::Friends;
        });
        if (capability->availability == PlatformServiceAvailability::Available &&
            (capability->limits.maxPageEntries == 0 || capability->limits.maxPayloadBytes == 0))
            return Result<PlatformFriendsCoordinator>::Failure(MakeError(FriendsCoordinatorErrors::InvalidConfiguration));
        return Result<PlatformFriendsCoordinator>::Success(PlatformFriendsCoordinator(*capability, std::move(session), config));
    }

    /** @copydoc PlatformFriendsCoordinator::PlatformFriendsCoordinator(PlatformServiceCapability, PlatformSessionSnapshot,
     * PlatformFriendsCoordinatorConfig) */
    PlatformFriendsCoordinator::PlatformFriendsCoordinator(PlatformServiceCapability capability, PlatformSessionSnapshot session,
                                                           PlatformFriendsCoordinatorConfig config)
        : capability_(std::move(capability)), session_(std::move(session)), config_(config) {}

    /** @copydoc PlatformFriendsCoordinator::PlatformFriendsCoordinator(PlatformFriendsCoordinator&&) */
    PlatformFriendsCoordinator::PlatformFriendsCoordinator(PlatformFriendsCoordinator &&other) noexcept
        : capability_(std::move(other.capability_)), session_(std::move(other.session_)), config_(other.config_),
          pending_(std::move(other.pending_)), page_(std::move(other.page_)), capturedAt_(other.capturedAt_), sequence_(other.sequence_),
          closed_(std::exchange(other.closed_, true)) {}

    /** @copydoc PlatformFriendsCoordinator::ValidateAccess */
    Result<void> PlatformFriendsCoordinator::ValidateAccess(const PlatformSubjectHandle subject,
                                                            const PlatformAccessPolicyRevision revision) const {
        if (closed_)
            return Result<void>::Failure(MakeError(FriendsCoordinatorErrors::Closed));
        if (capability_.availability != PlatformServiceAvailability::Available)
            return Result<void>::Failure(MakeError(capability_.unavailableReason == PlatformServiceUnavailableReason::NullProviderSelected
                                                       ? BackendErrors::NullProvider
                                                       : BackendErrors::ServiceUnavailable));
        return ValidatePlatformSessionAccess(session_, subject, revision, PlatformServiceKind::Friends);
    }

    /** @copydoc PlatformFriendsCoordinator::BeginQuery */
    Result<PlatformFriendsQueryIntent> PlatformFriendsCoordinator::BeginQuery(FriendsQuery query,
                                                                              const PlatformAccessPolicyRevision revision) {
        if (auto access = ValidateAccess(query.subject, revision); access.HasError())
            return Result<PlatformFriendsQueryIntent>::Failure(access.ErrorValue());
        if (query.pageSize == 0 || query.pageSize > std::min(config_.maximumPageEntries, capability_.limits.maxPageEntries) ||
            query.startIndex > std::numeric_limits<std::uint32_t>::max() - query.pageSize)
            return Result<PlatformFriendsQueryIntent>::Failure(MakeError(FriendsCoordinatorErrors::InvalidQuery));
        if (pending_)
            return Result<PlatformFriendsQueryIntent>::Failure(MakeError(FriendsCoordinatorErrors::Busy));
        if (sequence_ == std::numeric_limits<std::uint64_t>::max())
            return Result<PlatformFriendsQueryIntent>::Failure(MakeError(PlatformSessionErrors::GenerationExhausted));
        pending_ = PlatformFriendsQueryIntent{query, revision, ++sequence_};
        return Result<PlatformFriendsQueryIntent>::Success(*pending_);
    }

    /** @copydoc PlatformFriendsCoordinator::IsPending */
    bool PlatformFriendsCoordinator::IsPending(const PlatformFriendsQueryIntent &query) const noexcept {
        return pending_ && pending_->sequence == query.sequence && pending_->query.subject == query.query.subject &&
               pending_->query.pageSize == query.query.pageSize && pending_->query.startIndex == query.query.startIndex &&
               pending_->accessRevision == query.accessRevision;
    }

    /** @copydoc PlatformFriendsCoordinator::IsValidPage */
    bool PlatformFriendsCoordinator::IsValidPage(const PlatformFriendsQueryIntent &query, const FriendsPage &page) const noexcept {
        if (page.startIndex != query.query.startIndex || page.entries.size() > query.query.pageSize ||
            (page.hasMore && page.entries.empty()))
            return false;
        std::size_t bytes{};
        const auto limit = std::min<std::uint64_t>(config_.maximumPageBytes, capability_.limits.maxPayloadBytes);
        for (auto entry = page.entries.begin(); entry != page.entries.end(); ++entry) {
            if (!entry->subject.IsCurrent(session_) || entry->displayName.size() > config_.maximumDisplayNameBytes ||
                !IsValidUtf8ScalarSequence(entry->displayName) || entry->displayName.find('\0') != std::string::npos ||
                entry->displayName.size() > limit - bytes)
                return false;
            bytes += entry->displayName.size();
            if (std::any_of(page.entries.begin(), entry, [&](const auto &prior) {
                return prior.subject == entry->subject;
            }))
                return false;
        }
        return true;
    }

    /** @copydoc PlatformFriendsCoordinator::Complete */
    Result<void> PlatformFriendsCoordinator::Complete(const PlatformFriendsQueryIntent &query, Result<FriendsPage> result,
                                                      const std::chrono::steady_clock::time_point now) {
        if (auto access = ValidateAccess(query.query.subject, query.accessRevision); access.HasError())
            return access;
        if (!IsPending(query))
            return Result<void>::Failure(MakeError(FriendsCoordinatorErrors::StaleQuery));
        pending_.reset();
        page_.reset();
        capturedAt_.reset();
        if (result.HasError())
            return Result<void>::Failure(result.ErrorValue());
        if (!IsValidPage(query, result.Value()))
            return Result<void>::Failure(MakeError(FriendsCoordinatorErrors::InvalidPage));
        page_ = std::move(result).Value();
        capturedAt_ = now;
        return Result<void>::Success();
    }

    /** @copydoc PlatformFriendsCoordinator::Cancel */
    Result<void> PlatformFriendsCoordinator::Cancel(const PlatformFriendsQueryIntent &query) {
        if (closed_)
            return Result<void>::Failure(MakeError(FriendsCoordinatorErrors::Closed));
        if (!IsPending(query))
            return Result<void>::Failure(MakeError(FriendsCoordinatorErrors::StaleQuery));
        pending_.reset();
        return Result<void>::Success();
    }

    /** @copydoc PlatformFriendsCoordinator::ReadPage */
    Result<std::optional<FriendsPage>> PlatformFriendsCoordinator::ReadPage(const PlatformSubjectHandle subject,
                                                                            const PlatformAccessPolicyRevision revision,
                                                                            const std::chrono::steady_clock::time_point now) {
        if (auto access = ValidateAccess(subject, revision); access.HasError())
            return Result<std::optional<FriendsPage>>::Failure(access.ErrorValue());
        if (capturedAt_ && (now < *capturedAt_ || now - *capturedAt_ >= config_.retention)) {
            page_.reset();
            capturedAt_.reset();
        }
        return Result<std::optional<FriendsPage>>::Success(page_);
    }

    /** @copydoc PlatformFriendsCoordinator::UpdateSession */
    Result<void> PlatformFriendsCoordinator::UpdateSession(PlatformSessionSnapshot session) {
        if (closed_)
            return Result<void>::Failure(MakeError(FriendsCoordinatorErrors::Closed));
        if (session.Generation() < session_.Generation() || session.ProviderGeneration() != session_.ProviderGeneration() ||
            session.AccessRevision() < session_.AccessRevision() ||
            (session.Generation() == session_.Generation() &&
             (session.Subject() != session_.Subject() || session.Phase() != session_.Phase())))
            return Result<void>::Failure(MakeError(PlatformSessionErrors::StaleSession));
        const bool identical = session.Generation() == session_.Generation() && session.AccessRevision() == session_.AccessRevision() &&
                               session.Capabilities().services == session_.Capabilities().services && session.Reason() == session_.Reason();
        if (identical)
            return Result<void>::Success();
        if (session.AccessRevision() <= session_.AccessRevision())
            return Result<void>::Failure(MakeError(PlatformSessionErrors::StaleAccessPolicy));
        pending_.reset();
        page_.reset();
        capturedAt_.reset();
        session_ = std::move(session);
        return Result<void>::Success();
    }

    /** @copydoc PlatformFriendsCoordinator::Close */
    Result<void> PlatformFriendsCoordinator::Close() noexcept {
        closed_ = true;
        pending_.reset();
        page_.reset();
        capturedAt_.reset();
        return Result<void>::Success();
    }
}  // namespace Horo::PlatformServices
