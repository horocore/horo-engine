#include "EditorPlaySaveHostRestore.h"
#include "EditorPlaySavePhysicsServices.h"
#include "Horo/Runtime/Save/SaveErrors.h"

#include <algorithm>

namespace Horo::Application::Internal {
    /** @copydoc PlaySaveRestoreSource::StageLifecycle */
    Result<void> PlaySaveRestoreSource::StageLifecycle(Runtime::RuntimeSaveRestoreStaging &staging,
                                                       const Runtime::ValidatedSaveArchive &archive,
                                                       std::shared_ptr<Gameplay::BehaviorRuntimeRestoreCandidate> candidate,
                                                       const Runtime::PersistentEntityIdentityMap &identities) const {
        if (!lifecycle_)
            return Result<void>::Success();
        const auto descriptor = Gameplay::BehaviorLifecycleParticipant();
        const auto &rows = archive.Manifest().participants;
        const auto saved = std::ranges::find(rows, descriptor.participant, &Runtime::SaveManifestParticipant::participant);
        const auto bytes = archive.SelectChunk(Gameplay::BehaviorLifecycleRecord());
        if (bytes.HasError())
            return Result<void>::Failure(bytes.ErrorValue());
        if (saved == rows.end() || !bytes.Value())
            return Result<void>::Failure(MakeError(Runtime::SaveErrors::CaptureIncomplete));
        auto receipt = lifecycle_->StageRestore(saved->schemaVersion, Gameplay::BehaviorLifecycleRecord(), *bytes.Value(),
                                                std::move(candidate), identities);
        if (receipt.HasError())
            return Result<void>::Failure(receipt.ErrorValue());
        staging.participants.push_back(std::move(receipt).Value());
        return Result<void>::Success();
    }

    /** @copydoc PlaySaveRestoreSource::StagePhysics */
    Result<void> PlaySaveRestoreSource::StagePhysics(Runtime::RuntimeSaveRestoreStaging &staging,
                                                     const std::shared_ptr<const Runtime::ValidatedSaveArchive> &archive,
                                                     const Runtime::SaveParticipantRegistrySnapshot &participants,
                                                     Runtime::RuntimeSceneView scene,
                                                     const Runtime::PersistentEntityIdentityMap &identities) const {
        if (!schema_->physics)
            return Result<void>::Success();
        const auto *registered = participants.Find(Physics::Detail::PhysicsStateSaveParticipant());
        if (!physics_ || !physicsFence_ || !context_ || !registered)
            return Result<void>::Failure(MakeError(Runtime::SaveErrors::ParticipantAdapterMissing));
        if (registered->Descriptor() != *schema_->physics)
            return Result<void>::Failure(MakeError(Runtime::SaveErrors::CaptureRegistryStale));
        auto bytes = archive->SelectChunk(Physics::Detail::PhysicsStateSaveRecord());
        if (bytes.HasError())
            return Result<void>::Failure(bytes.ErrorValue());
        if (!bytes.Value())
            return Result<void>::Failure(MakeError(Runtime::SaveErrors::CaptureIncomplete));
        auto selected = std::move(bytes).Value();
        auto source = std::make_shared<const std::vector<std::byte>>(std::move(*selected));
        auto receipt = physics_->StageRestore(std::move(source), *context_, scene, identities, cancellation_, physicsFence_);
        if (receipt.HasError())
            return Result<void>::Failure(receipt.ErrorValue());
        staging.participants.push_back(std::move(receipt).Value());
        return Result<void>::Success();
    }

    /** @copydoc PlaySaveRestoreSource::PhysicsFinalCheck */
    std::shared_ptr<Runtime::IStagedRestoreFinalPublicationCheck> PlaySaveRestoreSource::PhysicsFinalCheck() const noexcept {
        return physicsFence_;
    }
}  // namespace Horo::Application::Internal
