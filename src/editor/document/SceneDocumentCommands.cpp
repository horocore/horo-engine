#include "editor/document/SceneDocumentInternal.h"

namespace Horo::Editor {
    using namespace SceneDocumentDetail;

    namespace {
        [[nodiscard]] Result<void> ValidateCreateSceneObjectCommand(const std::vector<SceneObjectSnapshot> &objects,
                                                                    const CreateSceneObjectCommand &command) {
            if (!IsValidSceneObjectName(command.name))
                return Result<void>::Failure(
                    MakeDocumentError(SceneDocumentErrors::InvalidName, "Scene object name must contain 1 to 128 bytes."));
            if (!IsValid(command.localTransform))
                return Result<void>::Failure(
                    MakeDocumentError(SceneDocumentErrors::InvalidTransform, "Scene object transform must be finite."));
            if (const Result<void> descriptor = ValidateDescriptor(command.primitiveMesh); descriptor.HasError())
                return descriptor;
            if ((command.meshAsset.has_value() && !command.meshAsset->IsValid()) ||
                (command.meshAsset.has_value() && command.primitiveMesh.has_value()))
                return Result<void>::Failure(MakeDocumentError(SceneDocumentErrors::InvalidPrimitiveMetadata,
                                                               "Scene object mesh references must be valid and mutually exclusive."));
            if (const Result<void> components = ValidateComponents(command.components); components.HasError())
                return components;
            if (const Result<void> navigation = ValidateSceneNavigationComponents(objects, std::nullopt, &command.components);
                navigation.HasError())
                return navigation;
            if (command.parent.has_value() && FindObject(objects, *command.parent) == objects.end())
                return Result<void>::Failure(MakeDocumentError(SceneDocumentErrors::ParentNotFound, "Scene object parent does not exist."));
            if (command.parent.has_value() && IsEffectivelyLocked(objects, *command.parent))
                return Result<void>::Failure(LockedObjectError());
            return Result<void>::Success();
        }

        [[nodiscard]] Result<std::vector<TransformedObjectDelta>> CollectChangedTransforms(
            const std::vector<SceneObjectSnapshot> &objects, const std::vector<SceneObjectTransformUpdate> &updates) {
            std::vector<TransformedObjectDelta> changed;
            changed.reserve(updates.size());
            std::vector<SceneObjectId> seen;
            seen.reserve(updates.size());
            for (const SceneObjectTransformUpdate &update : updates) {
                if (!update.object.IsValid() || !IsValid(update.localTransform))
                    return Result<std::vector<TransformedObjectDelta>>::Failure(
                        MakeDocumentError(SceneDocumentErrors::InvalidTransform, "Batch transform values must be finite."));
                if (std::ranges::find(seen, update.object) != seen.end())
                    return Result<std::vector<TransformedObjectDelta>>::Failure(
                        MakeDocumentError(SceneDocumentErrors::InvalidTransform, "Batch transform object identities must be unique."));
                seen.push_back(update.object);
                const auto object = FindObject(objects, update.object);
                if (object == objects.end())
                    return Result<std::vector<TransformedObjectDelta>>::Failure(
                        MakeDocumentError(SceneDocumentErrors::ObjectNotFound, "Batch transform object does not exist."));
                if (IsEffectivelyLocked(objects, update.object))
                    return Result<std::vector<TransformedObjectDelta>>::Failure(LockedObjectError());
                if (object->localTransform != update.localTransform)
                    changed.emplace_back(object->id, object->localTransform, update.localTransform);
            }
            return Result<std::vector<TransformedObjectDelta>>::Success(std::move(changed));
        }

        /** @brief Describes the error reported when an editable component is absent. */
        struct ExistingComponentError final {
            const ErrorCodeDescriptor &descriptor;
            std::string_view missingMessage;
        };

