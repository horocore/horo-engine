#include "Horo/PlatformServices/PlatformAchievementCoordinator.h"

#include <algorithm>
#include <limits>
#include <new>
#include <utility>

namespace Horo::PlatformServices {
    namespace {
        const ErrorDomainId Domain{"horo.platform.achievement"};

        [[nodiscard]] Result<void> Failure(const ErrorCodeDescriptor &descriptor) {
            return Result<void>::Failure(MakeError(descriptor));
        }

        enum class LedgerState : std::uint8_t {
            Pending,
            InFlight,
            Succeeded,
            Failed,
            Cancelled
        };
    }  // namespace

    namespace AchievementCoordinatorErrors {
        const ErrorCodeDescriptor InvalidConfiguration{Domain,
                                                       ErrorCode{"platform.achievement.invalid_configuration"},
                                                       ErrorSeverity::Error,
                                                       "The achievement coordinator configuration is invalid.",
                                                       "Use a complete registry and finite ledger capacities.",
                                                       false,
                                                       false};
        const ErrorCodeDescriptor InvalidRequest{Domain,
                                                 ErrorCode{"platform.achievement.invalid_request"},
                                                 ErrorSeverity::Error,
                                                 "The achievement request is malformed.",
                                                 "Provide valid typed identity, subject, authority and mutation evidence.",
                                                 false,
                                                 false};
        const ErrorCodeDescriptor UnknownAchievement{Domain,
                                                     ErrorCode{"platform.achievement.unknown"},
                                                     ErrorSeverity::Error,
                                                     "The achievement is not registered in the immutable project registry.",
                                                     "Resolve an active authored achievement identity before submission.",
                                                     false,
                                                     false};
        const ErrorCodeDescriptor AuthorityDenied{Domain,
                                                  ErrorCode{"platform.achievement.authority_denied"},
                                                  ErrorSeverity::Error,
                                                  "The achievement mutation authority is not permitted.",
                                                  "Submit through the semantic authority selected by the registered definition.",
                                                  false,
                                                  false};
        const ErrorCodeDescriptor InvalidProgress{Domain,
                                                  ErrorCode{"platform.achievement.progress_invalid"},
                                                  ErrorSeverity::Error,
                                                  "The achievement mutation contradicts its registered progress schema.",
                                                  "Use the registered mutation kind and a progress value within its total.",
                                                  false,
                                                  false};
        const ErrorCodeDescriptor IdempotencyConflict{Domain,
                                                      ErrorCode{"platform.achievement.idempotency_conflict"},
                                                      ErrorSeverity::Error,
                                                      "An achievement mutation ID was reused with different content.",
                                                      "Allocate a new logical mutation identity for different content.",
                                                      false,
                                                      false};
        const ErrorCodeDescriptor CapacityExceeded{Domain,
                                                   ErrorCode{"platform.achievement.capacity_exceeded"},
                                                   ErrorSeverity::Error,
                                                   "The bounded achievement coordinator is full.",
                                                   "Drain provider work or apply the product capacity policy.",
                                                   true,
                                                   false};
        const ErrorCodeDescriptor Closed{Domain,
                                         ErrorCode{"platform.achievement.closed"},
                                         ErrorSeverity::Error,
                                         "The achievement coordinator is closed.",
                                         "Compose a new coordinator for the current session.",
                                         false,
                                         true};
        const ErrorCodeDescriptor StalePublication{Domain,
                                                   ErrorCode{"platform.achievement.stale_publication"},
                                                   ErrorSeverity::Error,
                                                   "The achievement publication belongs to an obsolete session or request.",
                                                   "Discard the completion and reconcile through the current session.",
                                                   false,
                                                   false};
        const ErrorCodeDescriptor InvalidState{Domain,
                                               ErrorCode{"platform.achievement.state_invalid"},
                                               ErrorSeverity::Error,
                                               "The provider returned malformed achievement state.",
                                               "Reject the response and inspect the provider adapter contract.",
                                               false,
                                               false};
        const ErrorCodeDescriptor StaleState{Domain,
                                             ErrorCode{"platform.achievement.state_stale"},
                                             ErrorSeverity::Error,
                                             "The provider state belongs to an obsolete session or access policy.",
                                             "Discard it and query the current session generation.",
                                             false,
                                             false};
    }  // namespace AchievementCoordinatorErrors

