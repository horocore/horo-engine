#include "EditorPlaySaveHostRestore.h"

#include "EditorPlaySavePrefabCapture.h"

#include <algorithm>

namespace Horo::Application::Internal {
    namespace {
        using namespace Runtime;

        /** @brief Uses only the original installed binder and its actual generation; no
         * metadata grants a candidate source. */
        Result<std::shared_ptr<const GameplayPersistenceAdapter>> CandidateAdapter(
            const Editor::PlaySaveGameplayBinding &binding, const std::shared_ptr<Gameplay::BehaviorRuntimeRestoreCandidate> &candidate,
            const PersistentEntityIdentityMap &identities) {
            using Outcome = Result<std::shared_ptr<const GameplayPersistenceAdapter>>;
            const auto *type = std::get_if<Gameplay::BehaviorTypeId>(&binding.declaration.ownerIdentity);
            if (!type)
                return Outcome::Success(binding.adapter);
            if (!candidate)
                return Outcome::Failure(MakeError(SaveErrors::CompositionUnsupported));
            auto target = candidate->Binding(*type, identities);
            if (target.HasError())
                return Outcome::Failure(target.ErrorValue());
            auto installed = binding.adapter->BindRestoreCandidate(std::move(target).Value());
            if (installed.HasError())
                return Outcome::Failure(installed.ErrorValue());
            if (!binding.installation.SameGeneration(installed.Value()))
                return Outcome::Failure(MakeError(SaveErrors::CaptureRegistryStale));
            auto actual = std::dynamic_pointer_cast<const GameplayPersistenceAdapter>(installed.Value().AcquireAdapter());
            return actual ? Outcome::Success(std::move(actual)) : Outcome::Failure(MakeError(SaveErrors::ParticipantAdapterMissing));
        }

        /** @brief Stages only an actual declared native source whose exact frozen schema is registered in this operation. */
        Result<std::unique_ptr<IStagedRestoreParticipant>> StageGameplay(
            const Editor::PlaySaveGameplayBinding &binding, const ValidatedSaveArchive &archive,
            const SaveParticipantRegistrySnapshot &participants,
            const std::shared_ptr<Gameplay::BehaviorRuntimeRestoreCandidate> &candidate, const PersistentEntityIdentityMap &identities) {
            using Outcome = Result<std::unique_ptr<IStagedRestoreParticipant>>;
            const auto &descriptor = binding.declaration.descriptor;
            const auto *registered = participants.Find(descriptor.participant.participant);
            if (!binding.installation.CanUse() || !binding.adapter || !registered)
                return Outcome::Failure(MakeError(SaveErrors::ParticipantAdapterMissing));
            if (registered->Descriptor() != descriptor.participant)
                return Outcome::Failure(MakeError(SaveErrors::CaptureRegistryStale));
            const auto found = std::ranges::find(archive.Manifest().participants, descriptor.participant.participant,
                                                 &SaveManifestParticipant::participant);
            if (found == archive.Manifest().participants.end()) {
                if (descriptor.participant.required)
                    return Outcome::Failure(MakeError(SaveErrors::CaptureIncomplete));
                return Outcome::Success(nullptr);
            }
            const auto record = archive.SelectChunk(descriptor.record);
            if (record.HasError())
                return Outcome::Failure(record.ErrorValue());
            if (!record.Value())
                return Outcome::Failure(MakeError(SaveErrors::CaptureIncomplete));
            auto adapter = CandidateAdapter(binding, candidate, identities);
            if (adapter.HasError())
                return Outcome::Failure(adapter.ErrorValue());
            return adapter.Value()->StageRestore(found->schemaVersion, descriptor.record, *record.Value(),
                                                 GameplayPersistenceRestorePolicy::ReloadableScene);
        }

        /** @brief Covers the complete actual native inventory before publishing detached staging to the owner. */
        Result<RuntimeSaveRestoreStaging> StageGameplayInventory(
            const ValidatedSaveArchive &archive, const SaveParticipantRegistrySnapshot &participants,
            std::span<const Editor::PlaySaveGameplayBinding> bindings,
            const std::shared_ptr<Gameplay::BehaviorRuntimeRestoreCandidate> &candidate, const PersistentEntityIdentityMap &identities) {
            using Outcome = Result<RuntimeSaveRestoreStaging>;
            RuntimeSaveRestoreStaging staging;
            staging.participants.reserve(bindings.size());
            for (const auto &binding : bindings) {
                auto receipt = StageGameplay(binding, archive, participants, candidate, identities);
                if (receipt.HasError())
                    return Outcome::Failure(receipt.ErrorValue());
                if (receipt.Value())
                    staging.participants.push_back(std::move(receipt).Value());
            }
            return Outcome::Success(std::move(staging));
        }
    }  // namespace

    /** @copydoc PlaySaveRestoreSource::PlaySaveRestoreSource */
    PlaySaveRestoreSource::PlaySaveRestoreSource(std::shared_ptr<PlaySaveStorage> storage,
                                                 std::shared_ptr<const PlaySaveStorageSchema> schema, Runtime::SaveManagerCommand selection,
                                                 std::shared_ptr<const std::vector<Editor::PlaySaveGameplayBinding>> liveBindings,
                                                 std::shared_ptr<Gameplay::BehaviorRuntimeSceneOwner> behaviorOwner,
                                                 std::shared_ptr<Gameplay::BehaviorLifecyclePersistenceAdapter> lifecycle)
        : storage_(std::move(storage)), schema_(std::move(schema)), selection_(std::move(selection)),
          liveBindings_(std::move(liveBindings)), behaviorOwner_(std::move(behaviorOwner)), lifecycle_(std::move(lifecycle)) {}

