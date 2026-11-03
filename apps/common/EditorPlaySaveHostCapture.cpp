#include "EditorPlaySaveHostCapture.h"

#include "EditorPlaySavePrefabServices.h"

#include <algorithm>
#include <set>
#include <tuple>

namespace Horo::Application::Internal {
    namespace {
        using namespace Runtime;
        constexpr std::size_t MaximumEntities = 16'384;
        constexpr std::size_t MaximumPayloadBytes = 8U << 20U;

        /** @brief Rejects state belonging to physical owners which have not supplied a canonical save adapter. */
        bool PhysicalState(const RuntimeComponentSet &c) noexcept {
            return c.rigidBody || !c.colliders.empty() || !c.physicsConstraints.empty() || c.characterController;
        }

        /** @brief Rejects navigation and AI runtime state rather than persisting only its authored descriptors. */
        bool AgentState(const RuntimeComponentSet &c) noexcept {
            return c.navigationSurface || c.navigationRegion || c.navigationModifier || c.navigationLink || c.navigationAgent ||
                   c.aiAgent || c.aiController;
        }

        /** @brief Compares every immutable durable descriptor field without substituting a digest for identity. */
        bool SameDescriptor(const GameplayPersistenceDescriptor &a, const GameplayPersistenceDescriptor &b) noexcept {
            return std::tie(a.participant, a.record, a.owner, a.moduleId, a.moduleVersion) ==
                   std::tie(b.participant, b.record, b.owner, b.moduleId, b.moduleVersion);
        }

        /** @brief Requires a real admitted native installation and its exact source/schema before allowing behavior state. */
        bool CoversBehavior(const Editor::PlaySaveGameplayBinding &binding, const Gameplay::BehaviorTypeId &type) noexcept {
            if (!binding.installation.CanUse() || !binding.adapter)
                return false;
            const auto *owner = std::get_if<Gameplay::BehaviorTypeId>(&binding.declaration.ownerIdentity);
            return owner && *owner == type && SameDescriptor(binding.declaration.descriptor, binding.installation.Descriptor()) &&
                   SameDescriptor(binding.declaration.descriptor, binding.adapter->Descriptor());
        }

        /** @brief Every active behavior needs an actual registered source; Lua and undeclared state gain no implicit support. */
        bool BehaviorsCovered(const RuntimeComponentSet &components, std::span<const Editor::PlaySaveGameplayBinding> gameplay) {
            return std::ranges::all_of(components.behaviors, [&](const auto &behavior) {
                return std::ranges::any_of(gameplay, [&](const auto &binding) {
                    return CoversBehavior(binding, behavior.typeId);
                });
            });
        }

        /** @brief Rejects native peer fields for which this host profile has no canonical adapter; no metadata grants coverage. */
        bool UnsupportedPeerState(const RuntimeComponentSet &c) {
            return PhysicalState(c) || AgentState(c) || c.audioSource || c.audioListener || c.uiCanvas || !c.gameplayComponents.empty();
        }

        /** @brief Required peer state cannot disappear behind a successful core Scene record. */
        bool UnregisteredState(const RuntimeComponentSet &c, std::span<const Editor::PlaySaveGameplayBinding> gameplay) {
            return UnsupportedPeerState(c) || !BehaviorsCovered(c, gameplay);
        }

        /** @brief Identifies unsupported content without claiming that a source descriptor is live subsystem state. */
        Error Unsupported() {
            return MakeError(SaveErrors::RestoreParticipantInvalid, "This Play world requires a registered save/restore adapter "
                                                                    "for runtime-spawned or subsystem-owned state.");
        }

        /** @brief Retains all exact asset requirements and only still-present legacy migration evidence. */
        Result<void> BaselineRequirements(SceneDefinitionBuilder &builder, const RuntimeSceneDefinition &baseline,
                                          const RuntimeSceneView &scene) {
            for (const auto &asset : baseline.AssetDependencies())
                if (auto result = builder.RequireAsset(asset); result.HasError())
                    return result;
            for (const auto &evidence : baseline.TriggerVolumeEvidence())
                if (scene.Find(evidence.object))
                    if (auto result = builder.RequireTriggerVolume(evidence); result.HasError())
                        return result;
            return Result<void>::Success();
        }

        /** @brief Requires complete current core views before copying any entity value. */
        bool HasCoreValues(const RuntimeEntityView &entity) noexcept {
            return entity.authoredObject && entity.localTransform && entity.components && entity.primitiveMesh;
        }

