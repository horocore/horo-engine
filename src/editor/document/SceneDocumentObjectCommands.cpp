#include "Horo/Gameplay/GameplayErrors.h"
#include "editor/document/SceneDocumentInternal.h"

namespace Horo::Editor {
    using namespace SceneDocumentDetail;

    namespace {
        [[nodiscard]] Result<void> ConfigurePrimitiveCreationCommand(const Runtime::PrimitiveDescriptor &descriptor,
                                                                     CreateSceneObjectCommand &command) {
            if (descriptor.meshType.has_value()) {
                command.primitiveMesh = PrimitiveMeshDescriptor::Defaults(*descriptor.meshType);
                return Result<void>::Success();
            }
            if (!descriptor.sceneObjectType.has_value())
                return Result<void>::Failure(MakeDocumentError(SceneDocumentErrors::InvalidPrimitiveMetadata,
                                                               "Creatable primitive has no typed authoring descriptor."));
            switch (*descriptor.sceneObjectType) {
                using enum Horo::Runtime::SceneObjectPrimitiveType;
                case Runtime::SceneObjectPrimitiveType::Empty:
                    break;
                case Runtime::SceneObjectPrimitiveType::Camera:
                    command.components.camera = Runtime::CameraComponent{};
                    break;
                case Runtime::SceneObjectPrimitiveType::DirectionalLight:
                    command.components.light = Runtime::LightComponent{.kind = Runtime::LightKind::Directional};
                    break;
                case Runtime::SceneObjectPrimitiveType::PointLight:
                    command.components.light = Runtime::LightComponent{.kind = Runtime::LightKind::Point};
                    break;
                case Runtime::SceneObjectPrimitiveType::SpotLight:
                    command.components.light = Runtime::LightComponent{.kind = Runtime::LightKind::Spot};
                    break;
                case Runtime::SceneObjectPrimitiveType::TriggerVolume:
                    command.components.triggerVolume = Runtime::TriggerVolumeComponent{};
                    break;
                case Runtime::SceneObjectPrimitiveType::AudioSource:
                    command.components.audioSource = Runtime::AudioSourceComponent{};
                    break;
            }
            return Result<void>::Success();
        }

        /** @brief Validates one behavior attachment and prepares its semantic delta. */
        [[nodiscard]] Result<SceneCommandDelta> PrepareBehaviorAttachment(const SceneObjectSnapshot &object,
                                                                          const AttachSceneObjectBehaviorCommand &command,
                                                                          const std::uint64_t nextBehaviorInstanceId) {
            if (!command.typeId.IsValid() || command.schemaVersion == 0 ||
                (!command.allowMultiple &&
                 std::ranges::any_of(object.components.behaviors, [typeId = command.typeId](const auto &behavior) {
                return behavior.typeId == typeId;
            }))) {
                return Result<SceneCommandDelta>::Failure(
                    MakeDocumentError(SceneDocumentErrors::InvalidBehavior,
                                      "Behavior type cannot be attached more than once to this object."));
            }
            Gameplay::BehaviorComponent behavior{Gameplay::BehaviorInstanceId{nextBehaviorInstanceId}, command.typeId,
                                                 command.schemaVersion, command.enabled, command.fields};
            if (Gameplay::ValidateBehaviorComponent(behavior).HasError())
                return Result<SceneCommandDelta>::Failure(
                    MakeDocumentError(SceneDocumentErrors::InvalidBehavior, "Behavior attachment payload is invalid."));
            auto after = object.components.behaviors;
            after.push_back(std::move(behavior));
            SceneCommandDelta delta = BehaviorsChangedDelta{object.id, object.components.behaviors, std::move(after)};
            if (const Result<void> valid = ValidateHistoryDelta(delta, 1); valid.HasError())
                return Result<SceneCommandDelta>::Failure(valid.ErrorValue());
            return Result<SceneCommandDelta>::Success(std::move(delta));
        }

