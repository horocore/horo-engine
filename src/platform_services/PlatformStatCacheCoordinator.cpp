#include "Horo/PlatformServices/PlatformStatCacheCoordinator.h"

#include <algorithm>
#include <limits>
#include <new>
#include <utility>

namespace Horo::PlatformServices {
    namespace {
        const ErrorDomainId Domain{"horo.platform.stat"};

        [[nodiscard]] Result<void> Failure(const ErrorCodeDescriptor &descriptor) {
            return Result<void>::Failure(MakeError(descriptor));
        }

        [[nodiscard]] bool IsKnown(const ProgressionValueKind value) noexcept {
            return value == ProgressionValueKind::SignedInteger64 || value == ProgressionValueKind::UnsignedInteger64;
        }

        [[nodiscard]] bool IsKnown(const ProgressionAuthorityMode value) noexcept {
            return value == ProgressionAuthorityMode::LocalProduct || value == ProgressionAuthorityMode::AuthorityServer;
        }

        [[nodiscard]] bool IsKnown(const StatMutationPolicy value) noexcept {
            return value >= StatMutationPolicy::SetMaximum && value <= StatMutationPolicy::AddOnce;
        }

        [[nodiscard]] bool IsZero(const Sha256Digest &digest) noexcept {
            return digest.bytes == std::array<std::uint8_t, 32>{};
        }

        [[nodiscard]] std::uint64_t AddSaturating(const std::uint64_t value, const std::uint64_t increment) noexcept {
            if (increment > std::numeric_limits<std::uint64_t>::max() - value)
                return std::numeric_limits<std::uint64_t>::max();
            return value + increment;
        }

        [[nodiscard]] bool Fits(const PlatformStatValue &value, const StatDefinition &definition) noexcept {
            if (value.kind != definition.valueKind)
                return false;
            if (value.kind == ProgressionValueKind::SignedInteger64)
                return value.signedValue >= definition.range.minimum && value.signedValue <= definition.range.maximum;
            const auto minimum = static_cast<std::uint64_t>(definition.range.minimum);
            const auto maximum = static_cast<std::uint64_t>(definition.range.maximum);
            return value.unsignedValue >= minimum && value.unsignedValue <= maximum;
        }

        enum class LedgerState : std::uint8_t {
            Pending,
            InFlight,
            Succeeded,
            Failed,
            Cancelled
        };
    }  // namespace

