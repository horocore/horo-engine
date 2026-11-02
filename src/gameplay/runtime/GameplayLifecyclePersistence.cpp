#include "BehaviorRuntimeRestoreInternal.h"

#include <array>
#include <new>

namespace Horo::Gameplay {
    namespace {
        using namespace Runtime;
        constexpr std::size_t MaximumLifecycleBytes = 8U * 1024U * 1024U;

        CanonicalCodecLimits CodecLimits(std::size_t bytes = MaximumLifecycleBytes) {
            CanonicalCodecLimits limits;
            limits.maximumBytes = bytes;
            limits.maximumDecodedBytes = 16U * 1024U * 1024U;
            limits.maximumStringBytes = std::min(bytes, MaximumBehaviorTypeIdBytes);
            limits.maximumCollectionElements = 16'384;
            limits.maximumFields = 8;
            limits.maximumNestingDepth = 4;
            limits.maximumReadWorkBytes = 32U * 1024U * 1024U;
            return limits;
        }

        Result<std::size_t> CaptureBudget(const CanonicalCaptureContext &context, const CanonicalStateParticipantDescriptor &descriptor) {
            if (context.participant != descriptor.participant || context.schemaVersion != descriptor.schemaVersion ||
                context.scope != descriptor.scope)
                return Result<std::size_t>::Failure(MakeError(SaveErrors::CaptureContextInvalid));
            const auto bytes =
                std::min<std::uint64_t>({MaximumLifecycleBytes, context.admission.participantPayloadBytes,
                                         context.admission.operationPayloadBytes, context.admission.maximumCopiedRecordBytes});
            if (bytes == 0 || context.admission.participantRecords == 0 || context.admission.operationRecords == 0 ||
                context.admission.operationSegments == 0)
                return Result<std::size_t>::Failure(MakeError(SaveErrors::CaptureBudgetExceeded));
            return Result<std::size_t>::Success(static_cast<std::size_t>(bytes));
        }

        class Projection final : public ICanonicalRestorePreparedState {};

        class LifecycleReceipt final : public IStagedRestoreParticipant {
        public:
            LifecycleReceipt(std::shared_ptr<BehaviorRuntimeRestoreCandidate> candidate, std::vector<BehaviorLifecycleFlags> flags)
                : candidate_(std::move(candidate)), flags_(std::move(flags)),
                  requirement_{BehaviorLifecycleParticipant().participant, ParticipantSchemaVersion::Create(1).Value(),
                               SaveParticipantScope::RuntimeScene} {}

            const StagedRestoreParticipantRequirement &Requirement() const noexcept override {
                return requirement_;
            }

            Result<void> Decode(const StagedRestoreContext &) override {
                decoded_ = true;
                return Result<void>::Success();
            }

            Result<void> Validate(const StagedRestoreContext &) override {
                if (!decoded_)
                    return Result<void>::Failure(MakeError(SaveErrors::RestoreTransitionInvalid));
                validated_ = true;
                return Result<void>::Success();
            }

            Result<void> Instantiate(const StagedRestoreContext &) override {
                if (!validated_)
                    return Result<void>::Failure(MakeError(SaveErrors::RestoreTransitionInvalid));
                instantiated_ = true;
                return Result<void>::Success();
            }

            const ICanonicalRestorePreparedState *PreparedState() const noexcept override {
                return instantiated_ ? &projection_ : nullptr;
            }

            Result<void> ApplyState(const ICanonicalRestoreDependencyLookup &) override {
                if (!instantiated_ || applied_)
                    return Result<void>::Failure(MakeError(SaveErrors::RestoreTransitionInvalid));
                auto staged = BehaviorRuntimeRestoreAccess::StageFlags(*candidate_, std::move(flags_));
                if (staged.HasValue())
                    applied_ = true;
                return staged;
            }

            Result<void> FixupReferences(const ICanonicalRestoreDependencyLookup &) override {
                if (!applied_)
                    return Result<void>::Failure(MakeError(SaveErrors::RestoreTransitionInvalid));
                ready_ = true;
                return Result<void>::Success();
            }

            Result<void> FixupReferences(const ICanonicalRestoreDependencyLookup &dependencies,
                                         const SaveRestoreReferenceView &references) override {
                if (!references.references.empty())
                    return Result<void>::Failure(MakeError(SaveErrors::RestoreAdapterContractInvalid));
                return FixupReferences(dependencies);
            }

            void PublishPrepared() noexcept override {
                if (!ready_ || published_)
                    std::terminate();
                BehaviorRuntimeRestoreAccess::PublishLifecycle(*candidate_);
                published_ = true;
            }

            void RollbackPrepared() noexcept override {
                candidate_.reset();
                flags_.clear();
                ready_ = false;
            }

        private:
            std::shared_ptr<BehaviorRuntimeRestoreCandidate> candidate_;
            std::vector<BehaviorLifecycleFlags> flags_;
            StagedRestoreParticipantRequirement requirement_;
            Projection projection_;
            bool decoded_{}, validated_{}, instantiated_{}, applied_{}, ready_{}, published_{};
        };
    }  // namespace