        /** @brief Prepares one validated opaque gameplay-component update or reports a no-op. */
        [[nodiscard]] Result<std::optional<SceneCommandDelta>> PrepareGameplayComponentDelta(
            const SceneObjectSnapshot &object, const SetSceneObjectGameplayComponentCommand &command) {
            auto after = object.components.gameplayComponents;
            if (const auto existing = std::ranges::find(after, command.component.typeId, &Gameplay::SerializedComponent::typeId);
                existing != after.end()) {
                if (*existing == command.component)
                    return Result<std::optional<SceneCommandDelta>>::Success(std::nullopt);
                *existing = command.component;
            } else {
                after.push_back(command.component);
            }
            std::ranges::sort(after, {}, [](const Gameplay::SerializedComponent &component) {
                return component.typeId.Value();
            });

            SceneObjectComponentSet candidate = object.components;
            candidate.gameplayComponents = after;
            if (const Result<void> valid = ValidateComponents(candidate); valid.HasError())
                return Result<std::optional<SceneCommandDelta>>::Failure(valid.ErrorValue());
            return Result<std::optional<SceneCommandDelta>>::Success(
                SceneCommandDelta{GameplayComponentsChangedDelta{object.id, object.components.gameplayComponents, std::move(after)}});
        }
    }  // namespace

    /** @copydoc SceneDocumentCommandExecutor::Execute(const SetSceneObjectEditorStateCommand&) */
    Result<SceneCommandResult> SceneDocumentCommandExecutor::Execute(const SetSceneObjectEditorStateCommand &command) {
        const auto object = FindObject(m_document.m_objects, command.object);
        if (object == m_document.m_objects.end())
            return Result<SceneCommandResult>::Failure(
                MakeDocumentError(SceneDocumentErrors::ObjectNotFound, "Scene object does not exist."));
        if (object->editorState == command.editorState)
            return Result<SceneCommandResult>::Success(
                {command.object, m_document.m_revision, m_document.m_state, DocumentChangeKind::EditorStateChanged, {}, false});

        return CommitObject({EditorStateChangedDelta{object->id, object->editorState, command.editorState}, command.object,
                             DocumentChangeKind::EditorStateChanged});
    }

    /** @copydoc SceneDocumentCommandExecutor::Execute(const AddSceneObjectComponentCommand&) */
    Result<SceneCommandResult> SceneDocumentCommandExecutor::Execute(const AddSceneObjectComponentCommand &command) {
        return WithEditableObject(m_document, m_document.m_objects, command.object, [this, &command](const SceneObjectSnapshot &object) {
            if (HasComponent(object.components, command.type))
                return ComponentNoOpResult(m_document, object.id);
            return CommitObject({ComponentAddedDelta{object.id, command.type}, command.object, DocumentChangeKind::ComponentChanged});
        });
    }

    /** @copydoc SceneDocumentCommandExecutor::Execute(const RemoveSceneObjectComponentCommand&) */
    Result<SceneCommandResult> SceneDocumentCommandExecutor::Execute(const RemoveSceneObjectComponentCommand &command) {
        return WithEditableObject(m_document, m_document.m_objects, command.object, [this, &command](const SceneObjectSnapshot &object) {
            if (!HasComponent(object.components, command.type))
                return ComponentNoOpResult(m_document, object.id);
            return CommitObject({ComponentRemovedDelta{object.id, command.type, object.components.camera, object.components.light,
                                                       object.components.triggerVolume, object.components.audioSource},
                                 command.object, DocumentChangeKind::ComponentChanged});
        });
    }

    /** @copydoc SceneDocumentCommandExecutor::Execute(const AttachSceneObjectBehaviorCommand&) */
    Result<SceneCommandResult> SceneDocumentCommandExecutor::Execute(const AttachSceneObjectBehaviorCommand &command) {
        return WithEditableObject(m_document, m_document.m_objects, command.object, [this, &command](const SceneObjectSnapshot &object) {
            auto prepared = PrepareBehaviorAttachment(object, command, m_document.m_nextBehaviorInstanceId);
            if (prepared.HasError())
                return Result<SceneCommandResult>::Failure(prepared.ErrorValue());
            ++m_document.m_nextBehaviorInstanceId;
            return CommitObject({std::move(prepared).Value(), command.object, DocumentChangeKind::ComponentChanged});
        });
    }

