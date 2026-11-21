#include "Horo/XR/XRAnchorPersistence.h"

#include "Horo/Runtime/Save/SaveCaptureSnapshot.h"
#include "Horo/XR/XRSessionErrors.h"
#include "XRProtectedAnchorRecords.h"

#include <exception>
#include <limits>
#include <new>
#include <utility>

namespace Horo::XR {
    namespace {
        using namespace Runtime;
        using namespace AnchorPersistenceDetail;
        constexpr auto SchemaVersion = 1U;

        /** @brief Copies a typed 128-bit Save identity into the private wire representation. */
        template <typename Identity> [[nodiscard]] IdentityBytes EncodeIdentity(const Identity &value) noexcept {
            IdentityBytes bytes{};
            std::ranges::copy(std::as_bytes(std::span{value.Bytes()}), bytes.begin());
            return bytes;
        }

        /** @brief Materializes an opaque protected lookup only after v1 representation validation. */
        Result<XRProtectedAnchorReference> DecodeReference(const IdentityBytes &value) {
            SaveIdentityDetail::Bytes bytes{};
            std::ranges::transform(value, bytes.begin(), [](const std::byte octet) {
                return std::to_integer<std::uint8_t>(octet);
            });
            return XRProtectedAnchorReference::FromBytes(bytes);
        }

        /** @brief Admits untrusted routing metadata only when it matches the complete host-owned scope. */
        bool Matches(const ProtectedAnchorRecord &record, const XRAnchorStoreScope &scope) noexcept {
            return record.provider == EncodeIdentity(scope.provider) && record.localPartition == EncodeIdentity(scope.partition) &&
                   record.providerVersion == scope.providerVersion && record.accessRevision == scope.accessRevision;
        }

        /** @brief Builds a canonical record without retaining the live XR identity or any observed pose. */
        ProtectedAnchorRecord MakeRecord(const XRDurableAnchorState &state, const XRAnchorStoreScope &scope) noexcept {
            return {EncodeIdentity(scope.provider),
                    EncodeIdentity(scope.partition),
                    EncodeIdentity(*state.reference),
                    scope.providerVersion,
                    scope.accessRevision,
                    state.expiresAtUnixSeconds};
        }

        /** @brief Old localization is never durable; an expired reference becomes explicitly stale. */
        XRDurableAnchorState AtTime(const XRDurableAnchorState &state, const std::uint64_t now) noexcept {
            if (state.reference && state.expiresAtUnixSeconds <= now)
                return {.status = XRDurableAnchorStatus::Stale};
            return state;
        }
    }  // namespace

    /** @brief Owns one inactive XR candidate alongside its exact aggregate gate check. */
    class XRAnchorPersistence::Restore final : public IStagedRestoreParticipant, public ICanonicalRestorePreparedState {
        /** @brief Revalidates provider and XR owner before the same Save operation commit gate. */
        class Publication final : public IStagedRestoreAggregatePublication {
        public:
            explicit Publication(Restore &receipt) noexcept : receipt_(receipt) {}

            Result<void> ValidateAndArm() override {
                return receipt_.Arm();
            }

            void PublishPrepared() noexcept override {}

        private:
            Restore &receipt_;
        };

    public:
        Restore(std::shared_ptr<XRAnchorPersistence> owner, const std::span<const std::byte> bytes)
            : owner_(std::move(owner)), expectedRevision_(owner_->revision_), hasRecord_(!bytes.empty()) {
            std::ranges::copy(bytes, bytes_.begin());
        }

        [[nodiscard]] IStagedRestoreAggregatePublication *FinalPublication() noexcept {
            return &publication_;
        }

        const StagedRestoreParticipantRequirement &Requirement() const noexcept override {
            return owner_->requirement_;
        }

        Result<void> Decode(const StagedRestoreContext &) override {
            if (!hasRecord_)
                return Result<void>::Success();
            auto parsed = AnchorPersistenceDetail::Decode(bytes_);
            if (parsed.HasError())
                return Result<void>::Failure(parsed.ErrorValue());
            record_ = parsed.Value();
            return Result<void>::Success();
        }

        Result<void> Validate(const StagedRestoreContext &) override {
            const auto now = owner_->CheckCurrent();
            return now.HasError() ? Result<void>::Failure(now.ErrorValue()) : Result<void>::Success();
        }