    namespace StatCoordinatorErrors {
        const ErrorCodeDescriptor InvalidConfiguration{Domain,
                                                       ErrorCode{"platform.stat.invalid_configuration"},
                                                       ErrorSeverity::Error,
                                                       "The stat cache coordinator configuration is invalid.",
                                                       "Use finite nonzero cache, queue, ledger, and freshness limits.",
                                                       false,
                                                       false};
        const ErrorCodeDescriptor InvalidRequest{Domain,
                                                 ErrorCode{"platform.stat.invalid_request"},
                                                 ErrorSeverity::Error,
                                                 "The stat request is malformed.",
                                                 "Provide a valid typed subject, stat, value, and mutation envelope.",
                                                 false,
                                                 false};
        const ErrorCodeDescriptor UnknownStat{Domain,
                                              ErrorCode{"platform.stat.unknown"},
                                              ErrorSeverity::Error,
                                              "The stat is not registered in the immutable project registry.",
                                              "Resolve an active authored stat identity before admission.",
                                              false,
                                              false};
        const ErrorCodeDescriptor AuthorityDenied{Domain,
                                                  ErrorCode{"platform.stat.authority_denied"},
                                                  ErrorSeverity::Error,
                                                  "The stat write authority is not permitted.",
                                                  "Submit through the authority selected by the registered definition.",
                                                  false,
                                                  false};
        const ErrorCodeDescriptor InvalidValue{Domain,
                                               ErrorCode{"platform.stat.value_invalid"},
                                               ErrorSeverity::Error,
                                               "The stat value contradicts its registered numeric schema.",
                                               "Use the registered representation and inclusive value range.",
                                               false,
                                               false};
        const ErrorCodeDescriptor RevisionRequired{Domain,
                                                   ErrorCode{"platform.stat.revision_required"},
                                                   ErrorSeverity::Error,
                                                   "The stat operation requires an exact provider revision.",
                                                   "Supply a nonzero revision captured from the authoritative query.",
                                                   false,
                                                   false};
        const ErrorCodeDescriptor IdempotencyConflict{Domain,
                                                      ErrorCode{"platform.stat.idempotency_conflict"},
                                                      ErrorSeverity::Error,
                                                      "A stat mutation ID was reused with different content.",
                                                      "Allocate a new logical mutation identity for different content.",
                                                      false,
                                                      false};
        const ErrorCodeDescriptor CapacityExceeded{Domain,
                                                   ErrorCode{"platform.stat.capacity_exceeded"},
                                                   ErrorSeverity::Error,
                                                   "The bounded stat coordinator cannot retain more work or cache state.",
                                                   "Drain provider work or apply the product capacity policy.",
                                                   true,
                                                   false};
        const ErrorCodeDescriptor Closed{Domain,
                                         ErrorCode{"platform.stat.closed"},
                                         ErrorSeverity::Error,
                                         "The stat cache coordinator is closed.",
                                         "Compose a new coordinator for the current session.",
                                         false,
                                         true};
        const ErrorCodeDescriptor StalePublication{Domain,
                                                   ErrorCode{"platform.stat.stale_publication"},
                                                   ErrorSeverity::Error,
                                                   "The stat publication belongs to an obsolete session or request.",
                                                   "Discard the completion and reconcile through the current session.",
                                                   false,
                                                   false};
        const ErrorCodeDescriptor InvalidState{Domain,
                                               ErrorCode{"platform.stat.state_invalid"},
                                               ErrorSeverity::Error,
                                               "The provider returned malformed stat state.",
                                               "Reject the response and inspect the provider adapter contract.",
                                               false,
                                               false};
        const ErrorCodeDescriptor StaleState{Domain,
                                             ErrorCode{"platform.stat.state_stale"},
                                             ErrorSeverity::Error,
                                             "The provider stat state belongs to an obsolete session or access policy.",
                                             "Discard it and query the current session generation.",
                                             false,
                                             false};
        const ErrorCodeDescriptor CacheCorrupt{Domain,
                                               ErrorCode{"platform.stat.cache_corrupt"},
                                               ErrorSeverity::Error,
                                               "A restored stat cache record is corrupt or belongs to another namespace.",
                                               "Quarantine the record and perform an explicit provider read.",
                                               false,
                                               false};
    }  // namespace StatCoordinatorErrors

    struct PlatformStatCacheCoordinator::LedgerEntry final {
        PlatformStatWriteRequest request;
        std::uint64_t sequence{};
        LedgerState state{LedgerState::Pending};
    };

    bool PlatformStatMutationId::IsValid() const noexcept {
        for (const std::byte byte : bytes) {
            if (byte != std::byte{})
                return true;
        }
        return false;
    }

    PlatformStatValue PlatformStatValue::FromSigned(const std::int64_t value) noexcept {
        return {.kind = ProgressionValueKind::SignedInteger64, .signedValue = value, .unsignedValue = 0};
    }

    PlatformStatValue PlatformStatValue::FromUnsigned(const std::uint64_t value) noexcept {
        return {.kind = ProgressionValueKind::UnsignedInteger64, .signedValue = 0, .unsignedValue = value};
    }

    bool PlatformStatValue::IsValid() const noexcept {
        if (kind == ProgressionValueKind::SignedInteger64)
            return unsignedValue == 0;
        if (kind == ProgressionValueKind::UnsignedInteger64)
            return signedValue == 0;
        return false;
    }

    /** @copydoc PlatformStatCacheCoordinator::Create */
    Result<PlatformStatCacheCoordinator> PlatformStatCacheCoordinator::Create(std::shared_ptr<const StatDefinitionRegistry> registry,
                                                                              PlatformSessionSnapshot session,
                                                                              const PlatformStatCacheCoordinatorConfig config) {
        if (!registry || config.maximumCacheEntries == 0 || config.maximumCacheEntries > MaximumPlatformStatCacheEntries ||
            config.maximumPendingWrites == 0 || config.maximumPendingWrites > MaximumPlatformStatPendingWrites ||
            config.maximumLedgerEntries == 0 || config.maximumLedgerEntries > MaximumPlatformStatMutationLedger ||
            config.maximumPendingWrites > config.maximumLedgerEntries || config.freshnessWindowTicks == 0)
            return Result<PlatformStatCacheCoordinator>::Failure(MakeError(StatCoordinatorErrors::InvalidConfiguration));
        return Result<PlatformStatCacheCoordinator>::Success(PlatformStatCacheCoordinator{std::move(registry), std::move(session), config});
    }