    /** @copydoc SceneDocumentCommandExecutor::Execute(const SetSceneObjectBehaviorCommand&) */
    Result<SceneCommandResult> SceneDocumentCommandExecutor::Execute(const SetSceneObjectBehaviorCommand &command) {
        if (Gameplay::ValidateBehaviorComponent(command.behavior).HasError())
            return Result<SceneCommandResult>::Failure(
                MakeDocumentError(SceneDocumentErrors::InvalidBehavior, "Behavior replacement payload is invalid."));
        return WithEditableObject(m_document, m_document.m_objects, command.object, [this, &command](const SceneObjectSnapshot &object) {
            const auto behavior =
                std::ranges::find(object.components.behaviors, command.behavior.instanceId, &Gameplay::BehaviorComponent::instanceId);
            if (behavior == object.components.behaviors.end() || behavior->typeId != command.behavior.typeId)
                return Result<SceneCommandResult>::Failure(
                    MakeDocumentError(SceneDocumentErrors::InvalidBehavior, "Behavior attachment does not exist or changed type."));
            if (*behavior == command.behavior)
                return Result<SceneCommandResult>::Success(
                    {command.object, m_document.m_revision, m_document.m_state, DocumentChangeKind::ComponentChanged, {}, false});
            auto after = object.components.behaviors;
            *std::ranges::find(after, command.behavior.instanceId, &Gameplay::BehaviorComponent::instanceId) = command.behavior;
            SceneCommandDelta delta = BehaviorsChangedDelta{object.id, object.components.behaviors, std::move(after)};
            if (const Result<void> valid = ValidateHistoryDelta(delta, 1); valid.HasError())
                return Result<SceneCommandResult>::Failure(valid.ErrorValue());
            return CommitObject({std::move(delta), command.object, DocumentChangeKind::ComponentChanged});
        });
    }

    /** @copydoc SceneDocumentCommandExecutor::Execute(const RemoveSceneObjectBehaviorCommand&) */
    Result<SceneCommandResult> SceneDocumentCommandExecutor::Execute(const RemoveSceneObjectBehaviorCommand &command) {
        return WithEditableObject(m_document, m_document.m_objects, command.object, [this, &command](const SceneObjectSnapshot &object) {
            auto after = object.components.behaviors;
            if (const auto removed = std::erase_if(after,
                                                   [behaviorId = command.behavior](const Gameplay::BehaviorComponent &behavior) {
                return behavior.instanceId == behaviorId;
            });
                removed == 0)
                return Result<SceneCommandResult>::Failure(
                    MakeDocumentError(SceneDocumentErrors::InvalidBehavior, "Behavior attachment does not exist."));
            return CommitObject({BehaviorsChangedDelta{object.id, object.components.behaviors, std::move(after)}, command.object,
                                 DocumentChangeKind::ComponentChanged});
        });
    }

    /** @copydoc SceneDocumentCommandExecutor::Execute(const SetSceneObjectGameplayComponentCommand&) */
    Result<SceneCommandResult> SceneDocumentCommandExecutor::Execute(const SetSceneObjectGameplayComponentCommand &command) {
        if (const Result<void> valid = Gameplay::ValidateSerializedComponent(command.component); valid.HasError())
            return Result<SceneCommandResult>::Failure(valid.ErrorValue());
        return WithEditableObject(m_document, m_document.m_objects, command.object, [this, &command](const SceneObjectSnapshot &object) {
            auto prepared = PrepareGameplayComponentDelta(object, command);
            if (prepared.HasError())
                return Result<SceneCommandResult>::Failure(prepared.ErrorValue());
            std::optional<SceneCommandDelta> delta = std::move(prepared).Value();
            if (!delta)
                return ComponentNoOpResult(m_document, object.id);
            if (const Result<void> validHistory = ValidateHistoryDelta(*delta, 1); validHistory.HasError())
                return Result<SceneCommandResult>::Failure(validHistory.ErrorValue());
            return CommitObject({std::move(*delta), command.object, DocumentChangeKind::ComponentChanged});
        });
    }