        /** @brief Validates and commits a replacement for one existing typed component. */
        template <typename Component, typename Delta, typename Commit>
        [[nodiscard]] Result<SceneCommandResult> CommitExistingComponent(SceneDocument &document, std::vector<SceneObjectSnapshot> &objects,
                                                                         const SceneObjectId objectId, const Component &replacement,
                                                                         const std::optional<Component> SceneObjectComponentSet::*member,
                                                                         const ExistingComponentError &error, Commit &&commit) {
            return WithEditableObject(document, objects, objectId,
                                      [&document, &replacement, member, &error, &commit](const SceneObjectSnapshot &object) {
                const std::optional<Component> &current = object.components.*member;
                if (!current.has_value())
                    return Result<SceneCommandResult>::Failure(MakeDocumentError(error.descriptor, std::string{error.missingMessage}));
                if (*current == replacement)
                    return ComponentNoOpResult(document, object.id);
                return std::forward<Commit>(commit)(SceneCommandDelta{Delta{object.id, *current, replacement}});
            });
        }

        /** @brief Carries a validated navigation candidate and its semantic history delta. */
        struct PreparedNavigationComponents final {
            SceneObjectComponentSet candidate;
            SceneCommandDelta delta;
        };

        /** @brief Validates navigation component updates and prepares their semantic delta. */
        [[nodiscard]] Result<std::optional<PreparedNavigationComponents>> PrepareNavigationComponents(
            const std::vector<SceneObjectSnapshot> &objects, const SceneObjectSnapshot &object,
            const std::optional<Runtime::NavigationSurfaceComponent> *surface,
            const std::optional<Runtime::NavigationRegionComponent> *region,
            const std::optional<Runtime::NavigationModifierComponent> *modifier,
            const std::optional<Runtime::NavigationLinkComponent> *link,
            const std::optional<Runtime::NavigationAgentComponent> *agent) {
            SceneObjectComponentSet candidate = object.components;
            if (surface != nullptr)
                candidate.navigationSurface = *surface;
            if (region != nullptr)
                candidate.navigationRegion = *region;
            if (modifier != nullptr)
                candidate.navigationModifier = *modifier;
            if (link != nullptr)
                candidate.navigationLink = *link;
            if (agent != nullptr)
                candidate.navigationAgent = *agent;
            if (Result<void> valid = ValidateComponents(candidate); valid.HasError())
                return Result<std::optional<PreparedNavigationComponents>>::Failure(valid.ErrorValue());
            if (Result<void> valid =
                    ValidateSceneNavigationComponents(objects,
                                                      std::pair{object.id, static_cast<const SceneObjectComponentSet *>(&candidate)});
                valid.HasError())
                return Result<std::optional<PreparedNavigationComponents>>::Failure(valid.ErrorValue());
            if (candidate.navigationSurface == object.components.navigationSurface &&
                candidate.navigationRegion == object.components.navigationRegion &&
                candidate.navigationModifier == object.components.navigationModifier &&
                candidate.navigationLink == object.components.navigationLink &&
                candidate.navigationAgent == object.components.navigationAgent)
                return Result<std::optional<PreparedNavigationComponents>>::Success(std::nullopt);

            SceneCommandDelta delta = NavigationComponentsChangedDelta{
                .object = object.id,
                .surfaceBefore = object.components.navigationSurface,
                .surfaceAfter = candidate.navigationSurface,
                .regionBefore = object.components.navigationRegion,
                .regionAfter = candidate.navigationRegion,
                .modifierBefore = object.components.navigationModifier,
                .modifierAfter = candidate.navigationModifier,
                .linkBefore = object.components.navigationLink,
                .linkAfter = candidate.navigationLink,
                .agentBefore = object.components.navigationAgent,
                .agentAfter = candidate.navigationAgent,
            };
            return Result<std::optional<PreparedNavigationComponents>>::Success(
                PreparedNavigationComponents{std::move(candidate), std::move(delta)});
        }
    }  // namespace

    /** @copydoc SceneDocumentCommandExecutor::SceneDocumentCommandExecutor */
    SceneDocumentCommandExecutor::SceneDocumentCommandExecutor(SceneDocument &document, EditorHistory &history) noexcept
        : m_document(document), m_history(history) {}