    PlatformStatCacheCoordinator::PlatformStatCacheCoordinator(std::shared_ptr<const StatDefinitionRegistry> registry,
                                                               PlatformSessionSnapshot session,
                                                               const PlatformStatCacheCoordinatorConfig config) noexcept
        : registry_(std::move(registry)), session_(std::move(session)), config_(config) {}

    PlatformStatCacheCoordinator::~PlatformStatCacheCoordinator() {
        static_cast<void>(Close());
    }

    PlatformStatCacheCoordinator::PlatformStatCacheCoordinator(PlatformStatCacheCoordinator &&other) noexcept
        : registry_(std::move(other.registry_)), session_(std::move(other.session_)), config_(other.config_),
          cache_(std::move(other.cache_)), ledger_(std::move(other.ledger_)), pending_(std::move(other.pending_)),
          inFlight_(std::move(other.inFlight_)), nextWriteSequence_(other.nextWriteSequence_), nextQuerySequence_(other.nextQuerySequence_),
          closed_(std::exchange(other.closed_, true)) {}

    PlatformStatCacheCoordinator &PlatformStatCacheCoordinator::operator=(PlatformStatCacheCoordinator &&other) noexcept {
        if (this != &other) {
            static_cast<void>(Close());
            registry_ = std::move(other.registry_);
            session_ = std::move(other.session_);
            config_ = other.config_;
            cache_ = std::move(other.cache_);
            ledger_ = std::move(other.ledger_);
            pending_ = std::move(other.pending_);
            inFlight_ = std::move(other.inFlight_);
            nextWriteSequence_ = other.nextWriteSequence_;
            nextQuerySequence_ = other.nextQuerySequence_;
            closed_ = std::exchange(other.closed_, true);
        }
        return *this;
    }

    Result<const StatDefinition *> PlatformStatCacheCoordinator::FindDefinition(const StatId id) const {
        if (!id.IsValid())
            return Result<const StatDefinition *>::Failure(MakeError(StatCoordinatorErrors::InvalidRequest));
        const auto definition = registry_->Find(id);
        if (definition.HasError())
            return Result<const StatDefinition *>::Failure(MakeError(StatCoordinatorErrors::UnknownStat));
        return definition;
    }

    /** @copydoc PlatformStatCacheCoordinator::ValidateRead */
    Result<void> PlatformStatCacheCoordinator::ValidateRead(const PlatformStatReadRequest &request) const {
        if (const auto access =
                ValidatePlatformSessionAccess(session_, request.subject, request.accessRevision, PlatformServiceKind::LeaderboardsAndStats);
            access.HasError())
            return access;
        const auto definition = FindDefinition(request.stat);
        if (definition.HasError())
            return Result<void>::Failure(definition.ErrorValue());
        return Result<void>::Success();
    }

    /** @copydoc PlatformStatCacheCoordinator::ValidateWrite */
    Result<void> PlatformStatCacheCoordinator::ValidateWrite(const PlatformStatWriteRequest &request) const {
        if (!request.mutation.IsValid() || !request.value.IsValid() || !IsKnown(request.authority))
            return Failure(StatCoordinatorErrors::InvalidRequest);
        if (const auto access =
                ValidatePlatformSessionAccess(session_, request.subject, request.accessRevision, PlatformServiceKind::LeaderboardsAndStats);
            access.HasError())
            return access;
        const auto definition = FindDefinition(request.stat);
        if (definition.HasError())
            return Result<void>::Failure(definition.ErrorValue());
        const auto &expected = *definition.Value();
        if (request.authority != expected.authority)
            return Failure(StatCoordinatorErrors::AuthorityDenied);
        if (!Fits(request.value, expected))
            return Failure(StatCoordinatorErrors::InvalidValue);
        if (!IsKnown(expected.mutation))
            return Failure(StatCoordinatorErrors::InvalidRequest);
        if (expected.mutation == StatMutationPolicy::SnapshotAtRevision) {
            if (!request.expectedProviderRevision || *request.expectedProviderRevision == 0)
                return Failure(StatCoordinatorErrors::RevisionRequired);
        } else if (request.expectedProviderRevision)
            return Failure(StatCoordinatorErrors::InvalidRequest);
        return Result<void>::Success();
    }