    /** @copydoc SceneDocumentCommandExecutor::Execute(const RemoveSceneObjectGameplayComponentCommand&) */
    Result<SceneCommandResult> SceneDocumentCommandExecutor::Execute(const RemoveSceneObjectGameplayComponentCommand &command) {
        if (!command.typeId.IsValid())
            return Result<SceneCommandResult>::Failure(
                MakeError(Gameplay::GameplayErrors::InvalidComponentTypeId, "Gameplay component type ID is invalid."));
        return WithEditableObject(m_document, m_document.m_objects, command.object, [this, &command](const SceneObjectSnapshot &object) {
            auto after = object.components.gameplayComponents;
            if (const auto removed = std::erase_if(after,
                                                   [&command](const Gameplay::SerializedComponent &component) {
                return component.typeId == command.typeId;
            });
                removed == 0)
                return ComponentNoOpResult(m_document, object.id);

            SceneCommandDelta delta = GameplayComponentsChangedDelta{object.id, object.components.gameplayComponents, std::move(after)};
            if (const Result<void> validHistory = ValidateHistoryDelta(delta, 1); validHistory.HasError())
                return Result<SceneCommandResult>::Failure(validHistory.ErrorValue());
            return CommitObject({std::move(delta), command.object, DocumentChangeKind::ComponentChanged});
        });
    }

    /** @copydoc SceneDocumentCommandExecutor::Execute(const DuplicateSceneObjectCommand&) */
    Result<SceneCommandResult> SceneDocumentCommandExecutor::Execute(const DuplicateSceneObjectCommand &command) {
        if (!IsValidSceneObjectName(command.name)) {
            return Result<SceneCommandResult>::Failure(
                MakeDocumentError(SceneDocumentErrors::InvalidName, "Scene object name must contain 1 to 128 bytes."));
        }
        const auto source = FindObject(m_document.m_objects, command.source);
        if (source == m_document.m_objects.end()) {
            return Result<SceneCommandResult>::Failure(
                MakeDocumentError(SceneDocumentErrors::ObjectNotFound, "Scene object does not exist."));
        }
        if (IsEffectivelyLocked(m_document.m_objects, command.source))
            return Result<SceneCommandResult>::Failure(LockedObjectError());

        const SceneObjectId id{m_document.m_nextObjectId};
        SceneObjectComponentSet duplicatedComponents = source->components;
        if (Result<void> regenerated =
                RegenerateDuplicatedNavigationIdentities(duplicatedComponents, m_document.m_nextNavigationSurfaceId,
                                                         m_document.m_nextNavigationRegionId, m_document.m_nextNavigationModifierId,
                                                         m_document.m_nextNavigationLinkId);
            regenerated.HasError())
            return Result<SceneCommandResult>::Failure(regenerated.ErrorValue());
        if (Result<void> navigation = ValidateSceneNavigationComponents(m_document.m_objects, std::nullopt, &duplicatedComponents);
            navigation.HasError()) {
            return Result<SceneCommandResult>::Failure(navigation.ErrorValue());
        }
        for (Gameplay::BehaviorComponent &behavior : duplicatedComponents.behaviors)
            behavior.instanceId = Gameplay::BehaviorInstanceId{m_document.m_nextBehaviorInstanceId++};
        ObserveNavigationComponentIds(duplicatedComponents, m_document.m_nextNavigationSurfaceId, m_document.m_nextNavigationRegionId,
                                      m_document.m_nextNavigationModifierId, m_document.m_nextNavigationLinkId);
        SceneCommandDelta delta = CreatedObjectDelta{
            .object = SceneObjectSnapshot{.id = id,
                                          .parent = source->parent,
                                          .name = command.name,
                                          .localTransform = source->localTransform,
                                          .primitiveMesh = source->primitiveMesh,
                                          .components = std::move(duplicatedComponents),
                                          .meshAsset = source->meshAsset,
                                          .editorState = source->editorState},
            .index = m_document.m_objects.size(),
            .kind = DocumentChangeKind::Duplicated,
        };
        ++m_document.m_nextObjectId;
        return CommitObject({std::move(delta), id, DocumentChangeKind::Duplicated});
    }