    /** @copydoc SceneDocumentCommandExecutor::CommitObject */
    Result<SceneCommandResult> SceneDocumentCommandExecutor::CommitObject(ObjectCommitContext context) {
        const std::size_t memoryBytes = EstimateMemoryBytes(context.delta, 1);
        const DocumentStateId beforeState = m_document.m_state;
        ApplyDelta(m_document.m_objects, context.delta);
        ++m_document.m_revision.value;
        m_document.m_state = DocumentStateId{m_document.m_nextStateId++};
        std::vector affected{context.object};
        PushHistory(*m_history.m_impl, HistoryRecord{beforeState, m_document.m_state, std::move(context.delta), affected, memoryBytes});
        return Result<SceneCommandResult>::Success(
            {context.object, m_document.m_revision, m_document.m_state, context.kind, std::move(affected), true});
    }

    /** @copydoc SceneDocumentCommandExecutor::CommitPrefab */
    Result<SceneCommandResult> SceneDocumentCommandExecutor::CommitPrefab(PrefabCommitContext context) {
        if (const Result<void> validHistory = ValidateHistoryDelta(context.delta, 1); validHistory.HasError())
            return Result<SceneCommandResult>::Failure(validHistory.ErrorValue());

        const std::size_t memoryBytes = EstimateMemoryBytes(context.delta, 1);
        const DocumentStateId beforeState = m_document.m_state;
        ApplyDelta(m_document.m_objects, m_document.m_prefabInstances, context.delta);
        if (context.advanceInstanceId)
            ++m_document.m_nextPrefabInstanceId;
        ++m_document.m_revision.value;
        m_document.m_state = DocumentStateId{m_document.m_nextStateId++};

        std::vector affected{context.instance};
        PushHistory(*m_history.m_impl, HistoryRecord{beforeState, m_document.m_state, std::move(context.delta), {}, memoryBytes, affected});
        SceneCommandResult result{{}, m_document.m_revision, m_document.m_state, context.kind, {}, true};
        result.prefabInstance = context.instance;
        result.affectedPrefabInstances = std::move(affected);
        return Result<SceneCommandResult>::Success(std::move(result));
    }

    /** @copydoc SceneDocumentCommandExecutor::Execute(const CreateSceneObjectCommand&) */
    Result<SceneCommandResult> SceneDocumentCommandExecutor::Execute(const CreateSceneObjectCommand &command) {
        if (const Result<void> valid = ValidateCreateSceneObjectCommand(m_document.m_objects, command); valid.HasError())
            return Result<SceneCommandResult>::Failure(valid.ErrorValue());

        const SceneObjectId id{m_document.m_nextObjectId};
        SceneCommandDelta delta = CreatedObjectDelta{
            .object = SceneObjectSnapshot{.id = id,
                                          .parent = command.parent,
                                          .name = command.name,
                                          .localTransform = command.localTransform,
                                          .primitiveMesh = command.primitiveMesh,
                                          .components = command.components,
                                          .meshAsset = command.meshAsset},
            .index = m_document.m_objects.size(),
            .kind = DocumentChangeKind::Created,
        };
        if (const Result<void> validHistory = ValidateHistoryDelta(delta, 1); validHistory.HasError()) {
            return Result<SceneCommandResult>::Failure(validHistory.ErrorValue());
        }
        ObserveNavigationComponentIds(command.components, m_document.m_nextNavigationSurfaceId, m_document.m_nextNavigationRegionId,
                                      m_document.m_nextNavigationModifierId, m_document.m_nextNavigationLinkId);
        ++m_document.m_nextObjectId;
        return CommitObject({std::move(delta), id, DocumentChangeKind::Created});
    }