    /** @copydoc PlatformStatCacheCoordinator::ValidateState */
    Result<void> PlatformStatCacheCoordinator::ValidateState(const PlatformStatStateEvidence &state, const StatId expectedStat) const {
        if (!state.providerGeneration.IsValid() || !state.sessionGeneration.IsValid() || !state.accessRevision.IsValid() ||
            state.providerRevision == 0 || state.stat != expectedStat || !state.value.IsValid())
            return Failure(StatCoordinatorErrors::InvalidState);
        const auto definition = FindDefinition(state.stat);
        if (definition.HasError())
            return Result<void>::Failure(definition.ErrorValue());
        if (!Fits(state.value, *definition.Value()))
            return Failure(StatCoordinatorErrors::InvalidState);
        return Result<void>::Success();
    }

    /** @copydoc PlatformStatCacheCoordinator::ValidateStateForQuery */
    Result<void> PlatformStatCacheCoordinator::ValidateStateForQuery(const PlatformStatQueryIntent &query,
                                                                     const PlatformStatStateEvidence &state) const {
        const auto currentSubject = session_.Subject();
        if (query.providerGeneration != session_.ProviderGeneration() || query.sessionGeneration != session_.Generation() ||
            query.request.accessRevision != session_.AccessRevision() || !currentSubject || query.request.subject != *currentSubject)
            return Failure(StatCoordinatorErrors::StaleState);
        if (const auto access = ValidatePlatformSessionAccess(session_, query.request.subject, query.request.accessRevision,
                                                              PlatformServiceKind::LeaderboardsAndStats);
            access.HasError())
            return Failure(StatCoordinatorErrors::StaleState);
        if (state.providerGeneration != query.providerGeneration || state.sessionGeneration != query.sessionGeneration ||
            state.accessRevision != query.request.accessRevision)
            return Failure(StatCoordinatorErrors::StaleState);
        return ValidateState(state, query.request.stat);
    }

    /** @copydoc PlatformStatCacheCoordinator::ValidateCacheRecord */
    Result<void> PlatformStatCacheCoordinator::ValidateCacheRecord(const PlatformStatCacheRecord &record) const {
        const auto currentSubject = session_.Subject();
        if (!currentSubject || !record.subject.IsValid() || record.subject != *currentSubject || IsZero(record.definitionFingerprint) ||
            record.definitionFingerprint != registry_->Fingerprint() || record.expiresAtTick == 0)
            return Failure(StatCoordinatorErrors::CacheCorrupt);
        if (const auto state = ValidateState(record.state, record.state.stat); state.HasError())
            return Failure(StatCoordinatorErrors::CacheCorrupt);
        if (record.state.providerGeneration != session_.ProviderGeneration() || record.state.sessionGeneration != session_.Generation() ||
            record.state.accessRevision != session_.AccessRevision())
            return Failure(StatCoordinatorErrors::CacheCorrupt);
        if (const auto access = ValidatePlatformSessionAccess(session_, record.subject, record.state.accessRevision,
                                                              PlatformServiceKind::LeaderboardsAndStats);
            access.HasError())
            return Failure(StatCoordinatorErrors::CacheCorrupt);
        return Result<void>::Success();
    }

    bool PlatformStatCacheCoordinator::SameWrite(const PlatformStatWriteRequest &left, const PlatformStatWriteRequest &right) noexcept {
        return left.subject == right.subject && left.accessRevision == right.accessRevision && left.stat == right.stat &&
               left.authority == right.authority && left.value == right.value &&
               left.expectedProviderRevision == right.expectedProviderRevision && left.mutation == right.mutation;
    }

    bool PlatformStatCacheCoordinator::SameSessionAuthority(const PlatformSessionSnapshot &left,
                                                            const PlatformSessionSnapshot &right) noexcept {
        return left.Phase() == right.Phase() && left.Generation() == right.Generation() &&
               left.ProviderGeneration() == right.ProviderGeneration() && left.AccessRevision() == right.AccessRevision() &&
               left.Subject() == right.Subject() && left.Reason() == right.Reason() &&
               left.Capabilities().services == right.Capabilities().services;
    }

    PlatformStatCacheRecord *PlatformStatCacheCoordinator::FindCache(const PlatformSubjectHandle subject, const StatId stat) noexcept {
        const auto found = std::ranges::find_if(cache_, [subject, stat](const PlatformStatCacheRecord &record) {
            return record.subject == subject && record.state.stat == stat;
        });
        return found == cache_.end() ? nullptr : std::to_address(found);
    }