    /** @copydoc SceneDocumentCommandExecutor::Execute(const DeleteSceneObjectCommand&) */
    Result<SceneCommandResult> SceneDocumentCommandExecutor::Execute(const DeleteSceneObjectCommand &command) {
        return Execute(DeleteSceneObjectsCommand{{command.object}});
    }

    /** @copydoc SceneDocumentCommandExecutor::Execute(const DeleteSceneObjectsCommand&) */
    Result<SceneCommandResult> SceneDocumentCommandExecutor::Execute(const DeleteSceneObjectsCommand &command) {
        const std::vector<SceneObjectId> selected = SelectExistingObjects(m_document.m_objects, command.objects);
        if (selected.empty()) {
            return Result<SceneCommandResult>::Failure(
                MakeDocumentError(SceneDocumentErrors::ObjectNotFound, "No requested scene object exists in the active document."));
        }

        std::vector<SceneObjectId> roots = DeletionRoots(m_document.m_objects, selected);
        std::unordered_set<std::uint64_t> removedIds;
        std::vector<SceneObjectId> removed = CollectRemovedObjects(m_document.m_objects, roots, removedIds);
        if (std::ranges::any_of(removed, [&](const SceneObjectId object) {
            return IsEffectivelyLocked(m_document.m_objects, object);
        }))
            return Result<SceneCommandResult>::Failure(LockedObjectError());
        if (Result<void> navigation = ValidateRemainingNavigation(m_document.m_objects, removedIds); navigation.HasError()) {
            return Result<SceneCommandResult>::Failure(navigation.ErrorValue());
        }
        const SceneObjectId primary = roots.front();
        SceneCommandDelta delta = CaptureDeletedObjects(m_document.m_objects, m_document.m_prefabInstances, std::move(roots), removedIds);
        const auto &deleted = std::get<DeletedObjectsDelta>(delta);
        std::vector<Prefab::PrefabInstanceId> affectedPrefabInstances;
        affectedPrefabInstances.reserve(deleted.prefabInstances.size());
        for (const IndexedPrefabInstance &instance : deleted.prefabInstances)
            affectedPrefabInstances.push_back(instance.instance.instanceId);
        if (const Result<void> validHistory = ValidateHistoryDelta(delta, removed.size() + affectedPrefabInstances.size());
            validHistory.HasError()) {
            return Result<SceneCommandResult>::Failure(validHistory.ErrorValue());
        }
        const std::size_t memoryBytes = EstimateMemoryBytes(delta, removed.size() + affectedPrefabInstances.size());
        const DocumentStateId beforeState = m_document.m_state;
        ApplyDelta(m_document.m_objects, m_document.m_prefabInstances, delta);
        ++m_document.m_revision.value;
        m_document.m_state = DocumentStateId{m_document.m_nextStateId++};
        PushHistory(*m_history.m_impl,
                    HistoryRecord{beforeState, m_document.m_state, std::move(delta), removed, memoryBytes, affectedPrefabInstances});
        SceneCommandResult result{primary, m_document.m_revision, m_document.m_state, DocumentChangeKind::Deleted, std::move(removed),
                                  true};
        result.affectedPrefabInstances = std::move(affectedPrefabInstances);
        return Result<SceneCommandResult>::Success(std::move(result));
    }

