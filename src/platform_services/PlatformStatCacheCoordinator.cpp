#include "Horo/PlatformServices/PlatformStatCacheCoordinator.h"

#include <algorithm>
#include <limits>
#include <new>
#include <utility>

namespace Horo::PlatformServices {
    namespace {
        [[nodiscard]] Result<void> Failure(const ErrorCodeDescriptor &descriptor) {
            return Result<void>::Failure(MakeError(descriptor));
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

    struct PlatformStatCacheCoordinator::LedgerEntry final {
        PlatformStatWriteRequest request;
        std::uint64_t sequence{};
        LedgerState state{LedgerState::Pending};
    };

    bool PlatformStatMutationId::IsValid() const noexcept {
        return std::ranges::any_of(bytes, [](const std::byte byte) {
            return byte != std::byte{};
        });
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
                                                                              const PlatformStatCacheCoordinatorConfig &config) {
        if (!registry || config.maximumCacheEntries == 0 || config.maximumCacheEntries > MaximumPlatformStatCacheEntries ||
            config.maximumPendingWrites == 0 || config.maximumPendingWrites > MaximumPlatformStatPendingWrites ||
            config.maximumLedgerEntries == 0 || config.maximumLedgerEntries > MaximumPlatformStatMutationLedger ||
            config.maximumPendingWrites > config.maximumLedgerEntries || config.freshnessWindowTicks == 0)
            return Result<PlatformStatCacheCoordinator>::Failure(MakeError(StatCoordinatorErrors::InvalidConfiguration));
        try {
            return Result<PlatformStatCacheCoordinator>::Success(
                PlatformStatCacheCoordinator{std::move(registry), std::move(session), config});
        } catch (const std::bad_alloc &) {
            return Result<PlatformStatCacheCoordinator>::Failure(MakeError(StatCoordinatorErrors::CapacityExceeded));
        }
    }

    PlatformStatCacheCoordinator::PlatformStatCacheCoordinator(std::shared_ptr<const StatDefinitionRegistry> registry,
                                                               PlatformSessionSnapshot session,
                                                               const PlatformStatCacheCoordinatorConfig &config)
        : registry_(std::move(registry)), session_(std::move(session)), config_(config) {
        cache_.reserve(config_.maximumCacheEntries);
        ledger_.reserve(config_.maximumLedgerEntries);
    }

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
        if (const auto definition = FindDefinition(request.stat); definition.HasError())
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
            if (!request.expectedProviderRevision.has_value() || *request.expectedProviderRevision == 0)
                return Failure(StatCoordinatorErrors::RevisionRequired);
        } else if (request.expectedProviderRevision.has_value())
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
        if (const auto currentSubject = session_.Subject(); query.providerGeneration != session_.ProviderGeneration() ||
                                                            query.sessionGeneration != session_.Generation() ||
                                                            query.request.accessRevision != session_.AccessRevision() ||
                                                            !currentSubject.has_value() || query.request.subject != *currentSubject)
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
        if (const auto currentSubject = session_.Subject();
            !currentSubject.has_value() || !record.subject.IsValid() || record.subject != *currentSubject ||
            IsZero(record.definitionFingerprint) || record.definitionFingerprint != registry_->Fingerprint() || record.expiresAtTick == 0)
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

    PlatformStatCacheRecord *PlatformStatCacheCoordinator::FindCache(const PlatformSubjectHandle &subject, const StatId stat) noexcept {
        const auto found = std::ranges::find_if(cache_, [&subject, stat](const PlatformStatCacheRecord &record) {
            return record.subject == subject && record.state.stat == stat;
        });
        return found == cache_.end() ? nullptr : std::to_address(found);
    }