        Result<void> Instantiate(const StagedRestoreContext &) override {
            const auto now = owner_->CheckCurrent();
            if (now.HasError())
                return Result<void>::Failure(now.ErrorValue());
            auto detached = Resolve(now.Value());
            if (detached.HasError())
                return Result<void>::Failure(detached.ErrorValue());
            candidate_ = detached.Value();
            return Result<void>::Success();
        }

        const ICanonicalRestorePreparedState *PreparedState() const noexcept override {
            return candidate_ ? this : nullptr;
        }

        Result<void> ApplyState(const ICanonicalRestoreDependencyLookup &) override {
            return Result<void>::Success();
        }

        Result<void> FixupReferences(const ICanonicalRestoreDependencyLookup &) override {
            return Result<void>::Success();
        }

        void PublishPrepared() noexcept override {
            if (!armed_)
                std::terminate();  // A registered host must bind the exact publication port to the aggregate.
            owner_->active_ = *candidate_;
            owner_->restored_ = true;
            ++owner_->revision_;
            RollbackPrepared();
        }

        void RollbackPrepared() noexcept override {
            bytes_ = {};
            record_.reset();
            candidate_.reset();
            armed_ = false;
        }

    private:
        /** @brief Converts store availability and durable expiry without using a last-known pose. */
        Result<XRDurableAnchorState> Resolve(const std::uint64_t now) const {
            using Outcome = Result<XRDurableAnchorState>;
            if (!record_)
                return Outcome::Success({});
            if (!Matches(*record_, owner_->storeScope_) || record_->expiresAt <= now)
                return Outcome::Success({.status = XRDurableAnchorStatus::Stale});
            const auto reference = DecodeReference(record_->lookup);
            if (reference.HasError())
                return Outcome::Failure(reference.ErrorValue());
            const auto stored = owner_->store_->Resolve(owner_->storeScope_, reference.Value());
            if (stored.HasError())
                return Outcome::Failure(MakeError(XRErrors::OperationUnavailable));
            if (!stored.Value())
                return Outcome::Success({});
            const auto &entry = *stored.Value();
            if (entry.reference != reference.Value() || entry.expiresAtUnixSeconds != record_->expiresAt)
                return Outcome::Success({.status = XRDurableAnchorStatus::Stale});
            return Outcome::Success({XRDurableAnchorStatus::Unlocalized, entry.reference, entry.expiresAtUnixSeconds});
        }

        /** @brief Claims this candidate's exact owner revision after all fallible Save phases. */
        Result<void> Arm() {
            const auto now = owner_->CheckCurrent();
            if (now.HasError())
                return Result<void>::Failure(now.ErrorValue());
            if (!candidate_ || armed_ || owner_->revision_ != expectedRevision_)
                return Result<void>::Failure(MakeError(XRErrors::IdentityStale));
            if (expectedRevision_ == std::numeric_limits<std::uint64_t>::max())
                return Result<void>::Failure(MakeError(XRErrors::CapacityExceeded));
            candidate_ = AtTime(*candidate_, now.Value());
            armed_ = true;
            return Result<void>::Success();
        }

        std::shared_ptr<XRAnchorPersistence> owner_;
        std::uint64_t expectedRevision_;
        bool hasRecord_{};
        std::array<std::byte, RecordBytes> bytes_{};
        std::optional<ProtectedAnchorRecord> record_;
        std::optional<XRDurableAnchorState> candidate_;
        bool armed_{};
        Publication publication_{*this};
    };

    XRAnchorPersistence::XRAnchorPersistence(SaveParticipantId participant, SaveRecordId record,
                                             const std::shared_ptr<const XRSpatialAnchors> &anchors, XRAnchorScope liveScope,
                                             XRAnchorStoreScope storeScope, std::shared_ptr<const IXRProtectedAnchorStore> store)
        : requirement_{std::move(participant), ParticipantSchemaVersion::Create(SchemaVersion).Value(),
                       SaveParticipantScope::PersistentWorld},
          record_(record), anchors_(anchors), liveScope_(liveScope), storeScope_(storeScope), store_(std::move(store)) {}