    /** @copydoc SceneDocumentCommandExecutor::Execute(const CreateScenePrefabInstanceCommand&) */
    Result<SceneCommandResult> SceneDocumentCommandExecutor::Execute(const CreateScenePrefabInstanceCommand &command) {
        if (!command.sourcePrefab.IsValid()) {
            return Result<SceneCommandResult>::Failure(
                MakeDocumentError(SceneDocumentErrors::InvalidPrefabInstance, "Prefab placement requires a valid asset reference."));
        }
        if (!IsValid(command.rootTransform)) {
            return Result<SceneCommandResult>::Failure(
                MakeDocumentError(SceneDocumentErrors::InvalidTransform, "Prefab root placement transform must be finite."));
        }
        if (const Result<void> validParent = ValidatePrefabParent(m_document.m_objects, command.parent); validParent.HasError())
            return Result<SceneCommandResult>::Failure(validParent.ErrorValue());
        auto instanceId = AllocatePrefabInstanceId(m_document.m_nextPrefabInstanceId);
        if (instanceId.HasError())
            return Result<SceneCommandResult>::Failure(instanceId.ErrorValue());
        SceneCommandDelta delta = CreatedPrefabInstanceDelta{
            .instance = ScenePrefabInstance{instanceId.Value(), command.sourcePrefab, command.parent, command.rootTransform},
            .index = m_document.m_prefabInstances.size(),
        };
        return CommitPrefab(PrefabCommitContext{std::move(delta), instanceId.Value(), DocumentChangeKind::PrefabInstanceCreated, true});
    }

    /** @copydoc SceneDocumentCommandExecutor::Execute(const SetScenePrefabInstanceRootTransformCommand&) */
    Result<SceneCommandResult> SceneDocumentCommandExecutor::Execute(const SetScenePrefabInstanceRootTransformCommand &command) {
        if (!IsValid(command.rootTransform)) {
            return Result<SceneCommandResult>::Failure(
                MakeDocumentError(SceneDocumentErrors::InvalidTransform, "Prefab root placement transform must be finite."));
        }
        auto instance = FindEditablePrefabInstance(m_document.m_objects, m_document.m_prefabInstances, command.instance);
        if (instance.HasError())
            return Result<SceneCommandResult>::Failure(instance.ErrorValue());
        if (instance.Value()->rootTransform == command.rootTransform)
            return Result<SceneCommandResult>::Success(PrefabNoOpResult(m_document.m_revision, m_document.m_state, command.instance,
                                                                        DocumentChangeKind::PrefabInstanceTransformChanged));
        SceneCommandDelta delta = PrefabInstanceTransformDelta{command.instance, instance.Value()->rootTransform, command.rootTransform};
        return CommitPrefab(PrefabCommitContext{std::move(delta), command.instance, DocumentChangeKind::PrefabInstanceTransformChanged});
    }

    /** @copydoc SceneDocumentCommandExecutor::Execute(const DuplicateScenePrefabInstanceCommand&) */
    Result<SceneCommandResult> SceneDocumentCommandExecutor::Execute(const DuplicateScenePrefabInstanceCommand &command) {
        auto source = FindEditablePrefabInstance(m_document.m_objects, m_document.m_prefabInstances, command.source);
        if (source.HasError())
            return Result<SceneCommandResult>::Failure(source.ErrorValue());
        auto instanceId = AllocatePrefabInstanceId(m_document.m_nextPrefabInstanceId);
        if (instanceId.HasError())
            return Result<SceneCommandResult>::Failure(instanceId.ErrorValue());
        ScenePrefabInstance duplicate = *source.Value();
        duplicate.instanceId = instanceId.Value();
        SceneCommandDelta delta = CreatedPrefabInstanceDelta{
            .instance = std::move(duplicate),
            .index = m_document.m_prefabInstances.size(),
            .kind = DocumentChangeKind::PrefabInstanceDuplicated,
        };
        return CommitPrefab(PrefabCommitContext{std::move(delta), instanceId.Value(), DocumentChangeKind::PrefabInstanceDuplicated, true});
    }