    const PlatformStatCacheRecord *PlatformStatCacheCoordinator::FindCache(const PlatformSubjectHandle &subject,
                                                                           const StatId stat) const noexcept {
        const auto found = std::ranges::find_if(cache_, [&subject, stat](const PlatformStatCacheRecord &record) {
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
        try {
            cache_.push_back(std::move(record));
        } catch (const std::bad_alloc &) {
            return Failure(StatCoordinatorErrors::CapacityExceeded);
        }
        if (cache_.size() > config_.maximumCacheEntries)
            cache_.erase(cache_.begin());
        return Result<void>::Success();
    }

    /** @copydoc PlatformStatCacheCoordinator::ReadStat */
    Result<PlatformStatReadDecision> PlatformStatCacheCoordinator::ReadStat(const PlatformStatReadRequest &request) {
        using enum PlatformStatReadDisposition;
        if (closed_)
            return Result<PlatformStatReadDecision>::Failure(MakeError(StatCoordinatorErrors::Closed));
        if (const auto valid = ValidateRead(request); valid.HasError())
            return Result<PlatformStatReadDecision>::Failure(valid.ErrorValue());
        if (nextQuerySequence_ == std::numeric_limits<std::uint64_t>::max())
            return Result<PlatformStatReadDecision>::Failure(MakeError(StatCoordinatorErrors::CapacityExceeded));

        const PlatformStatQueryIntent query{.request = request,
                                            .providerGeneration = session_.ProviderGeneration(),
                                            .sessionGeneration = session_.Generation(),
                                            .definitionFingerprint = registry_->Fingerprint(),
                                            .sequence = nextQuerySequence_++};
        if (const auto *cached = FindCache(request.subject, request.stat); cached != nullptr) {
            if (const auto valid = ValidateCacheRecord(*cached); valid.HasError()) {
                cache_.erase(cache_.begin() + (cached - cache_.data()));
                return Result<PlatformStatReadDecision>::Success({.disposition = CorruptCacheQuery, .query = query});
            }
            if (cached->expiresAtTick > request.observedTick)
                return Result<PlatformStatReadDecision>::Success(
                    {.disposition = FreshCacheHit,
                     .cached = PlatformStatCachedState{.state = cached->state, .expiresAtTick = cached->expiresAtTick}});
            return Result<PlatformStatReadDecision>::Success({.disposition = StaleCacheQuery, .query = query});
        }
        return Result<PlatformStatReadDecision>::Success({.disposition = ProviderQuery, .query = query});
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
        cache_ = std::move(records);
        return Result<void>::Success();
    }

    /** @copydoc PlatformStatCacheCoordinator::SubmitWrite */
    Result<PlatformStatWriteAdmission> PlatformStatCacheCoordinator::SubmitWrite(const PlatformStatWriteRequest &request) {
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
        if (nextWriteSequence_ == std::numeric_limits<std::uint64_t>::max())
            return Result<PlatformStatWriteAdmission>::Failure(MakeError(StatCoordinatorErrors::CapacityExceeded));

        const std::uint64_t sequence = nextWriteSequence_;
        const PlatformStatWritePublication publication{.request = request,
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
            return Result<PlatformStatWriteAdmission>::Failure(MakeError(StatCoordinatorErrors::CapacityExceeded));
        }
        ++nextWriteSequence_;
        return Result<PlatformStatWriteAdmission>::Success(PlatformStatWriteAdmission::Queued);
    }

    /** @copydoc PlatformStatCacheCoordinator::TakeNextWrite */
    Result<std::optional<PlatformStatWritePublication>> PlatformStatCacheCoordinator::TakeNextWrite() {
        if (closed_)
            return Result<std::optional<PlatformStatWritePublication>>::Failure(MakeError(StatCoordinatorErrors::Closed));
        if (inFlight_ || pending_.empty())
            return Result<std::optional<PlatformStatWritePublication>>::Success(std::nullopt);
        inFlight_ = std::move(pending_.front());
        pending_.pop_front();
        if (auto *entry = FindLedger(inFlight_->request.mutation); entry != nullptr)
            entry->state = LedgerState::InFlight;
        return Result<std::optional<PlatformStatWritePublication>>::Success(*inFlight_);
    }

    /** @copydoc PlatformStatCacheCoordinator::ProcessSuccessfulWrite */
    Result<void> PlatformStatCacheCoordinator::ProcessSuccessfulWrite(const PlatformStatWritePublication &publication, LedgerEntry &entry,
                                                                      const std::optional<PlatformStatStateEvidence> &state) {
        if (!state || state->providerGeneration != session_.ProviderGeneration() || state->sessionGeneration != session_.Generation() ||
            state->accessRevision != session_.AccessRevision()) {
            entry.state = LedgerState::Failed;
            return Failure(StatCoordinatorErrors::InvalidState);
        }
        if (const auto access = ValidatePlatformSessionAccess(session_, publication.request.subject, publication.request.accessRevision,
                                                              PlatformServiceKind::LeaderboardsAndStats);
            access.HasError()) {
            entry.state = LedgerState::Failed;
            return Failure(StatCoordinatorErrors::StaleState);
        }
        if (const auto valid = ValidateState(*state, publication.request.stat); valid.HasError()) {
            entry.state = LedgerState::Failed;
            return valid;
        }
        const PlatformStatCacheRecord record{.subject = publication.request.subject,
                                             .state = *state,
                                             .definitionFingerprint = registry_->Fingerprint(),
                                             .capturedTick = publication.request.observedTick,
                                             .expiresAtTick =
                                                 AddSaturating(publication.request.observedTick, config_.freshnessWindowTicks)};
        entry.state = LedgerState::Succeeded;
        return UpsertCache(record);
    }

    /** @copydoc PlatformStatCacheCoordinator::CompleteWrite */
    Result<void> PlatformStatCacheCoordinator::CompleteWrite(const PlatformStatWritePublication &publication,
                                                             const PlatformStatWriteOutcome outcome,
                                                             const std::optional<PlatformStatStateEvidence> &state) {
        if (closed_)
            return Failure(StatCoordinatorErrors::Closed);
        if (!inFlight_ || inFlight_->sequence != publication.sequence || !SameWrite(inFlight_->request, publication.request))
            return Failure(StatCoordinatorErrors::StalePublication);
        auto *entry = FindLedger(publication.request.mutation);
        if (entry == nullptr)
            return Failure(StatCoordinatorErrors::StalePublication);

        if (outcome == PlatformStatWriteOutcome::Succeeded) {
            const auto processed = ProcessSuccessfulWrite(publication, *entry, state);
            inFlight_.reset();
            if (processed.HasError())
                return processed;
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