    /** @copydoc XRAnchorPersistence::Bind */
    Result<std::shared_ptr<XRAnchorPersistence>> XRAnchorPersistence::Bind(SaveParticipantId participant, SaveRecordId record,
                                                                           const std::shared_ptr<const XRSpatialAnchors> &anchors,
                                                                           const XRAnchorScope liveScope,
                                                                           const XRAnchorStoreScope storeScope,
                                                                           std::shared_ptr<const IXRProtectedAnchorStore> store) {
        using Outcome = Result<std::shared_ptr<XRAnchorPersistence>>;
        if (!store)
            return Outcome::Failure(MakeError(XRErrors::OperationUnsupported));
        if (!anchors || !participant.IsValid() || !record.IsValid() || !storeScope.provider.IsValid() || !storeScope.partition.IsValid() ||
            storeScope.providerVersion == 0 || storeScope.accessRevision == 0)
            return Outcome::Failure(MakeError(XRErrors::OperationInvalid));
        try {
            auto value = std::shared_ptr<XRAnchorPersistence>{
                new XRAnchorPersistence(std::move(participant), record, anchors, liveScope, storeScope, std::move(store))};
            const auto ready = value->CheckCurrent();
            return ready.HasError() ? Outcome::Failure(ready.ErrorValue()) : Outcome::Success(std::move(value));
        } catch (const std::bad_alloc &) {
            return Outcome::Failure(MakeError(SaveErrors::OperationAllocationFailed));
        }
    }

    /** @copydoc XRAnchorPersistence::Descriptor */
    CanonicalStateParticipantDescriptor XRAnchorPersistence::Descriptor() const {
        return Describe(requirement_.participant, record_);
    }

    CanonicalStateParticipantDescriptor XRAnchorPersistence::Describe(SaveParticipantId participant, SaveRecordId record) {
        return {std::move(participant),
                ParticipantSchemaVersion::Create(SchemaVersion).Value(),
                SaveParticipantScope::PersistentWorld,
                SaveParticipantRole::Capture | SaveParticipantRole::Restore,
                false,
                {RecordBytes, 1, 1},
                {},
                {record}};
    }

    /** @copydoc XRAnchorPersistence::RecordLocalized */
    Result<void> XRAnchorPersistence::RecordLocalized(const XRSpatialAnchorId &id, const XRRuntimeTime now) {
        const auto admitted = CheckCurrent();
        if (admitted.HasError())
            return Result<void>::Failure(admitted.ErrorValue());
        const auto revision = revision_;
        if (revision == std::numeric_limits<std::uint64_t>::max())
            return Result<void>::Failure(MakeError(XRErrors::CapacityExceeded));
        const auto anchors = anchors_.lock();
        if (!anchors)
            return Result<void>::Failure(MakeError(XRErrors::IdentityStale));
        const auto snapshot = anchors->Read(liveScope_, id, now);
        if (snapshot.HasError())
            return Result<void>::Failure(snapshot.ErrorValue());
        if (snapshot.Value().Observation().state != XRAnchorLocalization::Localized)
            return Result<void>::Failure(MakeError(XRErrors::OperationUnavailable));
        const auto stored = store_->FindLive(storeScope_, id);
        if (stored.HasError())
            return Result<void>::Failure(MakeError(XRErrors::OperationUnavailable));
        const auto current = CheckCurrent();
        if (current.HasError())
            return Result<void>::Failure(current.ErrorValue());
        if (revision != revision_)
            return Result<void>::Failure(MakeError(XRErrors::IdentityStale));
        const auto revalidated = anchors->Read(liveScope_, id, now);
        if (revalidated.HasError())
            return Result<void>::Failure(revalidated.ErrorValue());
        if (revalidated.Value().Observation().state != XRAnchorLocalization::Localized)
            return Result<void>::Failure(MakeError(XRErrors::OperationUnavailable));
        const auto &entry = stored.Value();
        if (!entry.reference.IsValid() || entry.expiresAtUnixSeconds <= current.Value())
            return Result<void>::Failure(MakeError(XRErrors::OperationInvalid));
        active_ = {XRDurableAnchorStatus::Unlocalized, entry.reference, entry.expiresAtUnixSeconds};
        restored_ = false;
        ++revision_;
        return Result<void>::Success();
    }