        /** @brief Copies one real entity and translates its parent through current authored provenance. */
        Result<RuntimeEntityDefinition> CaptureEntity(const RuntimeSceneView &scene, const RuntimeEntityView &entity,
                                                      std::span<const Editor::PlaySaveGameplayBinding> gameplay) {
            using Outcome = Result<RuntimeEntityDefinition>;
            if (!HasCoreValues(entity))
                return Outcome::Failure(Unsupported());
            if (UnregisteredState(*entity.components, gameplay) || !entity.groupReferences.empty() || !entity.groupSpawnLineage.empty())
                return Outcome::Failure(Unsupported());
            std::optional<SceneObjectId> parent;
            if (entity.parent) {
                const auto current = scene.Get(*entity.parent);
                if (current.HasError())
                    return Outcome::Failure(current.ErrorValue());
                if (!current.Value().authoredObject)
                    return Outcome::Failure(Unsupported());
                parent = current.Value().authoredObject;
            }
            return Outcome::Success({*entity.authoredObject, parent, *entity.localTransform, *entity.primitiveMesh, *entity.components});
        }

        /** @brief Dynamic existence is owned by the same canonical inventory and all subsystem state still requires actual adapters. */
        Result<void> CaptureDynamic(const RuntimeEntityView &entity, const PersistentEntityIdentityMap &inventory,
                                    std::span<const Editor::PlaySaveGameplayBinding> gameplay) {
            auto identity = inventory.Find(entity.entity);
            if (identity.HasError())
                return Result<void>::Failure(identity.ErrorValue());
            const auto records = inventory.Records();
            const auto record = std::ranges::find(records, identity.Value(), &PersistentEntityRecord::identity);
            if (record == records.end() || !std::holds_alternative<SpawnedPersistentEntityProvenance>(record->provenance) ||
                !entity.components || UnregisteredState(*entity.components, gameplay))
                return Result<void>::Failure(Unsupported());
            return Result<void>::Success();
        }

        /** @brief Freezes current typed values only after bounds and actual Scene identity have been admitted. */
        Result<RuntimeSceneDefinition> CaptureDefinition(const RuntimeSceneView &scene, const RuntimeSceneDefinition &baseline,
                                                         std::span<const Editor::PlaySaveGameplayBinding> gameplay,
                                                         const PersistentEntityIdentityMap *inventory) {
            SceneDefinitionBuilder builder(baseline.Id(), baseline.Revision());
            for (std::size_t index = 0; index < scene.SlotCount(); ++index) {
                const auto entity = scene.EntityAt(index);
                if (!entity)
                    continue;
                if (!entity->authoredObject && inventory) {
                    if (auto admitted = CaptureDynamic(*entity, *inventory, gameplay); admitted.HasError())
                        return Result<RuntimeSceneDefinition>::Failure(admitted.ErrorValue());
                    continue;
                }
                auto captured = CaptureEntity(scene, *entity, gameplay);
                if (captured.HasError())
                    return Result<RuntimeSceneDefinition>::Failure(captured.ErrorValue());
                builder.Add(std::move(captured).Value());
            }
            if (auto required = BaselineRequirements(builder, baseline, scene); required.HasError())
                return Result<RuntimeSceneDefinition>::Failure(required.ErrorValue());
            return std::move(builder).Build();
        }

        /** @brief Checks complete authored-origin coverage without deriving persistent IDs from transient entities. */
        bool InventoryMatches(const RuntimeSceneDefinition &baseline, std::span<const PersistentEntityRecord> records) {
            std::set<SceneObjectId> objects;
            for (const auto &entity : baseline.Entities())
                objects.insert(entity.object);
            for (const auto &record : records) {
                const auto *authored = std::get_if<AuthoredPersistentEntityProvenance>(&record.provenance);
                if (!authored || authored->scene != baseline.Id() || objects.erase(authored->object) != 1)
                    return false;
            }
            return objects.empty();
        }