    struct PlatformAchievementCoordinator::LedgerEntry final {
        PlatformAchievementMutationRequest request;
        std::uint64_t sequence{};
        LedgerState state{LedgerState::Pending};
    };

    bool PlatformAchievementMutationId::IsValid() const noexcept {
        return std::ranges::any_of(bytes, [](const std::byte byte) {
            return byte != std::byte{};
        });
    }

    /** @copydoc PlatformAchievementCoordinator::Create */
    Result<PlatformAchievementCoordinator> PlatformAchievementCoordinator::Create(
        std::shared_ptr<const AchievementDefinitionRegistry> registry, PlatformSessionSnapshot session,
        const PlatformAchievementCoordinatorConfig config) {
        if (!registry || config.maximumPendingMutations == 0 ||
            config.maximumPendingMutations > MaximumPlatformAchievementPendingMutations || config.maximumLedgerEntries == 0 ||
            config.maximumLedgerEntries > MaximumPlatformAchievementMutationLedger ||
            config.maximumPendingMutations > config.maximumLedgerEntries)
            return Result<PlatformAchievementCoordinator>::Failure(MakeError(AchievementCoordinatorErrors::InvalidConfiguration));
        try {
            return Result<PlatformAchievementCoordinator>::Success(
                PlatformAchievementCoordinator{std::move(registry), std::move(session), config});
        } catch (const std::bad_alloc &) {
            return Result<PlatformAchievementCoordinator>::Failure(MakeError(AchievementCoordinatorErrors::CapacityExceeded));
        }
    }

    PlatformAchievementCoordinator::PlatformAchievementCoordinator(std::shared_ptr<const AchievementDefinitionRegistry> registry,
                                                                   PlatformSessionSnapshot session,
                                                                   const PlatformAchievementCoordinatorConfig config)
        : registry_(std::move(registry)), session_(std::move(session)), config_(config) {
        ledger_.reserve(config_.maximumLedgerEntries);
    }

    PlatformAchievementCoordinator::~PlatformAchievementCoordinator() {
        static_cast<void>(Close());
    }

    PlatformAchievementCoordinator::PlatformAchievementCoordinator(PlatformAchievementCoordinator &&other) noexcept
        : registry_(std::move(other.registry_)), session_(std::move(other.session_)), config_(other.config_),
          ledger_(std::move(other.ledger_)), pending_(std::move(other.pending_)), inFlight_(std::move(other.inFlight_)),
          nextSequence_(other.nextSequence_), nextQuerySequence_(other.nextQuerySequence_), closed_(std::exchange(other.closed_, true)) {}

    PlatformAchievementCoordinator &PlatformAchievementCoordinator::operator=(PlatformAchievementCoordinator &&other) noexcept {
        if (this != &other) {
            static_cast<void>(Close());
            registry_ = std::move(other.registry_);
            session_ = std::move(other.session_);
            config_ = other.config_;
            ledger_ = std::move(other.ledger_);
            pending_ = std::move(other.pending_);
            inFlight_ = std::move(other.inFlight_);
            nextSequence_ = other.nextSequence_;
            nextQuerySequence_ = other.nextQuerySequence_;
            closed_ = std::exchange(other.closed_, true);
        }
        return *this;
    }

    Result<const AchievementDefinition *> PlatformAchievementCoordinator::FindDefinition(const AchievementId id) const {
        if (!id.IsValid())
            return Result<const AchievementDefinition *>::Failure(MakeError(AchievementCoordinatorErrors::InvalidRequest));
        const auto definition = registry_->Find(id);
        if (definition.HasError())
            return Result<const AchievementDefinition *>::Failure(MakeError(AchievementCoordinatorErrors::UnknownAchievement));
        return definition;
    }