    const PlatformStatCacheRecord *PlatformStatCacheCoordinator::FindCache(const PlatformSubjectHandle subject,
                                                                           const StatId stat) const noexcept {
        const auto found = std::ranges::find_if(cache_, [subject, stat](const PlatformStatCacheRecord &record) {
            return record.subject == subject && record.state.stat == stat;
        });
        return found == cache_.end() ? nullptr : std::to_address(found);
    }

    PlatformStatCacheCoordinator::LedgerEntry *PlatformStatCacheCoordinator::FindLedger(const PlatformStatMutationId id) noexcept {
        const auto found = std::ranges::find_if(ledger_, [id](const LedgerEntry &entry) {
            return entry.request.mutation == id;
        });
        return found == ledger_.end() ? nullptr : std::to_address(found);
    }

    void PlatformStatCacheCoordinator::PruneExpired(const std::uint64_t observedTick) noexcept {
        const auto retained = std::ranges::remove_if(cache_, [observedTick](const PlatformStatCacheRecord &record) {
            return record.expiresAtTick <= observedTick;
        });
        cache_.erase(retained.begin(), retained.end());
    }

    /** @copydoc PlatformStatCacheCoordinator::UpsertCache */
    Result<void> PlatformStatCacheCoordinator::UpsertCache(PlatformStatCacheRecord record) {
        if (auto *existing = FindCache(record.subject, record.state.stat); existing != nullptr) {
            *existing = std::move(record);
            return Result<void>::Success();
        }
        PruneExpired(record.capturedTick);
        if (cache_.size() >= config_.maximumCacheEntries)
            cache_.erase(cache_.begin());
        try {
            cache_.push_back(std::move(record));
        } catch (const std::bad_alloc &) {
            return Failure(StatCoordinatorErrors::CapacityExceeded);
        }
        return Result<void>::Success();
    }

    /** @copydoc PlatformStatCacheCoordinator::ReadStat */
    Result<PlatformStatReadDecision> PlatformStatCacheCoordinator::ReadStat(PlatformStatReadRequest request) {
        if (closed_)
            return Result<PlatformStatReadDecision>::Failure(MakeError(StatCoordinatorErrors::Closed));
        if (const auto valid = ValidateRead(request); valid.HasError())
            return Result<PlatformStatReadDecision>::Failure(valid.ErrorValue());

        const PlatformStatQueryIntent query{.request = request,
                                            .providerGeneration = session_.ProviderGeneration(),
                                            .sessionGeneration = session_.Generation(),
                                            .definitionFingerprint = registry_->Fingerprint(),
                                            .sequence = nextQuerySequence_++};
        const auto *cached = FindCache(request.subject, request.stat);
        if (cached != nullptr) {
            if (const auto valid = ValidateCacheRecord(*cached); valid.HasError()) {
                cache_.erase(std::ranges::find_if(cache_, [cached](const PlatformStatCacheRecord &record) {
                    return std::addressof(record) == cached;
                }));
                return Result<PlatformStatReadDecision>::Success(
                    {.disposition = PlatformStatReadDisposition::CorruptCacheQuery, .query = query});
            }
            if (cached->expiresAtTick > request.observedTick)
                return Result<PlatformStatReadDecision>::Success(
                    {.disposition = PlatformStatReadDisposition::FreshCacheHit,
                     .cached = PlatformStatCachedState{.state = cached->state, .expiresAtTick = cached->expiresAtTick}});
            return Result<PlatformStatReadDecision>::Success({.disposition = PlatformStatReadDisposition::StaleCacheQuery, .query = query});
        }
        return Result<PlatformStatReadDecision>::Success({.disposition = PlatformStatReadDisposition::ProviderQuery, .query = query});
    }