    /** @copydoc SceneDocumentCommandExecutor::Execute(const ReparentScenePrefabInstanceCommand&) */
    Result<SceneCommandResult> SceneDocumentCommandExecutor::Execute(const ReparentScenePrefabInstanceCommand &command) {
        auto instance = FindEditablePrefabInstance(m_document.m_objects, m_document.m_prefabInstances, command.instance);
        if (instance.HasError())
            return Result<SceneCommandResult>::Failure(instance.ErrorValue());
        if (const Result<void> validParent = ValidatePrefabParent(m_document.m_objects, command.parent); validParent.HasError())
            return Result<SceneCommandResult>::Failure(validParent.ErrorValue());
        if (instance.Value()->parent == command.parent)
            return Result<SceneCommandResult>::Success(PrefabNoOpResult(m_document.m_revision, m_document.m_state, command.instance,
                                                                        DocumentChangeKind::PrefabInstanceReparented));
        SceneCommandDelta delta = PrefabInstanceReparentDelta{command.instance, instance.Value()->parent, command.parent};
        return CommitPrefab(PrefabCommitContext{std::move(delta), command.instance, DocumentChangeKind::PrefabInstanceReparented});
    }

    /** @copydoc SceneDocumentCommandExecutor::Execute(const DeleteScenePrefabInstanceCommand&) */
    Result<SceneCommandResult> SceneDocumentCommandExecutor::Execute(const DeleteScenePrefabInstanceCommand &command) {
        auto instance = FindEditablePrefabInstance(m_document.m_objects, m_document.m_prefabInstances, command.instance);
        if (instance.HasError())
            return Result<SceneCommandResult>::Failure(instance.ErrorValue());
        const auto index = static_cast<std::size_t>(instance.Value() - m_document.m_prefabInstances.data());
        SceneCommandDelta delta = DeletedPrefabInstancesDelta{{IndexedPrefabInstance{*instance.Value(), index}}};
        return CommitPrefab(PrefabCommitContext{std::move(delta), command.instance, DocumentChangeKind::PrefabInstanceDeleted});
    }

    /** @copydoc SceneDocumentCommandExecutor::Undo */
    Result<SceneCommandResult> SceneDocumentCommandExecutor::Undo() {
        if (m_history.m_impl->undo.empty()) {
            return Result<SceneCommandResult>::Failure(
                MakeDocumentError(SceneDocumentErrors::NothingToUndo, "No committed scene command is available to undo."));
        }
        HistoryRecord entry = std::move(m_history.m_impl->undo.back());
        m_history.m_impl->undo.pop_back();
        RevertDelta(m_document.m_objects, m_document.m_prefabInstances, entry.delta);
        ++m_document.m_revision.value;
        m_document.m_state = entry.beforeState;
        SceneCommandResult result = HistoryCommandResult(m_document, entry, DocumentChangeKind::Undone);
        m_history.m_impl->redo.push_back(std::move(entry));
        return Result<SceneCommandResult>::Success(std::move(result));
    }

    /** @copydoc SceneDocumentCommandExecutor::Redo */
    Result<SceneCommandResult> SceneDocumentCommandExecutor::Redo() {
        if (m_history.m_impl->redo.empty()) {
            return Result<SceneCommandResult>::Failure(
                MakeDocumentError(SceneDocumentErrors::NothingToRedo, "No reverted scene command is available to redo."));
        }
        HistoryRecord entry = std::move(m_history.m_impl->redo.back());
        m_history.m_impl->redo.pop_back();
        ApplyDelta(m_document.m_objects, m_document.m_prefabInstances, entry.delta);
        ++m_document.m_revision.value;
        m_document.m_state = entry.afterState;
        SceneCommandResult result = HistoryCommandResult(m_document, entry, DocumentChangeKind::Redone);
        m_history.m_impl->undo.push_back(std::move(entry));
        return Result<SceneCommandResult>::Success(std::move(result));
    }

