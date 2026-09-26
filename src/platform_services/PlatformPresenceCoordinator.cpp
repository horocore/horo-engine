#include "Horo/PlatformServices/PlatformPresenceCoordinator.h"

#include "Horo/Foundation/Utf8.h"

#include <algorithm>
#include <utility>

namespace Horo::PlatformServices {
    namespace {
        const ErrorDomainId Domain{"horo.platform.presence"};

        [[nodiscard]] Result<void> Failure(const ErrorCodeDescriptor &descriptor) {
            return Result<void>::Failure(MakeError(descriptor));
        }

        [[nodiscard]] bool SameSessionAuthority(const PlatformSessionSnapshot &left, const PlatformSessionSnapshot &right) noexcept {
            return left.Phase() == right.Phase() && left.Generation() == right.Generation() &&
                   left.ProviderGeneration() == right.ProviderGeneration() && left.AccessRevision() == right.AccessRevision() &&
                   left.Subject() == right.Subject() && left.Reason() == right.Reason() &&
                   left.Capabilities().services == right.Capabilities().services;
        }
    }  // namespace

    namespace PresenceCoordinatorErrors {
        const ErrorCodeDescriptor InvalidConfiguration{Domain,
                                                       ErrorCode{"platform.presence.invalid_configuration"},
                                                       ErrorSeverity::Error,
                                                       "The presence coordinator configuration is invalid.",
                                                       "Use a complete registry and finite detail/rate limits.",
                                                       false,
                                                       false};
        const ErrorCodeDescriptor InvalidRequest{Domain,
                                                 ErrorCode{"platform.presence.invalid_request"},
                                                 ErrorSeverity::Error,
                                                 "The presence request is malformed.",
                                                 "Use a live subject and the current access-policy revision.",
                                                 false,
                                                 false};
        const ErrorCodeDescriptor StatusNotRegistered{Domain,
                                                      ErrorCode{"platform.presence.status_unregistered"},
                                                      ErrorSeverity::Error,
                                                      "The presence status is not registered by the project.",
                                                      "Resolve the authored status through the immutable presence registry.",
                                                      false,
                                                      false};
        const ErrorCodeDescriptor DetailForbidden{Domain,
                                                  ErrorCode{"platform.presence.detail_forbidden"},
                                                  ErrorSeverity::Error,
                                                  "The selected presence status forbids free detail.",
                                                  "Submit an empty detail or select a status that permits detail.",
                                                  false,
                                                  false};
        const ErrorCodeDescriptor DetailInvalidUtf8{Domain,
                                                    ErrorCode{"platform.presence.detail_invalid_utf8"},
                                                    ErrorSeverity::Error,
                                                    "Presence detail is not valid UTF-8.",
                                                    "Submit a complete UTF-8 scalar sequence.",
                                                    false,
                                                    false};
        const ErrorCodeDescriptor DetailTooLarge{Domain,
                                                 ErrorCode{"platform.presence.detail_too_large"},
                                                 ErrorSeverity::Error,
                                                 "Presence detail exceeds its finite bound.",
                                                 "Reduce the detail payload to the registered limit.",
                                                 false,
                                                 false};
        const ErrorCodeDescriptor Closed{Domain,
                                         ErrorCode{"platform.presence.closed"},
                                         ErrorSeverity::Error,
                                         "The presence coordinator is closed.",
                                         "Compose a new coordinator for the current session.",
                                         false,
                                         true};
        const ErrorCodeDescriptor StalePublication{Domain,
                                                   ErrorCode{"platform.presence.stale_publication"},
                                                   ErrorSeverity::Error,
                                                   "The presence publication no longer belongs to the current session.",
                                                   "Discard the completion and capture the current desired state.",
                                                   false,
                                                   false};
    }  // namespace PresenceCoordinatorErrors

    /** @copydoc PlatformPresenceCoordinator::Create */
    Result<PlatformPresenceCoordinator> PlatformPresenceCoordinator::Create(std::shared_ptr<const PresenceDefinitionRegistry> registry,
                                                                            PlatformSessionSnapshot session,
                                                                            const PlatformPresenceCoordinatorConfig config) {
        if (!registry || config.maximumDetailBytes == 0 || config.maximumDetailBytes > MaximumPlatformPresenceDetailBytes ||
            config.minimumInterval.count() < 0 || config.minimumInterval > MaximumPlatformPresenceInterval)
            return Result<PlatformPresenceCoordinator>::Failure(MakeError(PresenceCoordinatorErrors::InvalidConfiguration));
        return Result<PlatformPresenceCoordinator>::Success(PlatformPresenceCoordinator{std::move(registry), std::move(session), config});
    }

    PlatformPresenceCoordinator::PlatformPresenceCoordinator(std::shared_ptr<const PresenceDefinitionRegistry> registry,
                                                             PlatformSessionSnapshot session,
                                                             const PlatformPresenceCoordinatorConfig config) noexcept
        : registry_(std::move(registry)), session_(std::move(session)), config_(config) {}