    /** @copydoc PlatformStatCacheCoordinator::PublishReadResult */
    Result<void> PlatformStatCacheCoordinator::PublishReadResult(const PlatformStatQueryIntent &query,
                                                                 const PlatformStatStateEvidence &state) {
        if (closed_)
            return Failure(StatCoordinatorErrors::Closed);
        if (query.providerGeneration != session_.ProviderGeneration() || query.sessionGeneration != session_.Generation() ||
            query.definitionFingerprint != registry_->Fingerprint())
            return Failure(StatCoordinatorErrors::StaleState);
        if (const auto valid = ValidateStateForQuery(query, state); valid.HasError())
            return valid;
        const PlatformStatCacheRecord record{.subject = query.request.subject,
                                             .state = state,
                                             .definitionFingerprint = query.definitionFingerprint,
                                             .capturedTick = query.request.observedTick,
                                             .expiresAtTick = AddSaturating(query.request.observedTick, config_.freshnessWindowTicks)};
        return UpsertCache(record);
    }

    /** @copydoc PlatformStatCacheCoordinator::RestoreCache */
    Result<void> PlatformStatCacheCoordinator::RestoreCache(std::vector<PlatformStatCacheRecord> records) {
        if (closed_)
            return Failure(StatCoordinatorErrors::Closed);
        if (records.size() > config_.maximumCacheEntries)
            return Failure(StatCoordinatorErrors::CapacityExceeded);
        for (std::size_t index = 0; index < records.size(); ++index) {
            if (const auto valid = ValidateCacheRecord(records[index]); valid.HasError())
                return Failure(StatCoordinatorErrors::CacheCorrupt);
            for (std::size_t prior = 0; prior < index; ++prior) {
                if (records[prior].subject == records[index].subject && records[prior].state.stat == records[index].state.stat)
                    return Failure(StatCoordinatorErrors::CacheCorrupt);
            }
        }
        try {
            cache_ = std::move(records);
        } catch (const std::bad_alloc &) {
            return Failure(StatCoordinatorErrors::CapacityExceeded);
        }
        return Result<void>::Success();
    }

    /** @copydoc PlatformStatCacheCoordinator::SubmitWrite */
    Result<PlatformStatWriteAdmission> PlatformStatCacheCoordinator::SubmitWrite(PlatformStatWriteRequest request) {
        if (closed_)
            return Result<PlatformStatWriteAdmission>::Failure(MakeError(StatCoordinatorErrors::Closed));
        if (const auto valid = ValidateWrite(request); valid.HasError())
            return Result<PlatformStatWriteAdmission>::Failure(valid.ErrorValue());
        if (const auto *existing = FindLedger(request.mutation); existing != nullptr) {
            if (SameWrite(existing->request, request))
                return Result<PlatformStatWriteAdmission>::Success(PlatformStatWriteAdmission::IgnoredDuplicate);
            return Result<PlatformStatWriteAdmission>::Failure(MakeError(StatCoordinatorErrors::IdempotencyConflict));
        }
        if (ledger_.size() >= config_.maximumLedgerEntries || pending_.size() >= config_.maximumPendingWrites)
            return Result<PlatformStatWriteAdmission>::Failure(MakeError(StatCoordinatorErrors::CapacityExceeded));

        const std::uint64_t sequence = nextWriteSequence_++;
        const PlatformStatWritePublication publication{.request = request,
                                                       .sessionGeneration = session_.Generation(),
                                                       .sequence = sequence};
        try {
            ledger_.push_back({.request = request, .sequence = sequence, .state = LedgerState::Pending});
            try {
                pending_.push_back(publication);
            } catch (const std::bad_alloc &) {
                ledger_.pop_back();
                return Result<PlatformStatWriteAdmission>::Failure(MakeError(StatCoordinatorErrors::CapacityExceeded));
            }
        } catch (const std::bad_alloc &) {
            return Result<PlatformStatWriteAdmission>::Failure(MakeError(StatCoordinatorErrors::CapacityExceeded));
        }
        return Result<PlatformStatWriteAdmission>::Success(PlatformStatWriteAdmission::Queued);
    }

    /** @copydoc PlatformStatCacheCoordinator::TakeNextWrite */
    Result<std::optional<PlatformStatWritePublication>> PlatformStatCacheCoordinator::TakeNextWrite() {
        if (closed_)
            return Result<std::optional<PlatformStatWritePublication>>::Failure(MakeError(StatCoordinatorErrors::Closed));
        if (inFlight_ || pending_.empty())
            return Result<std::optional<PlatformStatWritePublication>>::Success(std::nullopt);
        inFlight_ = std::move(pending_.front());
        pending_.erase(pending_.begin());
        if (auto *entry = FindLedger(inFlight_->request.mutation); entry != nullptr)
            entry->state = LedgerState::InFlight;
        return Result<std::optional<PlatformStatWritePublication>>::Success(*inFlight_);
    }