    /** @copydoc SceneDocumentCommandExecutor::Execute(const RenameSceneObjectCommand&) */
    Result<SceneCommandResult> SceneDocumentCommandExecutor::Execute(const RenameSceneObjectCommand &command) {
        if (!IsValidSceneObjectName(command.name)) {
            return Result<SceneCommandResult>::Failure(
                MakeDocumentError(SceneDocumentErrors::InvalidName, "Scene object name must contain 1 to 128 bytes."));
        }
        const auto object = FindObject(m_document.m_objects, command.object);
        if (object == m_document.m_objects.end()) {
            return Result<SceneCommandResult>::Failure(
                MakeDocumentError(SceneDocumentErrors::ObjectNotFound, "Scene object does not exist."));
        }
        if (IsEffectivelyLocked(m_document.m_objects, command.object))
            return Result<SceneCommandResult>::Failure(LockedObjectError());
        if (object->name == command.name) {
            return Result<SceneCommandResult>::Success(
                SceneCommandResult{object->id, m_document.m_revision, m_document.m_state, DocumentChangeKind::Renamed, {}, false});
        }

        SceneCommandDelta delta = RenamedObjectDelta{object->id, object->name, command.name};
        const std::size_t memoryBytes = EstimateMemoryBytes(delta, 1);
        if (const Result<void> validHistory = ValidateHistoryDelta(delta, 1); validHistory.HasError()) {
            return Result<SceneCommandResult>::Failure(validHistory.ErrorValue());
        }
        const DocumentStateId beforeState = m_document.m_state;
        ApplyDelta(m_document.m_objects, delta);
        ++m_document.m_revision.value;
        m_document.m_state = DocumentStateId{m_document.m_nextStateId++};
        std::vector affected{object->id};
        PushHistory(*m_history.m_impl, HistoryRecord{beforeState, m_document.m_state, std::move(delta), affected, memoryBytes});
        return Result<SceneCommandResult>::Success(SceneCommandResult{command.object, m_document.m_revision, m_document.m_state,
                                                                      DocumentChangeKind::Renamed, std::move(affected), true});
    }

    /** @copydoc SceneDocumentCommandExecutor::Execute(const SetSceneObjectTransformCommand&) */
    Result<SceneCommandResult> SceneDocumentCommandExecutor::Execute(const SetSceneObjectTransformCommand &command) {
        return Execute(SetSceneObjectTransformsCommand{{
            SceneObjectTransformUpdate{.object = command.object, .localTransform = command.localTransform},
        }});
    }

    /** @copydoc SceneDocumentCommandExecutor::Execute(const SetSceneObjectTransformsCommand&) */
    Result<SceneCommandResult> SceneDocumentCommandExecutor::Execute(const SetSceneObjectTransformsCommand &command) {
        if (command.updates.empty()) {
            return Result<SceneCommandResult>::Success(
                SceneCommandResult{{}, m_document.m_revision, m_document.m_state, DocumentChangeKind::TransformChanged, {}, false});
        }

        auto collected = CollectChangedTransforms(m_document.m_objects, command.updates);
        if (collected.HasError())
            return Result<SceneCommandResult>::Failure(collected.ErrorValue());
        std::vector<TransformedObjectDelta> changed = std::move(collected).Value();
        if (changed.empty()) {
            return Result<SceneCommandResult>::Success(SceneCommandResult{command.updates.front().object,
                                                                          m_document.m_revision,
                                                                          m_document.m_state,
                                                                          DocumentChangeKind::TransformChanged,
                                                                          {},
                                                                          false});
        }

        SceneCommandDelta delta = TransformedObjectsDelta{std::move(changed)};
        const std::vector<TransformedObjectDelta> &deltaObjects = std::get<TransformedObjectsDelta>(delta).objects;
        std::vector<SceneObjectId> affected;
        affected.reserve(deltaObjects.size());
        for (const TransformedObjectDelta &object : deltaObjects) {
            affected.push_back(object.object);
        }
        if (const Result<void> validHistory = ValidateHistoryDelta(delta, affected.size()); validHistory.HasError()) {
            return Result<SceneCommandResult>::Failure(validHistory.ErrorValue());
        }
        const std::size_t memoryBytes = EstimateMemoryBytes(delta, affected.size());
        const DocumentStateId beforeState = m_document.m_state;
        ApplyDelta(m_document.m_objects, delta);
        ++m_document.m_revision.value;
        m_document.m_state = DocumentStateId{m_document.m_nextStateId++};
        const SceneObjectId primary = affected.front();
        PushHistory(*m_history.m_impl, HistoryRecord{beforeState, m_document.m_state, std::move(delta), affected, memoryBytes});
        return Result<SceneCommandResult>::Success(SceneCommandResult{primary, m_document.m_revision, m_document.m_state,
                                                                      DocumentChangeKind::TransformChanged, std::move(affected), true});
    }