    /** @copydoc PlatformAchievementCoordinator::ValidateRequest */
    Result<void> PlatformAchievementCoordinator::ValidateRequest(const PlatformAchievementMutationRequest &request) const {
        if (!request.mutation.IsValid())
            return Failure(AchievementCoordinatorErrors::InvalidRequest);
        if (const auto access =
                ValidatePlatformSessionAccess(session_, request.subject, request.accessRevision, PlatformServiceKind::Achievements);
            access.HasError())
            return access;
        const auto definition = FindDefinition(request.achievement);
        if (definition.HasError())
            return Result<void>::Failure(definition.ErrorValue());
        const auto &expected = *definition.Value();
        if (request.authority != expected.authority)
            return Failure(AchievementCoordinatorErrors::AuthorityDenied);
        if (request.kind != expected.progress.kind)
            return Failure(AchievementCoordinatorErrors::InvalidProgress);
        if (request.kind == AchievementProgressKind::UnlockOnce && request.progress != 1)
            return Failure(AchievementCoordinatorErrors::InvalidProgress);
        if (request.kind == AchievementProgressKind::SetProgressMaximum &&
            (request.progress == 0 || request.progress > expected.progress.total))
            return Failure(AchievementCoordinatorErrors::InvalidProgress);
        return Result<void>::Success();
    }

    /** @copydoc PlatformAchievementCoordinator::ValidateQuery */
    Result<void> PlatformAchievementCoordinator::ValidateQuery(const PlatformAchievementStateQueryRequest &request) const {
        if (const auto access =
                ValidatePlatformSessionAccess(session_, request.subject, request.accessRevision, PlatformServiceKind::Achievements);
            access.HasError())
            return access;
        const auto definition = FindDefinition(request.achievement);
        return definition.HasError() ? Result<void>::Failure(definition.ErrorValue()) : Result<void>::Success();
    }

    bool PlatformAchievementCoordinator::SameMutation(const PlatformAchievementMutationRequest &left,
                                                      const PlatformAchievementMutationRequest &right) noexcept {
        return left.subject == right.subject && left.accessRevision == right.accessRevision && left.achievement == right.achievement &&
               left.authority == right.authority && left.kind == right.kind && left.progress == right.progress &&
               left.mutation == right.mutation;
    }

    bool PlatformAchievementCoordinator::SameSessionAuthority(const PlatformSessionSnapshot &left,
                                                              const PlatformSessionSnapshot &right) noexcept {
        return left.Phase() == right.Phase() && left.Generation() == right.Generation() &&
               left.ProviderGeneration() == right.ProviderGeneration() && left.AccessRevision() == right.AccessRevision() &&
               left.Subject() == right.Subject() && left.Reason() == right.Reason() &&
               left.Capabilities().services == right.Capabilities().services;
    }

    PlatformAchievementCoordinator::LedgerEntry *PlatformAchievementCoordinator::FindLedger(
        const PlatformAchievementMutationId id) noexcept {
        const auto found = std::ranges::find_if(ledger_, [id](const LedgerEntry &entry) {
            return entry.request.mutation == id;
        });
        return found == ledger_.end() ? nullptr : std::to_address(found);
    }

