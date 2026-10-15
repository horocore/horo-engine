#include "DestructionSaveInternal.h"
#include "Horo/Runtime/Save/SaveErrors.h"

#include <algorithm>
#include <new>

namespace Horo::Destruction {
    /** @copydoc DestructionSaveAdapter::DestructionSaveAdapter */
    DestructionSaveAdapter::DestructionSaveAdapter(ConstructionKey, DestructionSaveBinding binding,
                                                   std::shared_ptr<const IDestructionSaveSource> source)
        : binding_(std::move(binding)), source_(std::move(source)), ownerThread_(std::this_thread::get_id()) {}

    /** @copydoc DestructionSaveAdapter::Create */
    Result<std::shared_ptr<DestructionSaveAdapter>> DestructionSaveAdapter::Create(const DestructionSaveBinding &binding,
                                                                                   std::shared_ptr<const IDestructionSaveSource> source) {
        if (!source || !binding.participant.IsValid() || !binding.record.IsValid() || !binding.sceneIncarnation ||
            !binding.target.IsValid() || !binding.content.IsValid() || !binding.capabilityRevision.IsValid() ||
            !binding.authority.IsValid() || !binding.limits.maximumChunks ||
            binding.limits.maximumChunks > DestructionHardLimits::ChunksPerDestructible ||
            binding.limits.maximumAnchors > binding.limits.maximumChunks ||
            binding.limits.maximumBytes < Detail::MinimumDestructionSavePayloadBytes || binding.limits.maximumBytes > 16384 ||
            (binding.physicsParticipant && (!binding.physicsParticipant->IsValid() || *binding.physicsParticipant == binding.participant)))
            return Result<std::shared_ptr<DestructionSaveAdapter>>::Failure(MakeError(DestructionErrors::DescriptorInvalid));
        try {
            return Result<std::shared_ptr<DestructionSaveAdapter>>::Success(
                std::make_shared<DestructionSaveAdapter>(ConstructionKey{}, binding, std::move(source)));
        } catch (const std::bad_alloc &) {
            return Result<std::shared_ptr<DestructionSaveAdapter>>::Failure(MakeError(DestructionErrors::LimitExceeded));
        }
    }

    /** @copydoc DestructionSaveAdapter::Descriptor */
    Runtime::CanonicalStateParticipantDescriptor DestructionSaveAdapter::Descriptor() const {
        Runtime::CanonicalStateParticipantDescriptor result{binding_.participant,
                                                            Runtime::ParticipantSchemaVersion::Create(1).Value(),
                                                            Runtime::SaveParticipantScope::RuntimeScene,
                                                            Runtime::SaveParticipantRole::Capture,
                                                            true,
                                                            {binding_.limits.maximumBytes, 1, 1},
                                                            {},
                                                            {binding_.record}};
        if (binding_.physicsParticipant)
            result.dependencies.emplace_back(*binding_.physicsParticipant, Runtime::SaveParticipantDependencyRequirement::Required,
                                             Runtime::SaveParticipantDependencyPhase::Capture);
        return result;
    }

    /** @copydoc DestructionSaveAdapter::Capture */
    Result<Runtime::CanonicalCaptureDisposition> DestructionSaveAdapter::Capture(const Runtime::CanonicalCaptureContext &context,
                                                                                 Runtime::ICanonicalCaptureSink &sink) const {
        using Return = Result<Runtime::CanonicalCaptureDisposition>;
        if (shutdown_.Token().IsCancellationRequested())
            return Return::Failure(MakeError(DestructionErrors::ShutdownInProgress));
        if (ownerThread_ != std::this_thread::get_id() || context.provenance.sceneIncarnation != binding_.sceneIncarnation ||
            context.participant != binding_.participant || context.schemaVersion.Value() != 1 ||
            context.scope != Runtime::SaveParticipantScope::RuntimeScene)
            return Return::Failure(MakeError(DestructionErrors::StaleGeneration));
        if (!context.admission.operationRecords || !context.admission.participantRecords || !context.admission.operationSegments ||
            context.admission.operationPayloadBytes < binding_.limits.maximumBytes ||
            context.admission.participantPayloadBytes < binding_.limits.maximumBytes ||
            context.admission.maximumCopiedRecordBytes < binding_.limits.maximumBytes)
            return Return::Failure(MakeError(Runtime::SaveErrors::CaptureBudgetExceeded));
        auto captured = source_->Capture(context);
        if (captured.HasError())
            return Return::Failure(captured.ErrorValue());
        const auto &value = captured.Value();
        if (value.Fence().sceneIncarnation != binding_.sceneIncarnation ||
            value.Fence().sceneRevision != context.provenance.sceneRevision || value.Fence().epoch != context.provenance.epoch ||
            value.Fence().capabilityRevision != binding_.capabilityRevision || value.Fence().authority != binding_.authority ||
            value.State().target != binding_.target || value.State().content != binding_.content ||
            (value.Physics() && (!binding_.physicsParticipant || value.Physics()->participant != *binding_.physicsParticipant)))
            return Return::Failure(MakeError(DestructionErrors::StaleRevision));
        if (value.State().masks.chunkCount > binding_.limits.maximumChunks ||
            value.State().supportAnchors.size() > binding_.limits.maximumAnchors ||
            Detail::DestructionSavePayloadBytes(value) > binding_.limits.maximumBytes)
            return Return::Failure(MakeError(DestructionErrors::LimitExceeded));
        Runtime::CanonicalCodecLimits limits;
        limits.maximumBytes = binding_.limits.maximumBytes;
        limits.maximumStringBytes = std::min(limits.maximumStringBytes, limits.maximumBytes);
        auto encoded = EncodeDestructionSaveState(value, limits);
        if (encoded.HasError())
            return Return::Failure(encoded.ErrorValue());
        if (shutdown_.Token().IsCancellationRequested())
            return Return::Failure(MakeError(DestructionErrors::ShutdownInProgress));
        if (value.Fence().cancellation.IsCancellationRequested())
            return Return::Failure(MakeError(DestructionErrors::CancelledBeforeCommit));
        if (auto written = sink.WriteCopied(binding_.record, encoded.Value().Bytes()); written.HasError())
            return Return::Failure(written.ErrorValue());
        return Return::Success(Runtime::CanonicalCaptureDisposition::Captured);
    }

    /** @copydoc DestructionSaveAdapter::Shutdown */
    void DestructionSaveAdapter::Shutdown() noexcept {
        shutdown_.RequestCancellation();
    }
}  // namespace Horo::Destruction