    /** @copydoc SceneDocumentCommandExecutor::Execute(const SetSceneObjectCameraCommand&) */
    Result<SceneCommandResult> SceneDocumentCommandExecutor::Execute(const SetSceneObjectCameraCommand &command) {
        if (!IsValidCameraComponent(command.camera)) {
            return Result<SceneCommandResult>::Failure(
                MakeDocumentError(SceneDocumentErrors::InvalidCamera, "Camera authoring values are invalid."));
        }
        return CommitExistingComponent<Runtime::CameraComponent,
                                       CameraChangedDelta>(m_document, m_document.m_objects, command.object, command.camera,
                                                           &SceneObjectComponentSet::camera,
                                                           ExistingComponentError{SceneDocumentErrors::InvalidCamera,
                                                                                  "Scene object has no camera component."},
                                                           [this, objectId = command.object](SceneCommandDelta delta) {
            return CommitObject({std::move(delta), objectId, DocumentChangeKind::ComponentChanged});
        });
    }

    /** @copydoc SceneDocumentCommandExecutor::Execute(const SetSceneObjectLightCommand&) */
    Result<SceneCommandResult> SceneDocumentCommandExecutor::Execute(const SetSceneObjectLightCommand &command) {
        if (!IsValidLightComponent(command.light)) {
            return Result<SceneCommandResult>::Failure(
                MakeDocumentError(SceneDocumentErrors::InvalidLight, "Light authoring values are invalid."));
        }
        return CommitExistingComponent<Runtime::LightComponent,
                                       LightChangedDelta>(m_document, m_document.m_objects, command.object, command.light,
                                                          &SceneObjectComponentSet::light,
                                                          ExistingComponentError{SceneDocumentErrors::InvalidLight,
                                                                                 "Scene object has no light component."},
                                                          [this, objectId = command.object](SceneCommandDelta delta) {
            return CommitObject({std::move(delta), objectId, DocumentChangeKind::ComponentChanged});
        });
    }

    /** @copydoc SceneDocumentCommandExecutor::Execute(const SetSceneObjectTriggerVolumeCommand&) */
    Result<SceneCommandResult> SceneDocumentCommandExecutor::Execute(const SetSceneObjectTriggerVolumeCommand &command) {
        return CommitExistingComponent<Runtime::TriggerVolumeComponent,
                                       TriggerVolumeChangedDelta>(m_document, m_document.m_objects, command.object, command.triggerVolume,
                                                                  &SceneObjectComponentSet::triggerVolume,
                                                                  ExistingComponentError{SceneDocumentErrors::InvalidTriggerVolume,
                                                                                         "Scene object has no trigger volume component."},
                                                                  [this, objectId = command.object](SceneCommandDelta delta) {
            return CommitObject({std::move(delta), objectId, DocumentChangeKind::ComponentChanged});
        });
    }

    /** @copydoc SceneDocumentCommandExecutor::Execute(const SetSceneObjectAudioSourceCommand&) */
    Result<SceneCommandResult> SceneDocumentCommandExecutor::Execute(const SetSceneObjectAudioSourceCommand &command) {
        if (!IsValidAudioSourceComponent(command.audioSource)) {
            return Result<SceneCommandResult>::Failure(
                MakeDocumentError(SceneDocumentErrors::InvalidAudioSource, "Audio source reference or playback values are invalid."));
        }
        return CommitExistingComponent<Runtime::AudioSourceComponent,
                                       AudioSourceChangedDelta>(m_document, m_document.m_objects, command.object, command.audioSource,
                                                                &SceneObjectComponentSet::audioSource,
                                                                ExistingComponentError{SceneDocumentErrors::InvalidAudioSource,
                                                                                       "Scene object has no audio source component."},
                                                                [this, objectId = command.object](SceneCommandDelta delta) {
            return CommitObject({std::move(delta), objectId, DocumentChangeKind::ComponentChanged});
        });
    }