    /** @copydoc PlatformAchievementCoordinator::SubmitMutation */
    Result<PlatformAchievementMutationAdmission> PlatformAchievementCoordinator::SubmitMutation(
        const PlatformAchievementMutationRequest &request) {
        if (closed_)
            return Result<PlatformAchievementMutationAdmission>::Failure(MakeError(AchievementCoordinatorErrors::Closed));
        if (const auto valid = ValidateRequest(request); valid.HasError())
            return Result<PlatformAchievementMutationAdmission>::Failure(valid.ErrorValue());
        if (const auto *existing = FindLedger(request.mutation); existing != nullptr) {
            if (SameMutation(existing->request, request))
                return Result<PlatformAchievementMutationAdmission>::Success(PlatformAchievementMutationAdmission::IgnoredDuplicate);
            return Result<PlatformAchievementMutationAdmission>::Failure(MakeError(AchievementCoordinatorErrors::IdempotencyConflict));
        }
        if (ledger_.size() >= config_.maximumLedgerEntries || pending_.size() >= config_.maximumPendingMutations)
            return Result<PlatformAchievementMutationAdmission>::Failure(MakeError(AchievementCoordinatorErrors::CapacityExceeded));
        if (nextSequence_ == std::numeric_limits<std::uint64_t>::max())
            return Result<PlatformAchievementMutationAdmission>::Failure(MakeError(AchievementCoordinatorErrors::CapacityExceeded));

        const std::uint64_t sequence = nextSequence_;
        const PlatformAchievementMutationPublication publication{.request = request,
                                                                 .sessionGeneration = session_.Generation(),
                                                                 .sequence = sequence};
        bool ledgerInserted = false;
        try {
            ledger_.push_back({.request = request, .sequence = sequence, .state = LedgerState::Pending});
            ledgerInserted = true;
            pending_.push_back(publication);
        } catch (const std::bad_alloc &) {
            if (ledgerInserted)
                ledger_.pop_back();
            return Result<PlatformAchievementMutationAdmission>::Failure(MakeError(AchievementCoordinatorErrors::CapacityExceeded));
        }
        ++nextSequence_;
        return Result<PlatformAchievementMutationAdmission>::Success(PlatformAchievementMutationAdmission::Queued);
    }

    /** @copydoc PlatformAchievementCoordinator::TakeNextMutation */
    Result<std::optional<PlatformAchievementMutationPublication>> PlatformAchievementCoordinator::TakeNextMutation() {
        if (closed_)
            return Result<std::optional<PlatformAchievementMutationPublication>>::Failure(MakeError(AchievementCoordinatorErrors::Closed));
        if (inFlight_ || pending_.empty())
            return Result<std::optional<PlatformAchievementMutationPublication>>::Success(std::nullopt);

        inFlight_ = std::move(pending_.front());
        pending_.pop_front();
        if (auto *entry = FindLedger(inFlight_->request.mutation); entry != nullptr)
            entry->state = LedgerState::InFlight;
        return Result<std::optional<PlatformAchievementMutationPublication>>::Success(*inFlight_);
    }

    /** @copydoc PlatformAchievementCoordinator::CompleteMutation */
    Result<void> PlatformAchievementCoordinator::CompleteMutation(const PlatformAchievementMutationPublication &publication,
                                                                  const PlatformAchievementPublicationOutcome outcome) {
        if (closed_)
            return Failure(AchievementCoordinatorErrors::Closed);
        if (!inFlight_ || inFlight_->sequence != publication.sequence || !SameMutation(inFlight_->request, publication.request))
            return Failure(AchievementCoordinatorErrors::StalePublication);
        auto *entry = FindLedger(publication.request.mutation);
        if (entry == nullptr)
            return Failure(AchievementCoordinatorErrors::StalePublication);
        switch (outcome) {
            case PlatformAchievementPublicationOutcome::Succeeded:
                entry->state = LedgerState::Succeeded;
                break;
            case PlatformAchievementPublicationOutcome::Failed:
                entry->state = LedgerState::Failed;
                break;
            case PlatformAchievementPublicationOutcome::Cancelled:
                entry->state = LedgerState::Cancelled;
                break;
        }
        inFlight_.reset();
        return Result<void>::Success();
    }