    PlatformPresenceCoordinator::~PlatformPresenceCoordinator() {
        static_cast<void>(Close());
    }

    PlatformPresenceCoordinator::PlatformPresenceCoordinator(PlatformPresenceCoordinator &&other) noexcept
        : registry_(std::move(other.registry_)), session_(std::move(other.session_)), config_(other.config_),
          pending_(std::move(other.pending_)), inFlight_(std::move(other.inFlight_)), lastCompletedAt_(other.lastCompletedAt_),
          nextSequence_(other.nextSequence_), closed_(std::exchange(other.closed_, true)) {}

    PlatformPresenceCoordinator &PlatformPresenceCoordinator::operator=(PlatformPresenceCoordinator &&other) noexcept {
        if (this != &other) {
            static_cast<void>(Close());
            registry_ = std::move(other.registry_);
            session_ = std::move(other.session_);
            config_ = other.config_;
            pending_ = std::move(other.pending_);
            inFlight_ = std::move(other.inFlight_);
            lastCompletedAt_ = other.lastCompletedAt_;
            nextSequence_ = other.nextSequence_;
            closed_ = std::exchange(other.closed_, true);
        }
        return *this;
    }

    /** @copydoc PlatformPresenceCoordinator::ValidateSet */
    Result<void> PlatformPresenceCoordinator::ValidateSet(const PlatformPresenceSetRequest &request) const {
        if (const auto access =
                ValidatePlatformSessionAccess(session_, request.subject, request.accessRevision, PlatformServiceKind::Presence);
            access.HasError())
            return access;

        const auto definition = registry_->Find(request.status);
        if (definition.HasError())
            return Failure(PresenceCoordinatorErrors::StatusNotRegistered);
        const auto &presence = *definition.Value();
        if (presence.detailPolicy == PresenceDetailPolicy::Forbidden && !request.detail.empty())
            return Failure(PresenceCoordinatorErrors::DetailForbidden);
        if (const auto maximumDetailBytes = std::min<std::size_t>(config_.maximumDetailBytes, presence.maximumDetailUtf8Bytes);
            request.detail.size() > maximumDetailBytes)
            return Failure(PresenceCoordinatorErrors::DetailTooLarge);
        if (!IsValidUtf8ScalarSequence(request.detail))
            return Failure(PresenceCoordinatorErrors::DetailInvalidUtf8);
        return Result<void>::Success();
    }

    /** @copydoc PlatformPresenceCoordinator::ValidateClear */
    Result<void> PlatformPresenceCoordinator::ValidateClear(const PlatformPresenceClearRequest &request) const {
        return ValidatePlatformSessionAccess(session_, request.subject, request.accessRevision, PlatformServiceKind::Presence);
    }

    PlatformPresenceIntent PlatformPresenceCoordinator::MakeSetIntent(PlatformPresenceSetRequest request) const {
        return {.operation = PlatformPresenceOperation::Set,
                .subject = request.subject,
                .sessionGeneration = session_.Generation(),
                .accessRevision = request.accessRevision,
                .status = request.status,
                .detail = std::move(request.detail),
                .sequence = nextSequence_};
    }

    PlatformPresenceIntent PlatformPresenceCoordinator::MakeClearIntent(const PlatformPresenceClearRequest &request) const {
        return {.operation = PlatformPresenceOperation::Clear,
                .subject = request.subject,
                .sessionGeneration = session_.Generation(),
                .accessRevision = request.accessRevision,
                .status = std::nullopt,
                .detail = {},
                .sequence = nextSequence_};
    }

    bool PlatformPresenceCoordinator::SameIntent(const PlatformPresenceIntent &left, const PlatformPresenceIntent &right) noexcept {
        return left.operation == right.operation && left.subject == right.subject && left.sessionGeneration == right.sessionGeneration &&
               left.accessRevision == right.accessRevision && left.status == right.status && left.detail == right.detail;
    }

    /** @copydoc PlatformPresenceCoordinator::SubmitSet */
    Result<PlatformPresenceAdmission> PlatformPresenceCoordinator::SubmitSet(PlatformPresenceSetRequest request) {
        using enum PlatformPresenceAdmission;
        if (closed_)
            return Result<PlatformPresenceAdmission>::Failure(MakeError(PresenceCoordinatorErrors::Closed));
        if (const auto valid = ValidateSet(request); valid.HasError())
            return Result<PlatformPresenceAdmission>::Failure(valid.ErrorValue());

        auto intent = MakeSetIntent(std::move(request));
        if (pending_ && SameIntent(*pending_, intent))
            return Result<PlatformPresenceAdmission>::Success(IgnoredDuplicate);
        if (!pending_ && inFlight_ && SameIntent(inFlight_->intent, intent))
            return Result<PlatformPresenceAdmission>::Success(IgnoredDuplicate);
        const auto admission = pending_ ? Coalesced : Queued;
        pending_ = std::move(intent);
        ++nextSequence_;
        return Result<PlatformPresenceAdmission>::Success(admission);
    }