    /** @copydoc CreateSceneObjectUseCase::CreateSceneObjectUseCase */
    CreateSceneObjectUseCase::CreateSceneObjectUseCase(SceneDocument &document, SceneDocumentCommandExecutor &executor) noexcept
        : m_document(document), m_executor(executor) {}

    /** @copydoc CreateSceneObjectUseCase::Execute */
    Result<SceneCommandResult> CreateSceneObjectUseCase::Execute(const PrimitiveCreationRequest &request) {
        const Runtime::PrimitiveDescriptor *descriptor = Runtime::PrimitiveCatalog::Find(request.primitive.value);
        if (descriptor == nullptr) {
            return Result<SceneCommandResult>::Failure(
                MakeDocumentError(SceneDocumentErrors::UnknownPrimitive, "Requested primitive is not registered."));
        }
        if (descriptor->creationGroup == Runtime::PrimitiveCreationGroup::NotCreatable ||
            descriptor->category == Runtime::PrimitiveCategory::Collider) {
            return Result<SceneCommandResult>::Failure(
                MakeDocumentError(SceneDocumentErrors::PrimitiveNotCreatable, "Requested primitive is not a hierarchy creation object."));
        }
        if (request.parent.has_value() && !m_document.Contains(*request.parent)) {
            return Result<SceneCommandResult>::Failure(
                MakeDocumentError(SceneDocumentErrors::ParentNotFound, "Scene object parent does not exist."));
        }

        std::optional<std::string> name = UniqueSiblingName(descriptor->defaultObjectName, request.parent, m_document.Objects());
        if (!name.has_value())
            return Result<SceneCommandResult>::Failure(
                MakeDocumentError(SceneDocumentErrors::InvalidName, "Unable to allocate a unique primitive object name."));

        CreateSceneObjectCommand command{.name = std::move(*name), .parent = request.parent};
        if (const Result<void> configured = ConfigurePrimitiveCreationCommand(*descriptor, command); configured.HasError())
            return Result<SceneCommandResult>::Failure(configured.ErrorValue());
        return m_executor.Execute(command);
    }

    /** @copydoc InstantiateSceneAssetUseCase::InstantiateSceneAssetUseCase */
    InstantiateSceneAssetUseCase::InstantiateSceneAssetUseCase(SceneDocument &document, SceneDocumentCommandExecutor &executor) noexcept
        : document_(document), executor_(executor) {}

    /** @copydoc InstantiateSceneAssetUseCase::Execute */
    Result<SceneCommandResult> InstantiateSceneAssetUseCase::Execute(const AssetInstantiationRequest &request) {
        if (!request.asset.IsValid() || !IsValidSceneObjectName(request.baseName)) {
            return Result<SceneCommandResult>::Failure(
                MakeDocumentError(SceneDocumentErrors::InvalidPrimitiveMetadata, "Asset instantiation request is invalid."));
        }
        if (request.parent.has_value() && !document_.Contains(*request.parent)) {
            return Result<SceneCommandResult>::Failure(
                MakeDocumentError(SceneDocumentErrors::ParentNotFound, "Asset drop parent no longer exists."));
        }

        std::optional<std::string> name = UniqueSiblingName(request.baseName, request.parent, document_.Objects());
        if (!name.has_value())
            return Result<SceneCommandResult>::Failure(
                MakeDocumentError(SceneDocumentErrors::InvalidName, "Unable to allocate a unique asset object name."));
        return executor_.Execute(CreateSceneObjectCommand{.name = std::move(*name),
                                                          .parent = request.parent,
                                                          .localTransform = request.localTransform,
                                                          .components = {},
                                                          .meshAsset = request.asset});
    }
}  // namespace Horo::Editor