    /** @copydoc PlatformAchievementCoordinator::MakeStateQuery */
    Result<PlatformAchievementQueryIntent> PlatformAchievementCoordinator::MakeStateQuery(
        const PlatformAchievementStateQueryRequest &request) {
        if (closed_)
            return Result<PlatformAchievementQueryIntent>::Failure(MakeError(AchievementCoordinatorErrors::Closed));
        if (const auto valid = ValidateQuery(request); valid.HasError())
            return Result<PlatformAchievementQueryIntent>::Failure(valid.ErrorValue());
        if (nextQuerySequence_ == std::numeric_limits<std::uint64_t>::max())
            return Result<PlatformAchievementQueryIntent>::Failure(MakeError(AchievementCoordinatorErrors::CapacityExceeded));
        return Result<PlatformAchievementQueryIntent>::Success({.request = request,
                                                                .providerGeneration = session_.ProviderGeneration(),
                                                                .sessionGeneration = session_.Generation(),
                                                                .sequence = nextQuerySequence_++});
    }

    /** @copydoc PlatformAchievementCoordinator::ValidateStateResult */
    Result<void> PlatformAchievementCoordinator::ValidateStateResult(const PlatformAchievementQueryIntent &query,
                                                                     const PlatformAchievementStateSnapshot &state) const {
        if (closed_)
            return Failure(AchievementCoordinatorErrors::Closed);
        if (query.providerGeneration != session_.ProviderGeneration() || query.sessionGeneration != session_.Generation() ||
            query.request.accessRevision != session_.AccessRevision() || !query.request.subject.IsValid())
            return Failure(AchievementCoordinatorErrors::StaleState);
        if (const auto access = ValidatePlatformSessionAccess(session_, query.request.subject, query.request.accessRevision,
                                                              PlatformServiceKind::Achievements);
            access.HasError())
            return Failure(AchievementCoordinatorErrors::StaleState);
        const auto definition = FindDefinition(query.request.achievement);
        if (definition.HasError())
            return Result<void>::Failure(definition.ErrorValue());
        if (!state.subject.IsValid() || state.subject != query.request.subject)
            return Failure(AchievementCoordinatorErrors::StaleState);
        if (state.achievement != query.request.achievement || state.providerGeneration != query.providerGeneration ||
            state.sessionGeneration != query.sessionGeneration || state.accessRevision != query.request.accessRevision ||
            state.providerRevision == 0 || state.progress > definition.Value()->progress.total ||
            state.unlocked != (state.progress == definition.Value()->progress.total))
            return Failure(AchievementCoordinatorErrors::InvalidState);
        return Result<void>::Success();
    }

    /** @copydoc PlatformAchievementCoordinator::UpdateSession */
    Result<void> PlatformAchievementCoordinator::UpdateSession(PlatformSessionSnapshot session) {
        if (closed_)
            return Failure(AchievementCoordinatorErrors::Closed);
        if (session.Generation() < session_.Generation() || session.AccessRevision() < session_.AccessRevision() ||
            (session.Generation() == session_.Generation() && session.ProviderGeneration() != session_.ProviderGeneration()))
            return Failure(PlatformSessionErrors::StaleSession);
        if (SameSessionAuthority(session_, session))
            return Result<void>::Success();
        session_ = std::move(session);
        ledger_.clear();
        pending_.clear();
        inFlight_.reset();
        return Result<void>::Success();
    }

    /** @copydoc PlatformAchievementCoordinator::Close */
    Result<void> PlatformAchievementCoordinator::Close() noexcept {
        if (closed_)
            return Result<void>::Success();
        closed_ = true;
        ledger_.clear();
        pending_.clear();
        inFlight_.reset();
        return Result<void>::Success();
    }

    bool PlatformAchievementCoordinator::IsClosed() const noexcept {
        return closed_;
    }

    std::size_t PlatformAchievementCoordinator::PendingCount() const noexcept {
        return pending_.size();
    }

    bool PlatformAchievementCoordinator::HasInFlight() const noexcept {
        return inFlight_.has_value();
    }
}  // namespace Horo::PlatformServices