    /** @copydoc SceneDocumentCommandExecutor::CommitNavigationComponents */
    Result<SceneCommandResult> SceneDocumentCommandExecutor::CommitNavigationComponents(
        const SceneObjectId objectId, const std::optional<Runtime::NavigationSurfaceComponent> *surface,
        const std::optional<Runtime::NavigationRegionComponent> *region,
        const std::optional<Runtime::NavigationModifierComponent> *modifier, const std::optional<Runtime::NavigationLinkComponent> *link,
        const std::optional<Runtime::NavigationAgentComponent> *agent) {
        return WithEditableObject(m_document, m_document.m_objects, objectId,
                                  [this, surface, region, modifier, link, agent](const SceneObjectSnapshot &object) {
            auto prepared = PrepareNavigationComponents(m_document.m_objects, object, surface, region, modifier, link, agent);
            if (prepared.HasError())
                return Result<SceneCommandResult>::Failure(prepared.ErrorValue());
            if (!prepared.Value().has_value())
                return Result<SceneCommandResult>::Success(
                    {object.id, m_document.m_revision, m_document.m_state, DocumentChangeKind::ComponentChanged, {}, false});
            PreparedNavigationComponents navigation = std::move(prepared).Value().value();
            ObserveNavigationComponentIds(navigation.candidate, m_document.m_nextNavigationSurfaceId, m_document.m_nextNavigationRegionId,
                                          m_document.m_nextNavigationModifierId, m_document.m_nextNavigationLinkId);
            return CommitObject({std::move(navigation.delta), object.id, DocumentChangeKind::ComponentChanged});
        });
    }

    /** @copydoc SceneDocumentCommandExecutor::Execute(const SetSceneNavigationSurfaceCommand&) */
    Result<SceneCommandResult> SceneDocumentCommandExecutor::Execute(const SetSceneNavigationSurfaceCommand &command) {
        return CommitNavigationComponents(command.object, &command.surface, nullptr, nullptr, nullptr, nullptr);
    }

    /** @copydoc SceneDocumentCommandExecutor::Execute(const SetSceneNavigationRegionCommand&) */
    Result<SceneCommandResult> SceneDocumentCommandExecutor::Execute(const SetSceneNavigationRegionCommand &command) {
        return CommitNavigationComponents(command.object, nullptr, &command.region, nullptr, nullptr, nullptr);
    }

    /** @copydoc SceneDocumentCommandExecutor::Execute(const SetSceneNavigationModifierCommand&) */
    Result<SceneCommandResult> SceneDocumentCommandExecutor::Execute(const SetSceneNavigationModifierCommand &command) {
        return CommitNavigationComponents(command.object, nullptr, nullptr, &command.modifier, nullptr, nullptr);
    }

    /** @copydoc SceneDocumentCommandExecutor::Execute(const SetSceneNavigationLinkCommand&) */
    Result<SceneCommandResult> SceneDocumentCommandExecutor::Execute(const SetSceneNavigationLinkCommand &command) {
        return CommitNavigationComponents(command.object, nullptr, nullptr, nullptr, &command.link, nullptr);
    }

    /** @copydoc SceneDocumentCommandExecutor::Execute(const SetSceneNavigationAgentCommand&) */
    Result<SceneCommandResult> SceneDocumentCommandExecutor::Execute(const SetSceneNavigationAgentCommand &command) {
        return CommitNavigationComponents(command.object, nullptr, nullptr, nullptr, nullptr, &command.agent);
    }

}  // namespace Horo::Editor