    /** @copydoc PlatformPresenceCoordinator::SubmitClear */
    Result<PlatformPresenceAdmission> PlatformPresenceCoordinator::SubmitClear(const PlatformPresenceClearRequest &request) {
        using enum PlatformPresenceAdmission;
        if (closed_)
            return Result<PlatformPresenceAdmission>::Failure(MakeError(PresenceCoordinatorErrors::Closed));
        if (const auto valid = ValidateClear(request); valid.HasError())
            return Result<PlatformPresenceAdmission>::Failure(valid.ErrorValue());

        auto intent = MakeClearIntent(request);
        if (pending_ && SameIntent(*pending_, intent))
            return Result<PlatformPresenceAdmission>::Success(IgnoredDuplicate);
        if (!pending_ && inFlight_ && SameIntent(inFlight_->intent, intent))
            return Result<PlatformPresenceAdmission>::Success(IgnoredDuplicate);
        const auto admission = pending_ ? Coalesced : Queued;
        pending_ = std::move(intent);
        ++nextSequence_;
        return Result<PlatformPresenceAdmission>::Success(admission);
    }

    /** @copydoc PlatformPresenceCoordinator::TakeReady */
    Result<std::optional<PlatformPresencePublication>> PlatformPresenceCoordinator::TakeReady(
        const std::chrono::steady_clock::time_point now) {
        if (closed_)
            return Result<std::optional<PlatformPresencePublication>>::Failure(MakeError(PresenceCoordinatorErrors::Closed));
        if (!pending_ || inFlight_)
            return Result<std::optional<PlatformPresencePublication>>::Success(std::nullopt);
        if (lastCompletedAt_ && now < *lastCompletedAt_ + config_.minimumInterval)
            return Result<std::optional<PlatformPresencePublication>>::Success(std::nullopt);

        inFlight_ = PlatformPresencePublication{.intent = std::move(*pending_)};
        pending_.reset();
        return Result<std::optional<PlatformPresencePublication>>::Success(*inFlight_);
    }

    /** @copydoc PlatformPresenceCoordinator::Complete */
    Result<void> PlatformPresenceCoordinator::Complete(const PlatformPresencePublication &publication,
                                                       const PlatformPresencePublicationOutcome outcome,
                                                       const std::chrono::steady_clock::time_point now) {
        if (closed_)
            return Failure(PresenceCoordinatorErrors::Closed);
        if (!inFlight_ || inFlight_->intent.sequence != publication.intent.sequence || !SameIntent(inFlight_->intent, publication.intent))
            return Failure(PresenceCoordinatorErrors::StalePublication);
        static_cast<void>(outcome);
        inFlight_.reset();
        lastCompletedAt_ = now;
        return Result<void>::Success();
    }

    /** @copydoc PlatformPresenceCoordinator::UpdateSession */
    Result<PlatformPresenceSessionUpdate> PlatformPresenceCoordinator::UpdateSession(PlatformSessionSnapshot session) {
        using enum PlatformPresenceSessionUpdate;
        if (closed_)
            return Result<PlatformPresenceSessionUpdate>::Failure(MakeError(PresenceCoordinatorErrors::Closed));
        if (session.Generation() < session_.Generation() || session.AccessRevision() < session_.AccessRevision() ||
            (session.Generation() == session_.Generation() && session.ProviderGeneration() != session_.ProviderGeneration()))
            return Result<PlatformPresenceSessionUpdate>::Failure(MakeError(PlatformSessionErrors::StaleSession));
        if (SameSessionAuthority(session_, session))
            return Result<PlatformPresenceSessionUpdate>::Success(Unchanged);

        session_ = std::move(session);
        pending_.reset();
        inFlight_.reset();
        lastCompletedAt_.reset();
        const bool usable = session_.Phase() == PlatformSessionPhase::Active && session_.Subject() &&
                            session_.Capabilities().Access(PlatformServiceKind::Presence) == PlatformSessionAccessState::Granted;
        return Result<PlatformPresenceSessionUpdate>::Success(usable ? Replaced : Invalidated);
    }

    /** @copydoc PlatformPresenceCoordinator::Close */
    Result<void> PlatformPresenceCoordinator::Close() noexcept {
        if (closed_)
            return Result<void>::Success();
        closed_ = true;
        pending_.reset();
        inFlight_.reset();
        return Result<void>::Success();
    }

    bool PlatformPresenceCoordinator::IsClosed() const noexcept {
        return closed_;
    }

    bool PlatformPresenceCoordinator::HasPending() const noexcept {
        return pending_.has_value();
    }

    bool PlatformPresenceCoordinator::HasInFlight() const noexcept {
        return inFlight_.has_value();
    }

    std::uint64_t PlatformPresenceCoordinator::LastSequence() const noexcept {
        return nextSequence_ - 1U;
    }
}  // namespace Horo::PlatformServices