    /** @copydoc PlaySaveRestoreSource::Stage */
    Result<Runtime::RuntimeSaveRestoreStaging> PlaySaveRestoreSource::Stage(const Runtime::StagedRestoreContext &context,
                                                                            const Runtime::SaveParticipantRegistrySnapshot &participants,
                                                                            const CancellationToken &cancellation) {
        using Outcome = Result<Runtime::RuntimeSaveRestoreStaging>;
        {
            std::lock_guard lock(publication_);
            if (attempted_)
                return Outcome::Failure(MakeError(Runtime::SaveErrors::RestoreTransitionInvalid));
            attempted_ = true;
        }
        if (context.registryGeneration != participants.Generation() || !participants.Find(PlaySaveSceneParticipant()))
            return Outcome::Failure(MakeError(Runtime::SaveErrors::CaptureRegistryStale));
        auto archive = storage_->Read(selection_, cancellation);
        if (archive.HasError())
            return Outcome::Failure(archive.ErrorValue());
        const auto bytes = archive.Value().SelectChunk(PlaySaveSceneRecord());
        if (bytes.HasError())
            return Outcome::Failure(bytes.ErrorValue());
        if (!bytes.Value())
            return Outcome::Failure(MakeError(Runtime::SaveErrors::CaptureIncomplete));
        const auto bindings = liveBindings_ ? std::span<const Editor::PlaySaveGameplayBinding>{*liveBindings_}
                                            : std::span<const Editor::PlaySaveGameplayBinding>{};
        auto decoded = DecodePlaySaveScene(*bytes.Value(), schema_->baseline, schema_->identities, bindings);
        if (decoded.HasError())
            return Outcome::Failure(decoded.ErrorValue());
        auto prefabs = DecodePlaySavePrefabs(archive.Value(), schema_->prefabProfile, schema_->identities.size());
        if (prefabs.HasError())
            return Outcome::Failure(prefabs.ErrorValue());
        auto scene = std::move(decoded).Value();
        scene.prefabs = std::move(prefabs).Value();
        auto immutable = std::make_shared<const PlaySaveSceneState>(std::move(scene));
        {
            std::lock_guard lock(publication_);
            decoded_ = std::move(immutable);
            archive_ = std::make_shared<const Runtime::ValidatedSaveArchive>(std::move(archive).Value());
        }
        return Outcome::Success({});
    }

    /** @copydoc PlaySaveRestoreSource::BehaviorOwner */
    std::shared_ptr<Gameplay::BehaviorRuntimeSceneOwner> PlaySaveRestoreSource::BehaviorOwner() const noexcept {
        return behaviorOwner_;
    }

    /** @copydoc PlaySaveRestoreSource::StageOwners */
    Result<Runtime::RuntimeSaveRestoreStaging> PlaySaveRestoreSource::StageOwners(
        const Runtime::SaveParticipantRegistrySnapshot &participants, std::shared_ptr<Gameplay::BehaviorRuntimeRestoreCandidate> candidate,
        const Runtime::PersistentEntityIdentityMap &identities) const {
        using Outcome = Result<Runtime::RuntimeSaveRestoreStaging>;
        std::shared_ptr<const Runtime::ValidatedSaveArchive> archive;
        {
            std::lock_guard lock(publication_);
            archive = archive_;
        }
        if (!archive)
            return Outcome::Failure(MakeError(Runtime::SaveErrors::RestoreTransitionInvalid));
        const auto bindings = liveBindings_ ? std::span<const Editor::PlaySaveGameplayBinding>{*liveBindings_}
                                            : std::span<const Editor::PlaySaveGameplayBinding>{};
        auto prepared = StageGameplayInventory(*archive, participants, bindings, candidate, identities);
        if (prepared.HasError() || !lifecycle_)
            return prepared;
        const auto descriptor = Gameplay::BehaviorLifecycleParticipant();
        const auto saved =
            std::ranges::find(archive->Manifest().participants, descriptor.participant, &Runtime::SaveManifestParticipant::participant);
        const auto bytes = archive->SelectChunk(Gameplay::BehaviorLifecycleRecord());
        if (saved == archive->Manifest().participants.end() || bytes.HasError() || !bytes.Value())
            return Outcome::Failure(MakeError(Runtime::SaveErrors::CaptureIncomplete));
        auto lifecycle = lifecycle_->StageRestore(saved->schemaVersion, Gameplay::BehaviorLifecycleRecord(), *bytes.Value(),
                                                  std::move(candidate), identities);
        if (lifecycle.HasError())
            return Outcome::Failure(lifecycle.ErrorValue());
        auto result = std::move(prepared).Value();
        result.participants.push_back(std::move(lifecycle).Value());
        return Outcome::Success(std::move(result));
    }

    /** @copydoc PlaySaveRestoreSource::PreparedScene */
    std::shared_ptr<const PlaySaveSceneState> PlaySaveRestoreSource::PreparedScene() const {
        std::lock_guard lock(publication_);
        return decoded_;
    }
}  // namespace Horo::Application::Internal