        /** @brief Verifies durable bindings cover every current entity and retain absent authored records as explicit tombstones. */
        Result<PersistentEntityIdentityMap> CaptureIdentities(const RuntimeSceneView &scene, const RuntimeSceneDefinition &baseline,
                                                              std::span<const PersistentEntityRecord> source, std::size_t liveCount) {
            using Outcome = Result<PersistentEntityIdentityMap>;
            if (source.size() != baseline.Entities().size() || source.size() > MaximumEntities || !InventoryMatches(baseline, source))
                return Outcome::Failure(Unsupported());
            std::vector<PersistentEntityRecord> records(source.begin(), source.end());
            std::vector<PersistentEntityRuntimeBinding> bindings;
            bindings.reserve(liveCount);
            for (auto &record : records) {
                const auto *authored = std::get_if<AuthoredPersistentEntityProvenance>(&record.provenance);
                if (!authored || authored->scene != baseline.Id())
                    return Outcome::Failure(Unsupported());
                const auto entity = scene.Find(authored->object);
                record.disposition = entity ? PersistentEntityDisposition::Live : PersistentEntityDisposition::Tombstone;
                if (entity)
                    bindings.push_back({record.identity, record.generation, *entity});
            }
            if (bindings.size() != liveCount)
                return Outcome::Failure(Unsupported());
            return PersistentEntityIdentityMap::Create(scene.RuntimeId(), records, bindings, MaximumEntities);
        }
    }  // namespace

    /** @copydoc PlaySaveSceneParticipant */
    Runtime::SaveParticipantId PlaySaveSceneParticipant() {
        return Runtime::SaveParticipantId::Parse("horo.editor.play.scene.v1").Value();
    }

    /** @copydoc PlaySaveSceneRecord */
    Runtime::SaveRecordId PlaySaveSceneRecord() {
        return Runtime::SaveRecordId::Parse("eed9901c-f8ad-4db9-97c0-abde392deba4").Value();
    }

    /** @copydoc PlaySaveSceneDescriptor */
    Runtime::CanonicalStateParticipantDescriptor PlaySaveSceneDescriptor() {
        return {.participant = PlaySaveSceneParticipant(),
                .schemaVersion = Runtime::ParticipantSchemaVersion::Create(1).Value(),
                .scope = Runtime::SaveParticipantScope::RuntimeScene,
                .roles = Runtime::SaveParticipantRole::Capture,
                .required = true,
                .limits = {MaximumPayloadBytes, 1, 8},
                .dependencies = {},
                .ownedRecords = {PlaySaveSceneRecord()}};
    }

    /** @copydoc ValidatePlaySaveCoreState */
    Result<void> ValidatePlaySaveCoreState(const Runtime::RuntimeSceneDefinition &definition,
                                           std::span<const Editor::PlaySaveGameplayBinding> gameplay) {
        if (definition.Entities().size() > MaximumEntities)
            return Result<void>::Failure(MakeError(Runtime::SaveErrors::CaptureBudgetExceeded));
        for (const auto &entity : definition.Entities())
            if (UnregisteredState(entity.components, gameplay))
                return Result<void>::Failure(Unsupported());
        return Result<void>::Success();
    }

    /** @copydoc ValidatePlaySaveCoreStructure */
    Result<void> ValidatePlaySaveCoreStructure(const Runtime::RuntimeSceneDefinition &definition) {
        if (definition.Entities().size() > MaximumEntities)
            return Result<void>::Failure(MakeError(Runtime::SaveErrors::CaptureBudgetExceeded));
        for (const auto &entity : definition.Entities())
            if (UnsupportedPeerState(entity.components))
                return Result<void>::Failure(Unsupported());
        return Result<void>::Success();
    }

    /** @copydoc CapturePlaySaveIdentities */
    Result<Runtime::PersistentEntityIdentityMap> CapturePlaySaveIdentities(Runtime::RuntimeSceneView scene,
                                                                           const Runtime::RuntimeSceneDefinition &baseline,
                                                                           std::span<const Runtime::PersistentEntityRecord> identities) {
        using Outcome = Result<Runtime::PersistentEntityIdentityMap>;
        if (!scene.IsCurrent() || scene.DefinitionId() != baseline.Id() || scene.DefinitionRevision() != baseline.Revision())
            return Outcome::Failure(MakeError(Runtime::SaveErrors::CaptureContextInvalid));
        if (scene.SlotCount() > MaximumEntities || scene.BaselineCount() != 0)
            return Outcome::Failure(Unsupported());
        std::size_t live = 0;
        for (std::size_t slot = 0; slot < scene.SlotCount(); ++slot)
            live += scene.EntityAt(slot).has_value();
        return CaptureIdentities(scene, baseline, identities, live);
    }