    /** @copydoc PlatformStatCacheCoordinator::CompleteWrite */
    Result<void> PlatformStatCacheCoordinator::CompleteWrite(const PlatformStatWritePublication &publication,
                                                             const PlatformStatWriteOutcome outcome,
                                                             std::optional<PlatformStatStateEvidence> state) {
        if (closed_)
            return Failure(StatCoordinatorErrors::Closed);
        if (!inFlight_ || inFlight_->sequence != publication.sequence || !SameWrite(inFlight_->request, publication.request))
            return Failure(StatCoordinatorErrors::StalePublication);
        auto *entry = FindLedger(publication.request.mutation);
        if (entry == nullptr)
            return Failure(StatCoordinatorErrors::StalePublication);

        if (outcome == PlatformStatWriteOutcome::Succeeded) {
            if (!state || state->providerGeneration != session_.ProviderGeneration() || state->sessionGeneration != session_.Generation() ||
                state->accessRevision != session_.AccessRevision()) {
                entry->state = LedgerState::Failed;
                inFlight_.reset();
                return Failure(StatCoordinatorErrors::InvalidState);
            }
            if (const auto access = ValidatePlatformSessionAccess(session_, publication.request.subject, publication.request.accessRevision,
                                                                  PlatformServiceKind::LeaderboardsAndStats);
                access.HasError()) {
                entry->state = LedgerState::Failed;
                inFlight_.reset();
                return Failure(StatCoordinatorErrors::StaleState);
            }
            if (const auto valid = ValidateState(*state, publication.request.stat); valid.HasError()) {
                entry->state = LedgerState::Failed;
                inFlight_.reset();
                return valid;
            }
            const PlatformStatCacheRecord record{.subject = publication.request.subject,
                                                 .state = *state,
                                                 .definitionFingerprint = registry_->Fingerprint(),
                                                 .capturedTick = publication.request.observedTick,
                                                 .expiresAtTick =
                                                     AddSaturating(publication.request.observedTick, config_.freshnessWindowTicks)};
            if (const auto cached = UpsertCache(record); cached.HasError()) {
                entry->state = LedgerState::Succeeded;
                inFlight_.reset();
                return cached;
            }
        }

        switch (outcome) {
            case PlatformStatWriteOutcome::Succeeded:
                entry->state = LedgerState::Succeeded;
                break;
            case PlatformStatWriteOutcome::Failed:
                entry->state = LedgerState::Failed;
                break;
            case PlatformStatWriteOutcome::Cancelled:
                entry->state = LedgerState::Cancelled;
                break;
        }
        inFlight_.reset();
        return Result<void>::Success();
    }

    /** @copydoc PlatformStatCacheCoordinator::UpdateSession */
    Result<void> PlatformStatCacheCoordinator::UpdateSession(PlatformSessionSnapshot session) {
        if (closed_)
            return Failure(StatCoordinatorErrors::Closed);
        if (session.Generation() < session_.Generation() || session.AccessRevision() < session_.AccessRevision() ||
            (session.Generation() == session_.Generation() && session.ProviderGeneration() != session_.ProviderGeneration()))
            return Failure(PlatformSessionErrors::StaleSession);
        if (SameSessionAuthority(session_, session))
            return Result<void>::Success();
        session_ = std::move(session);
        cache_.clear();
        ledger_.clear();
        pending_.clear();
        inFlight_.reset();
        return Result<void>::Success();
    }

    /** @copydoc PlatformStatCacheCoordinator::Close */
    Result<void> PlatformStatCacheCoordinator::Close() noexcept {
        if (closed_)
            return Result<void>::Success();
        closed_ = true;
        cache_.clear();
        ledger_.clear();
        pending_.clear();
        inFlight_.reset();
        return Result<void>::Success();
    }

    bool PlatformStatCacheCoordinator::IsClosed() const noexcept {
        return closed_;
    }

    std::size_t PlatformStatCacheCoordinator::PendingWriteCount() const noexcept {
        return pending_.size();
    }

    std::size_t PlatformStatCacheCoordinator::CacheEntryCount() const noexcept {
        return cache_.size();
    }

    bool PlatformStatCacheCoordinator::HasInFlight() const noexcept {
        return inFlight_.has_value();
    }
}  // namespace Horo::PlatformServices