    /** @copydoc XRAnchorPersistence::Read */
    Result<XRDurableAnchorState> XRAnchorPersistence::Read() const {
        using Outcome = Result<XRDurableAnchorState>;
        const auto now = CheckCurrent();
        return now.HasError() ? Outcome::Failure(now.ErrorValue()) : Outcome::Success(AtTime(active_, now.Value()));
    }

    Result<XRDurableAnchorState> XRAnchorPersistence::ReadRestored(const IXRProtectedAnchorStore &store) const {
        using Outcome = Result<XRDurableAnchorState>;
        if (&store != store_.get())
            return Outcome::Failure(MakeError(XRErrors::PermissionDenied));
        const auto current = Read();
        if (current.HasError() || restored_)
            return current;
        return Outcome::Success({});
    }

    /** @copydoc XRAnchorPersistence::Capture */
    Result<CanonicalCaptureDisposition> XRAnchorPersistence::Capture(const CanonicalCaptureContext &context,
                                                                     ICanonicalCaptureSink &sink) const {
        using Outcome = Result<CanonicalCaptureDisposition>;
        if (context.participant != requirement_.participant || context.schemaVersion != requirement_.schemaVersion ||
            context.scope != requirement_.scope)
            return Outcome::Failure(MakeError(SaveErrors::CaptureRegistryStale));
        const auto current = Read();
        if (current.HasError())
            return Outcome::Failure(current.ErrorValue());
        if (!current.Value().reference)
            return Outcome::Success(CanonicalCaptureDisposition::Omitted);
        const auto &budget = context.admission;
        if (budget.operationRecords == 0 || budget.operationSegments == 0 || budget.participantRecords == 0 ||
            budget.operationPayloadBytes < RecordBytes || budget.participantPayloadBytes < RecordBytes ||
            budget.maximumCopiedRecordBytes < RecordBytes)
            return Outcome::Failure(MakeError(SaveErrors::CaptureBudgetExceeded));
        const auto bytes = Encode(MakeRecord(current.Value(), storeScope_));
        const auto written = sink.WriteCopied(record_, bytes);
        return written.HasError() ? Outcome::Failure(written.ErrorValue()) : Outcome::Success(CanonicalCaptureDisposition::Captured);
    }

    /** @copydoc XRAnchorPersistence::StageRestore */
    Result<XRStagedAnchorRestore> XRAnchorPersistence::StageRestore(const std::span<const std::byte> bytes) {
        using Outcome = Result<XRStagedAnchorRestore>;
        const auto current = CheckCurrent();
        if (current.HasError())
            return Outcome::Failure(current.ErrorValue());
        if (bytes.size() > RecordBytes)
            return Outcome::Failure(MakeError(XRErrors::CapacityExceeded));
        if (!bytes.empty() && bytes.size() != RecordBytes)
            return Outcome::Failure(MakeError(XRErrors::OperationInvalid));
        try {
            auto receipt = std::make_unique<Restore>(shared_from_this(), bytes);
            auto *publication = receipt->FinalPublication();
            return Outcome::Success({std::move(receipt), publication});
        } catch (const std::bad_alloc &) {
            return Outcome::Failure(MakeError(SaveErrors::OperationAllocationFailed));
        }
    }

    /** @brief Validates both the focused XR grant and the protected local purpose without doing storage reads. */
    Result<std::uint64_t> XRAnchorPersistence::CheckCurrent() const {
        using Outcome = Result<std::uint64_t>;
        if (std::this_thread::get_id() != owner_)
            return Outcome::Failure(MakeError(XRSessionErrors::WrongThread));
        const auto anchors = anchors_.lock();
        if (!anchors)
            return Outcome::Failure(MakeError(XRErrors::IdentityStale));
        if (const auto authorized = store_->Admit(storeScope_); authorized.HasError())
            return Outcome::Failure(MakeError(XRErrors::PermissionDenied));
        const auto now = store_->CurrentUnixSeconds();
        if (now == 0 || now < lastUnixSeconds_)
            return Outcome::Failure(MakeError(XRErrors::TimeDomainIncompatible));
        const auto live = anchors->CheckScope(liveScope_);
        if (live.HasError())
            return Outcome::Failure(live.ErrorValue());
        lastUnixSeconds_ = now;
        return Outcome::Success(now);
    }
}  // namespace Horo::XR
