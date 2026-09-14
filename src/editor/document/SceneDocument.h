#pragma once

/**
 * @file SceneDocument.h
 * @brief Editor-private authoritative scene state and narrow command boundary.
 */

#include "Horo/Assets/AssetId.h"
#include "Horo/Foundation/Result.h"
#include "Horo/Gameplay/BehaviorTypes.h"
#include "Horo/Math/SceneMath.h"
#include "Horo/Prefab/PrefabIdentity.h"
#include "Horo/Runtime/Scene/NavigationSceneComponents.h"
#include "Horo/Runtime/Scene/PhysicsSceneComponents.h"
#include "Horo/Runtime/Scene/PrimitiveMeshDescriptor.h"

#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Horo::Editor {
    inline constexpr std::size_t MaximumSceneObjectNameBytes = 128;

    /**
     * @brief Reports whether a scene object name satisfies the authored document contract.
     * @param name UTF-8 name measured in persisted bytes.
     * @return True when the name is non-empty and fits the bounded document field.
     */
    [[nodiscard]] bool IsValidSceneObjectName(std::string_view name) noexcept;

    /**
     * @brief Reports whether authored camera values satisfy document and runtime conversion constraints.
     * @param camera Camera component value to validate.
     * @return True when projection-specific values and clipping planes are finite and ordered.
     */
    [[nodiscard]] bool IsValidCameraComponent(const Runtime::CameraComponent &camera) noexcept;

    /**
     * @brief Reports whether authored light values satisfy document and runtime conversion constraints.
     * @param light Light component value to validate.
     * @return True when kind, color, intensity, range, and cone values are valid.
     */
    [[nodiscard]] bool IsValidLightComponent(const Runtime::LightComponent &light) noexcept;

    /**
     * @brief Reports whether authored audio source values satisfy document constraints.
     * @param audioSource Audio source component value to validate.
     * @return True when gain is finite and non-negative.
     */
    [[nodiscard]] bool IsValidAudioSourceComponent(const Runtime::AudioSourceComponent &audioSource) noexcept;

    /** @brief Stable identity of an authored scene object within one document session. */
    struct SceneObjectId {
        std::uint64_t value{0};

        /** @brief Reports whether this ID can identify an object. */
        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return value != 0;
        }

        [[nodiscard]] constexpr auto operator<=>(const SceneObjectId &) const noexcept = default;
    };

    /** @brief Monotonic committed revision of a scene document. */
    struct DocumentRevision {
        std::uint64_t value{0};

        [[nodiscard]] constexpr auto operator<=>(const DocumentRevision &) const noexcept = default;
    };

    /** @brief Immutable identity of one committed authored content state. */
    struct DocumentStateId {
        std::uint64_t value{0};

        /** @brief Reports whether this identity denotes a committed document state. */
        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return value != 0;
        }

        [[nodiscard]] constexpr auto operator<=>(const DocumentStateId &) const noexcept = default;
    };

    using PrimitiveMeshDescriptor = Runtime::PrimitiveMeshDescriptor;

    /** @brief Persisted editor-only visibility and interaction state for one authored object. */
    struct SceneObjectEditorState {
        bool visible{true}; /**< Local editor viewport visibility; runtime activation is unaffected. */
        bool locked{false}; /**< Local editor mutation lock; selection remains available for unlocking. */

        [[nodiscard]] constexpr bool operator==(const SceneObjectEditorState &) const noexcept = default;
    };

    /** @brief Local and ancestor-resolved editor state for one object. */
    struct ResolvedSceneObjectEditorState {
        SceneObjectEditorState local;  /**< Authored local value retained independently of ancestors. */
        bool effectivelyVisible{true}; /**< True only when the object and every ancestor are visible. */
        bool effectivelyLocked{false}; /**< True when the object or at least one ancestor is locked. */
        bool hiddenByParent{false};    /**< Whether an ancestor contributes effective invisibility. */
        bool lockedByParent{false};    /**< Whether an ancestor contributes the effective lock. */
    };

    /** @brief Typed authored component values attached to one scene object. */
    struct SceneObjectComponentSet {
        std::optional<Runtime::CameraComponent> camera;
        std::optional<Runtime::LightComponent> light;
        std::optional<Runtime::TriggerVolumeComponent> triggerVolume;
        std::optional<Runtime::AudioSourceComponent> audioSource;
        std::optional<Runtime::NavigationSurfaceComponent> navigationSurface;
        std::optional<Runtime::NavigationRegionComponent> navigationRegion;
        std::optional<Runtime::NavigationModifierComponent> navigationModifier;
        std::optional<Runtime::NavigationLinkComponent> navigationLink;
        std::optional<Runtime::RigidBodyComponent> rigidBody;
        std::vector<Runtime::ColliderComponent> colliders;
        std::vector<Runtime::PhysicsConstraintComponent> physicsConstraints;
        std::vector<Gameplay::BehaviorComponent> behaviors;

        [[nodiscard]] bool operator==(const SceneObjectComponentSet &) const noexcept = default;
    };

    /** @brief Immutable value snapshot of one authored scene object. */
    struct SceneObjectSnapshot {
        SceneObjectId id;
        std::optional<SceneObjectId> parent;
        std::string name;
        Math::Transform localTransform;
        std::optional<PrimitiveMeshDescriptor> primitiveMesh;
        SceneObjectComponentSet components;
        std::optional<Assets::AssetId> meshAsset; /**< Stable imported core.mesh identity. */
        SceneObjectEditorState editorState;
    };

    /** @brief Lightweight authored placement of one prefab asset in a containing scene. */
    struct ScenePrefabInstance final {
        Prefab::PrefabInstanceId instanceId;       /**< Stable scene-local occurrence identity. */
        Prefab::PrefabAssetReference sourcePrefab; /**< Path-independent source asset identity. */
        std::optional<SceneObjectId> parent;       /**< Optional containing-scene parent; never a prefab member. */
        Math::Transform rootTransform;             /**< Placement transform, separate from prefab-local transforms. */

        [[nodiscard]] bool operator==(const ScenePrefabInstance &) const noexcept = default;
    };

    /**
     * @brief Resolves local and inherited editor visibility/lock state.
     * @param objects Immutable objects from one coherent scene snapshot.
     * @param object Stable object identity to resolve.
     * @return Resolved state, or empty when the object or an ancestor chain is invalid.
     */
    [[nodiscard]] std::optional<ResolvedSceneObjectEditorState> ResolveSceneObjectEditorState(std::span<const SceneObjectSnapshot> objects,
                                                                                              SceneObjectId object) noexcept;

    /** @brief Immutable value snapshot of an entire committed scene document. */
    struct SceneDocumentSnapshot {
        DocumentRevision revision;
        DocumentStateId state;
        std::vector<SceneObjectSnapshot> objects;
        std::vector<ScenePrefabInstance> prefabInstances;
    };

    /** @brief One transient local-transform override keyed by stable scene-object identity. */
    struct SceneObjectTransformPreview {
        SceneObjectId object;
        Math::Transform localTransform;

        [[nodiscard]] constexpr bool operator==(const SceneObjectTransformPreview &) const noexcept = default;
    };

    /** @brief One transient Light-component override keyed by stable scene-object identity. */
    struct SceneObjectLightPreview {
        SceneObjectId object;
        Runtime::LightComponent light;

        [[nodiscard]] constexpr bool operator==(const SceneObjectLightPreview &) const noexcept = default;
    };

    /** @brief Semantic category of one committed document transition. */
    enum class DocumentChangeKind : std::uint8_t {
        Created,
        Renamed,
        TransformChanged,
        ComponentChanged,
        EditorStateChanged,
        Duplicated,
        Deleted,
        Undone,
        Redone,
        SaveStateChanged,
        PrefabInstanceCreated,
        PrefabInstanceTransformChanged,
        PrefabInstanceReparented,
        PrefabInstanceDuplicated,
        PrefabInstanceDeleted,
    };

    /** @brief Typed request to create one authored scene object. */
    struct CreateSceneObjectCommand {
        std::string name;
        std::optional<SceneObjectId> parent;
        Math::Transform localTransform;
        std::optional<PrimitiveMeshDescriptor> primitiveMesh;
        SceneObjectComponentSet components;
        std::optional<Assets::AssetId> meshAsset;
    };

    /** @brief Catalog-based request shared by editor creation adapters. */
    struct PrimitiveCreationRequest {
        Runtime::PrimitiveId primitive;
        std::optional<SceneObjectId> parent;
    };

    /** @brief Validated request shared by viewport and hierarchy asset-drop adapters. */
    struct AssetInstantiationRequest {
        Assets::AssetId asset;
        std::string baseName;
        std::optional<SceneObjectId> parent;
        Math::Transform localTransform;
    };

    /** @brief Typed request to rename an authored scene object. */
    struct RenameSceneObjectCommand {
        SceneObjectId object;
        std::string name;
    };

    /** @brief Typed request to replace an object's local transform. */
    struct SetSceneObjectTransformCommand {
        SceneObjectId object;
        Math::Transform localTransform;
    };

    /** @brief One object-local transform replacement inside a batch edit. */
    struct SceneObjectTransformUpdate {
        SceneObjectId object;
        Math::Transform localTransform;

        [[nodiscard]] constexpr bool operator==(const SceneObjectTransformUpdate &) const noexcept = default;
    };

    /**
     * @brief Typed request to atomically replace multiple object-local transforms.
     *
     * The executor validates the complete set before mutation and records at most
     * one semantic history entry for the batch.
     */
    struct SetSceneObjectTransformsCommand {
        std::vector<SceneObjectTransformUpdate> updates;
    };

    /** @brief Typed request to replace an existing core camera component. */
    struct SetSceneObjectCameraCommand {
        SceneObjectId object;
        Runtime::CameraComponent camera;
    };

    /** @brief Typed request to replace an existing core light component. */
    struct SetSceneObjectLightCommand {
        SceneObjectId object;
        Runtime::LightComponent light;
    };

    /** @brief Typed request to replace an existing trigger volume component. */
    struct SetSceneObjectTriggerVolumeCommand {
        SceneObjectId object;
        Runtime::TriggerVolumeComponent triggerVolume;
    };

    /** @brief Typed request to replace an existing audio source component. */
    struct SetSceneObjectAudioSourceCommand {
        SceneObjectId object;
        Runtime::AudioSourceComponent audioSource;
    };

    /** @brief Undoable replacement, attachment, or removal of one authored navigation surface. */
    struct SetSceneNavigationSurfaceCommand {
        SceneObjectId object;
        std::optional<Runtime::NavigationSurfaceComponent> surface;
    };

    /** @brief Undoable replacement, attachment, or removal of one authored navigation region. */
    struct SetSceneNavigationRegionCommand {
        SceneObjectId object;
        std::optional<Runtime::NavigationRegionComponent> region;
    };

    /** @brief Undoable replacement, attachment, or removal of one authored navigation modifier. */
    struct SetSceneNavigationModifierCommand {
        SceneObjectId object;
        std::optional<Runtime::NavigationModifierComponent> modifier;
    };

    /** @brief Undoable replacement, attachment, or removal of one authored grounded navigation link. */
    struct SetSceneNavigationLinkCommand {
        SceneObjectId object;
        std::optional<Runtime::NavigationLinkComponent> link;
    };

    /** @brief Undoable replacement of one object's local editor-only visibility and lock state. */
    struct SetSceneObjectEditorStateCommand {
        SceneObjectId object;
        SceneObjectEditorState editorState;
    };

    /** @brief Types of optional authored components on a scene object. */
    enum class ComponentType : std::uint8_t {
        Camera,
        Light,
        TriggerVolume,
        AudioSource
    };

    /** @brief Typed request to add a default-initialized component to an object. */
    struct AddSceneObjectComponentCommand {
        SceneObjectId object;
        ComponentType type;
    };

    /** @brief Typed request to remove a component from an object. */
    struct RemoveSceneObjectComponentCommand {
        SceneObjectId object;
        ComponentType type;
    };

    /** @brief Undoable request to attach one descriptor-derived behavior to an object. */
    struct AttachSceneObjectBehaviorCommand {
        SceneObjectId object;
        Gameplay::BehaviorTypeId typeId;
        std::uint32_t schemaVersion{1};
        bool enabled{true};
        bool allowMultiple{false};
        std::vector<Gameplay::BehaviorField> fields;
    };

    /** @brief Undoable request to replace enabled state or fields of one behavior attachment. */
    struct SetSceneObjectBehaviorCommand {
        SceneObjectId object;
        Gameplay::BehaviorComponent behavior;
    };

    /** @brief Undoable request to remove one attachment by stable identity. */
    struct RemoveSceneObjectBehaviorCommand {
        SceneObjectId object;
        Gameplay::BehaviorInstanceId behavior;
    };

    /** @brief Typed request to duplicate one object without duplicating its children. */
    struct DuplicateSceneObjectCommand {
        SceneObjectId source;
        std::string name;
    };

    /** @brief Typed request to delete one object and its complete descendant subtree. */
    struct DeleteSceneObjectCommand {
        SceneObjectId object;
    };

    /**
     * @brief Typed request to atomically delete multiple object subtrees.
     * @details Duplicate and missing identities are ignored. Objects whose ancestor is also requested are removed from the root set.
     */
    struct DeleteSceneObjectsCommand {
        std::vector<SceneObjectId> objects;
    };

    /** @brief Typed request to place a path-independent prefab reference in the containing scene. */
    struct CreateScenePrefabInstanceCommand final {
        Prefab::PrefabAssetReference sourcePrefab;
        std::optional<SceneObjectId> parent;
        Math::Transform rootTransform;
    };

    /** @brief Typed request to replace only a prefab instance's containing-scene root transform. */
    struct SetScenePrefabInstanceRootTransformCommand final {
        Prefab::PrefabInstanceId instance;
        Math::Transform rootTransform;
    };

    /** @brief Typed request to duplicate a placement with a fresh scene-local instance identity. */
    struct DuplicateScenePrefabInstanceCommand final {
        Prefab::PrefabInstanceId source;
    };

    /** @brief Typed request to reparent a prefab root to a containing-scene object or the scene root. */
    struct ReparentScenePrefabInstanceCommand final {
        Prefab::PrefabInstanceId instance;
        std::optional<SceneObjectId> parent;
    };

    /** @brief Typed request to remove one prefab reference without mutating its source asset. */
    struct DeleteScenePrefabInstanceCommand final {
        Prefab::PrefabInstanceId instance;
    };

    /** @brief Result metadata returned after a committed scene command. */
    struct SceneCommandResult {
        SceneObjectId object;
        DocumentRevision revision;
        DocumentStateId state;
        DocumentChangeKind kind{DocumentChangeKind::Created};
        std::vector<SceneObjectId> affectedObjects;
        bool committed{false};
        std::optional<Prefab::PrefabInstanceId> prefabInstance;
        std::vector<Prefab::PrefabInstanceId> affectedPrefabInstances;
    };

    class SceneDocumentCommandExecutor;

    /** @brief Bounded semantic undo/redo history owned by one editor document session. */
    class EditorHistory final {
    public:
        /** @brief Creates an empty history with bounded item and memory budgets. */
        EditorHistory();

        ~EditorHistory();

        EditorHistory(const EditorHistory &) = delete;

        EditorHistory &operator=(const EditorHistory &) = delete;

        /** @brief Reports whether one committed transaction can be undone. */
        [[nodiscard]] bool CanUndo() const noexcept;

        /** @brief Reports whether one previously undone transaction can be redone. */
        [[nodiscard]] bool CanRedo() const noexcept;

        /** @brief Clears both history branches at an explicit document load/reload boundary. */
        void Clear() noexcept;

    private:
        friend class SceneDocumentCommandExecutor;
        struct Impl;
        std::unique_ptr<Impl> m_impl;
    };

    /** @brief Authoritative scene authoring storage exposed publicly through immutable queries. */
    class SceneDocument final {
    public:
        /** @brief Returns the current committed document revision. */
        [[nodiscard]] DocumentRevision Revision() const noexcept;

        /** @brief Returns whether committed state differs from the last marked saved revision. */
        [[nodiscard]] bool IsDirty() const noexcept;

        /** @brief Returns an immutable value snapshot of the committed document. */
        [[nodiscard]] SceneDocumentSnapshot Snapshot() const;

        /**
         * @brief Returns a borrowed immutable view of committed objects for owner-thread projection work.
         * @return View valid until the next document mutation or destruction.
         */
        [[nodiscard]] std::span<const SceneObjectSnapshot> Objects() const noexcept;

        /** @brief Returns a borrowed immutable view of authored prefab instance references. */
        [[nodiscard]] std::span<const ScenePrefabInstance> PrefabInstances() const noexcept;

        /**
         * @brief Reports whether the committed document contains @p object.
         * @param object Stable scene object identity to query.
         * @return True when the object currently exists.
         */
        [[nodiscard]] bool Contains(SceneObjectId object) const noexcept;

        /** @brief Returns the immutable identity of the currently visible authored state. */
        [[nodiscard]] DocumentStateId State() const noexcept;

        /** @brief Returns the revision captured by the last successful canonical save. */
        [[nodiscard]] DocumentRevision SavedRevision() const noexcept;

        /** @brief Returns the authored state identity written by the last successful canonical save. */
        [[nodiscard]] DocumentStateId SavedState() const noexcept;

        /**
         * @brief Marks a captured revision/state pair as durably saved without changing current content.
         * @param revision Monotonic revision captured by the successful save operation.
         * @param state Immutable content state written by that operation.
         * @return Success or an error when the pair cannot belong to this document session.
         */
        [[nodiscard]] Result<void> MarkSaved(DocumentRevision revision, DocumentStateId state);

        /**
         * @brief Replaces the document at an explicit durable-load boundary.
         * @param objects Fully parsed object values from one validated scene document.
         * @param prefabInstances Fully parsed prefab-instance references from that document.
         * @return Success after installing a clean baseline, or a typed validation error.
         *
         * This operation clears the current authored state but does not own editor history;
         * the document-session owner must clear its history at the same load boundary.
         */
        [[nodiscard]] Result<void> LoadSaved(std::vector<SceneObjectSnapshot> objects,
                                             std::vector<ScenePrefabInstance> prefabInstances = {});

        /**
         * @brief Installs validated recovery content as a new dirty editor session.
         * @param objects Fully parsed object values from a trusted recovery-service result.
         * @param prefabInstances Fully parsed prefab-instance references from that recovery result.
         * @return Success with recovered content dirty relative to the saved baseline, or a typed validation error.
         */
        [[nodiscard]] Result<void> LoadRecovered(std::vector<SceneObjectSnapshot> objects,
                                                 std::vector<ScenePrefabInstance> prefabInstances = {});

    private:
        friend class SceneDocumentCommandExecutor;

        std::vector<SceneObjectSnapshot> m_objects;
        std::vector<ScenePrefabInstance> m_prefabInstances;
        DocumentRevision m_revision{};
        DocumentRevision m_savedRevision{};
        DocumentStateId m_state{1};
        DocumentStateId m_savedState{1};
        std::uint64_t m_nextStateId{2};
        std::uint64_t m_nextObjectId{1};
        std::uint64_t m_nextBehaviorInstanceId{1};
        std::uint64_t m_nextPrefabInstanceId{1};
        std::uint64_t m_nextNavigationSurfaceId{1};
        std::uint64_t m_nextNavigationRegionId{1};
        std::uint64_t m_nextNavigationModifierId{1};
        std::uint64_t m_nextNavigationLinkId{1};
    };

    /** @brief Sole mutation boundary for the minimum typed scene command set. */
    class SceneDocumentCommandExecutor final {
    public:
        /** @brief Creates an executor that commits commands against @p document. */
        explicit SceneDocumentCommandExecutor(SceneDocument &document, EditorHistory &history) noexcept;

        /** @brief Validates and atomically commits a create-object command. */
        [[nodiscard]] Result<SceneCommandResult> Execute(const CreateSceneObjectCommand &command);

        /** @brief Validates and atomically commits a rename-object command. */
        [[nodiscard]] Result<SceneCommandResult> Execute(const RenameSceneObjectCommand &command);

        /** @brief Validates and atomically commits a transform command. */
        [[nodiscard]] Result<SceneCommandResult> Execute(const SetSceneObjectTransformCommand &command);

        /** @brief Validates and atomically commits one batch transform command. */
        [[nodiscard]] Result<SceneCommandResult> Execute(const SetSceneObjectTransformsCommand &command);

        /** @brief Validates and atomically commits an existing camera component. */
        [[nodiscard]] Result<SceneCommandResult> Execute(const SetSceneObjectCameraCommand &command);

        /** @brief Validates and atomically commits an existing light component. */
        [[nodiscard]] Result<SceneCommandResult> Execute(const SetSceneObjectLightCommand &command);

        /** @brief Validates and atomically commits an existing trigger volume component. */
        [[nodiscard]] Result<SceneCommandResult> Execute(const SetSceneObjectTriggerVolumeCommand &command);

        /** @brief Validates and atomically commits an existing audio source component. */
        [[nodiscard]] Result<SceneCommandResult> Execute(const SetSceneObjectAudioSourceCommand &command);

        /** @brief Validates Scene-wide identities/references and atomically commits a navigation surface value. */
        [[nodiscard]] Result<SceneCommandResult> Execute(const SetSceneNavigationSurfaceCommand &command);

        /** @brief Validates Scene-wide identities/references and atomically commits a navigation region value. */
        [[nodiscard]] Result<SceneCommandResult> Execute(const SetSceneNavigationRegionCommand &command);

        /** @brief Validates Scene-wide identities/references and atomically commits a navigation modifier value. */
        [[nodiscard]] Result<SceneCommandResult> Execute(const SetSceneNavigationModifierCommand &command);

        /** @brief Validates endpoint/profile compatibility and atomically commits a grounded navigation link value. */
        [[nodiscard]] Result<SceneCommandResult> Execute(const SetSceneNavigationLinkCommand &command);

        /** @brief Atomically commits local editor visibility/lock state without changing runtime activation. */
        [[nodiscard]] Result<SceneCommandResult> Execute(const SetSceneObjectEditorStateCommand &command);
        [[nodiscard]] Result<SceneCommandResult> Execute(const AddSceneObjectComponentCommand &command);
        [[nodiscard]] Result<SceneCommandResult> Execute(const RemoveSceneObjectComponentCommand &command);
        /** @brief Validates and atomically attaches one behavior with a generated stable instance ID. */
        [[nodiscard]] Result<SceneCommandResult> Execute(const AttachSceneObjectBehaviorCommand &command);
        /** @brief Validates and atomically replaces one existing behavior attachment. */
        [[nodiscard]] Result<SceneCommandResult> Execute(const SetSceneObjectBehaviorCommand &command);
        /** @brief Validates and atomically removes one behavior attachment. */
        [[nodiscard]] Result<SceneCommandResult> Execute(const RemoveSceneObjectBehaviorCommand &command);

        /** @brief Validates and atomically commits a shallow duplicate-object command. */
        [[nodiscard]] Result<SceneCommandResult> Execute(const DuplicateSceneObjectCommand &command);

        /** @brief Validates and atomically deletes an object subtree. */
        [[nodiscard]] Result<SceneCommandResult> Execute(const DeleteSceneObjectCommand &command);

        /** @brief Validates and atomically deletes normalized object subtrees as one history entry. */
        [[nodiscard]] Result<SceneCommandResult> Execute(const DeleteSceneObjectsCommand &command);

        /** @brief Validates and atomically commits one prefab placement. */
        [[nodiscard]] Result<SceneCommandResult> Execute(const CreateScenePrefabInstanceCommand &command);

        /** @brief Validates and atomically replaces one prefab root placement transform. */
        [[nodiscard]] Result<SceneCommandResult> Execute(const SetScenePrefabInstanceRootTransformCommand &command);

        /** @brief Atomically duplicates a prefab reference under a fresh instance identity. */
        [[nodiscard]] Result<SceneCommandResult> Execute(const DuplicateScenePrefabInstanceCommand &command);

        /** @brief Atomically reparents a prefab root without crossing into prefab-local hierarchy. */
        [[nodiscard]] Result<SceneCommandResult> Execute(const ReparentScenePrefabInstanceCommand &command);

        /** @brief Atomically removes a prefab instance reference. */
        [[nodiscard]] Result<SceneCommandResult> Execute(const DeleteScenePrefabInstanceCommand &command);

        /** @brief Reverts the newest committed semantic history entry. */
        [[nodiscard]] Result<SceneCommandResult> Undo();

        /** @brief Reapplies the newest previously undone semantic history entry. */
        [[nodiscard]] Result<SceneCommandResult> Redo();

    private:
        struct PrefabCommitContext;
        struct ObjectCommitContext;
        [[nodiscard]] Result<SceneCommandResult> CommitObject(ObjectCommitContext context);
        [[nodiscard]] Result<SceneCommandResult> CommitNavigationComponents(
            SceneObjectId object, const std::optional<Runtime::NavigationSurfaceComponent> *surface,
            const std::optional<Runtime::NavigationRegionComponent> *region,
            const std::optional<Runtime::NavigationModifierComponent> *modifier,
            const std::optional<Runtime::NavigationLinkComponent> *link);

        /** @brief Commits one validated prefab delta through the shared document/history transition. */
        [[nodiscard]] Result<SceneCommandResult> CommitPrefab(PrefabCommitContext context);

        SceneDocument &m_document;
        EditorHistory &m_history;
    };

    /** @brief Notification published after document content or save-state authority commits. */
    struct SceneDocumentChangedEvent {
        static constexpr auto HoroEventTypeName = "SceneDocumentChangedEvent";

        DocumentRevision revision;
        DocumentStateId state;
        DocumentChangeKind kind{DocumentChangeKind::Created};
        bool dirty{false};
        std::vector<SceneObjectId> affectedObjects;
        std::vector<Prefab::PrefabInstanceId> affectedPrefabInstances;
    };

    /** @brief Validates catalog creation requests and commits one typed document command. */
    class CreateSceneObjectUseCase final {
    public:
        /** @brief Creates a catalog primitive through the supplied document mutation boundary. */
        CreateSceneObjectUseCase(SceneDocument &document, SceneDocumentCommandExecutor &executor) noexcept;

        /**
         * @brief Validates and creates one catalog primitive.
         * @param request Stable primitive identity and optional parent.
         * @return Committed command metadata or a typed validation error.
         */
        [[nodiscard]] Result<SceneCommandResult> Execute(const PrimitiveCreationRequest &request);

    private:
        SceneDocument &m_document;
        SceneDocumentCommandExecutor &m_executor;
    };

    /** @brief Central imported-mesh instantiation boundary used by every scene drop target. */
    class InstantiateSceneAssetUseCase final {
    public:
        InstantiateSceneAssetUseCase(SceneDocument &document, SceneDocumentCommandExecutor &executor) noexcept;
        [[nodiscard]] Result<SceneCommandResult> Execute(const AssetInstantiationRequest &request);

    private:
        SceneDocument &document_;
        SceneDocumentCommandExecutor &executor_;
    };
}  // namespace Horo::Editor