    Runtime::SaveRecordId BehaviorLifecycleRecord() {
        return Runtime::SaveRecordId::Parse("2dfe15e0-b72b-43e6-b508-9b8ca8a07301").Value();
    }

    Runtime::CanonicalStateParticipantDescriptor BehaviorLifecycleParticipant() {
        using namespace Runtime;
        return {.participant = SaveParticipantId::Parse("horo.gameplay.lifecycle.v1").Value(),
                .schemaVersion = ParticipantSchemaVersion::Create(1).Value(),
                .scope = SaveParticipantScope::RuntimeScene,
                .roles = SaveParticipantRole::Capture | SaveParticipantRole::Restore,
                .required = true,
                .limits = {MaximumLifecycleBytes, 1, 4},
                .ownedRecords = {BehaviorLifecycleRecord()}};
    }

    BehaviorLifecyclePersistenceAdapter::BehaviorLifecyclePersistenceAdapter(std::shared_ptr<BehaviorRuntimeSceneOwner> owner,
                                                                             IdentityProvider identities)
        : owner_(std::move(owner)), identities_(std::move(identities)) {}

    Result<std::shared_ptr<BehaviorLifecyclePersistenceAdapter>> BehaviorLifecyclePersistenceAdapter::Create(
        std::shared_ptr<BehaviorRuntimeSceneOwner> owner, IdentityProvider identities) {
        if (!owner || !identities)
            return Result<std::shared_ptr<BehaviorLifecyclePersistenceAdapter>>::Failure(
                BehaviorRestoreError("Missing actual lifecycle owner or identities."));
        return Result<std::shared_ptr<BehaviorLifecyclePersistenceAdapter>>::Success(std::shared_ptr<BehaviorLifecyclePersistenceAdapter>{
            new BehaviorLifecyclePersistenceAdapter{std::move(owner), std::move(identities)}});
    }

    Result<Runtime::CanonicalCaptureDisposition> BehaviorLifecyclePersistenceAdapter::Capture(
        const Runtime::CanonicalCaptureContext &context, Runtime::ICanonicalCaptureSink &sink) const {
        using namespace Runtime;
        using Outcome = Result<CanonicalCaptureDisposition>;
        try {
            const auto descriptor = BehaviorLifecycleParticipant();
            auto budget = CaptureBudget(context, descriptor);
            if (budget.HasError())
                return Outcome::Failure(budget.ErrorValue());
            auto identities = identities_();
            if (identities.HasError())
                return Outcome::Failure(identities.ErrorValue());
            auto snapshot = owner_->CaptureLifecycle(identities.Value());
            if (snapshot.HasError())
                return Outcome::Failure(snapshot.ErrorValue());
            auto encoded = EncodeBehaviorLifecycle(snapshot.Value(), CodecLimits(budget.Value()));
            if (encoded.HasError())
                return Outcome::Failure(encoded.ErrorValue());
            if (auto written = sink.WriteCopied(BehaviorLifecycleRecord(), encoded.Value().Bytes()); written.HasError())
                return Outcome::Failure(written.ErrorValue());
            return Outcome::Success(CanonicalCaptureDisposition::Captured);
        } catch (const std::bad_alloc &) {
            return Outcome::Failure(MakeError(SaveErrors::CaptureAllocationFailed));
        } catch (...) {
            return Outcome::Failure(MakeError(SaveErrors::LifecycleCallbackFailed));
        }
    }

    Result<std::unique_ptr<Runtime::IStagedRestoreParticipant>> BehaviorLifecyclePersistenceAdapter::StageRestore(
        Runtime::ParticipantSchemaVersion schema, Runtime::SaveRecordId record, std::span<const std::byte> bytes,
        std::shared_ptr<BehaviorRuntimeRestoreCandidate> candidate, const Runtime::PersistentEntityIdentityMap &identities) const {
        using Outcome = Result<std::unique_ptr<Runtime::IStagedRestoreParticipant>>;
        if (schema != Runtime::ParticipantSchemaVersion::Create(1).Value() || record != BehaviorLifecycleRecord() || !candidate ||
            !candidate->Runner())
            return Outcome::Failure(MakeError(Runtime::SaveErrors::RestoreParticipantInvalid));
        try {
            auto decoded = DecodeBehaviorLifecycle(bytes, CodecLimits());
            if (decoded.HasError())
                return Outcome::Failure(decoded.ErrorValue());
            auto flags = BehaviorRuntimeRestoreAccess::Match(*candidate->Runner(), decoded.Value(), identities);
            if (flags.HasError())
                return Outcome::Failure(flags.ErrorValue());
            return Outcome::Success(std::make_unique<LifecycleReceipt>(std::move(candidate), std::move(flags).Value()));
        } catch (const std::bad_alloc &) {
            return Outcome::Failure(MakeError(Runtime::SaveErrors::RestoreAllocationFailed));
        }
    }
}  // namespace Horo::Gameplay