    /** @copydoc CapturePlaySaveScene */
    Result<PlaySaveSceneState> CapturePlaySaveScene(Runtime::RuntimeSceneView scene, const Runtime::RuntimeSceneDefinition &baseline,
                                                    std::span<const Runtime::PersistentEntityRecord> identities,
                                                    std::span<const Editor::PlaySaveGameplayBinding> gameplay,
                                                    const Runtime::PersistentEntityIdentityMap *inventory) {
        using Outcome = Result<PlaySaveSceneState>;
        if (!scene.IsCurrent() || scene.DefinitionId() != baseline.Id() || scene.DefinitionRevision() != baseline.Revision())
            return Outcome::Failure(MakeError(Runtime::SaveErrors::CaptureContextInvalid));
        if (scene.SlotCount() > MaximumEntities || scene.BaselineCount() != 0)
            return Outcome::Failure(Unsupported());
        auto definition = CaptureDefinition(scene, baseline, gameplay, inventory);
        if (definition.HasError())
            return Outcome::Failure(definition.ErrorValue());
        const auto map = CaptureIdentities(scene, baseline, identities, definition.Value().Entities().size());
        if (map.HasError())
            return Outcome::Failure(map.ErrorValue());
        return Outcome::Success({std::move(definition).Value(), {map.Value().Records().begin(), map.Value().Records().end()}});
    }

    /** @copydoc PlaySaveSceneCapture::PlaySaveSceneCapture */
    PlaySaveSceneCapture::PlaySaveSceneCapture(Runtime::RuntimeSceneService &scenes, Runtime::RuntimeSceneDefinition baseline,
                                               std::vector<Runtime::PersistentEntityRecord> identities,
                                               std::weak_ptr<const std::vector<Editor::PlaySaveGameplayBinding>> gameplay,
                                               std::shared_ptr<PlaySavePrefabServices> prefabs)
        : scenes_(scenes), baseline_(std::move(baseline)), identities_(std::move(identities)), gameplay_(std::move(gameplay)),
          prefabs_(std::move(prefabs)) {}

    /** @copydoc PlaySaveSceneCapture::CompleteIdentities */
    Result<std::optional<Runtime::PersistentEntityIdentityMap>> PlaySaveSceneCapture::CompleteIdentities() const {
        using Outcome = Result<std::optional<Runtime::PersistentEntityIdentityMap>>;
        std::optional<Runtime::PersistentEntityIdentityMap> complete;
        if (prefabs_) {
            const auto inventory = prefabs_->Inventory();
            if (!inventory)
                return Outcome::Failure(MakeError(Runtime::SaveErrors::CaptureContextInvalid));
            auto mapped = inventory->Identities();
            if (mapped.HasError())
                return Outcome::Failure(mapped.ErrorValue());
            complete.emplace(std::move(mapped).Value());
        }
        return Outcome::Success(std::move(complete));
    }

    /** @copydoc PlaySaveSceneCapture::Capture */
    Result<Runtime::CanonicalCaptureDisposition> PlaySaveSceneCapture::Capture(const Runtime::CanonicalCaptureContext &context,
                                                                               Runtime::ICanonicalCaptureSink &sink) const {
        using Outcome = Result<Runtime::CanonicalCaptureDisposition>;
        const auto scene = scenes_.ActiveScene();
        if (!scene || context.provenance.sceneIncarnation != scene->RuntimeId().value ||
            context.provenance.sceneRevision != scene->StructuralRevision())
            return Outcome::Failure(MakeError(Runtime::SaveErrors::CaptureContextInvalid));
        const auto admitted = gameplay_.lock();
        const auto bindings =
            admitted ? std::span<const Editor::PlaySaveGameplayBinding>{*admitted} : std::span<const Editor::PlaySaveGameplayBinding>{};
        auto complete = CompleteIdentities();
        if (complete.HasError())
            return Outcome::Failure(complete.ErrorValue());
        const auto state = CapturePlaySaveScene(*scene, baseline_, identities_, bindings, complete.Value() ? &*complete.Value() : nullptr);
        if (state.HasError())
            return Outcome::Failure(state.ErrorValue());
        const auto encoded = EncodePlaySaveScene(state.Value());
        if (encoded.HasError())
            return Outcome::Failure(encoded.ErrorValue());
        if (const auto copied = sink.WriteCopied(PlaySaveSceneRecord(), encoded.Value().Bytes()); copied.HasError())
            return Outcome::Failure(copied.ErrorValue());
        return Outcome::Success(Runtime::CanonicalCaptureDisposition::Captured);
    }
}  // namespace Horo::Application::Internal
