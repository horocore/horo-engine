#pragma once

/**
 * @file RuntimeScene.h
 * @brief Generation-checked runtime scene ownership and deferred structural mutations.
 */

#include "Horo/Assets/AssetPayloadCache.h"
#include "Horo/Assets/AssetRegistry.h"
#include "Horo/Runtime/RuntimeLifecycle.h"
#include "Horo/Runtime/Scene/RuntimeSceneDefinition.h"

#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <variant>
#include <vector>

namespace Horo::Assets {
    class AssetLoadService;
}

namespace Horo::Runtime {
    class RuntimeScene;
    class RuntimeSceneView;
    class IStagedRestoreAggregatePublication;

    /**
     * @brief Owned composite restore retained behind normal Scene preparation admission.
     * @details Preparation touches only the unpublished candidate. Pending readiness cannot activate
     *          the Scene. Commit reuses the original save operation cancellation gate and invokes the
     *          supplied no-fail Scene transfer before operation completion becomes observable.
     */
    class SceneAggregateRestore {
    public:
        virtual ~SceneAggregateRestore() = default;
        /** @brief Allocates exact identities and stages core values on the unpublished Scene.
         * @param scene Actual candidate owned by the Scene service, borrowed until retirement.
         * @return Success or a typed preparation failure with no active-state mutation. */
        [[nodiscard]] virtual Result<void> PrepareScene(RuntimeScene &scene) = 0;
        /** @brief Advances bounded detached restore preparation at the owner lifecycle boundary.
         * @return Ready, still pending, or a typed failure requiring aggregate rollback. */
        [[nodiscard]] virtual Result<bool> PrepareOwners() = 0;
        /** @brief Revalidates current owner evidence immediately before the shared publication gate.
         * @param active Current active Scene view, or absence at initial load.
         * @return Success or typed stale/cancellation/reference failure before publication. */
        [[nodiscard]] virtual Result<void> ValidatePublication(const RuntimeSceneView *active) const = 0;
        /** @brief Publishes prepared roots through the original restore operation commit gate.
         * @param sceneTransfer Service-owned no-fail Scene aggregate transfer.
         * @return Success or a typed failure before any root has changed.
         * @pre Every Scene and restore candidate has passed publication validation.
         */
        [[nodiscard]] virtual Result<void> Commit(IStagedRestoreAggregatePublication &sceneTransfer) = 0;
        /** @brief Cancels and retires unpublished owned work without touching active state; repeated calls are harmless. */
        virtual void Rollback() noexcept = 0;
    };

    /** @brief Prepared-owner declaration of persistent dataset participation; runtime/backend world handles are not dataset IDs. */
    enum class SceneCanonicalDatasetProjection : std::uint8_t {
        Unqualified,
        Absent,
        PersistentWorld
    };
    /** @brief Finite admission bound for one aggregate Scene preparation. */
    inline constexpr std::size_t MaximumSceneActivationParticipants = 256;

    /** @brief Detached subsystem state prepared for one aggregate runtime-scene candidate. */
    class SceneActivationCandidate {
    public:
        virtual ~SceneActivationCandidate() = default;
        /** @brief Revalidates authoritative evidence immediately before aggregate publication. */
        [[nodiscard]] virtual Result<void> ValidatePublication() const = 0;

        /** @brief Projects persistent dataset ownership from the actual prepared root, without activation or allocation.
         * @return Absent only when this owner contains no persistent dataset; legacy owners remain Unqualified.
         * @details PersistentWorld requires an actual typed dataset mapping before content-aware canonical capture is admitted.
         */
        [[nodiscard]] virtual SceneCanonicalDatasetProjection CanonicalDatasetProjection() const noexcept {
            return SceneCanonicalDatasetProjection::Unqualified;
        }

        /** @brief Installs fully validated state after every aggregate participant has passed validation.
         * @details Implementations must not fail, allocate, or perform provider work in this call. */
        virtual void Publish() noexcept {
            // Validation-only participants own no state to install at the aggregate publication boundary.
        }

        /** @brief Closes subsystem admission and releases fully prepared state; safe before or after publication. */
        virtual void Shutdown() noexcept = 0;
    };

    /** @brief Owned pure admission predicate retained until deferred scene publication or cancellation. */
    class ScenePublicationCheck {
    public:
        virtual ~ScenePublicationCheck() = default;
        /** @brief Revalidates external owner evidence at the publication safe point.
         * @return Success or a typed rejection that preserves the active scene.
         * @details Must not mutate state, allocate resources or perform I/O. Referenced authorities must outlive the pending operation.
         */
        [[nodiscard]] virtual Result<void> ValidatePublication() const = 0;

        /** @brief Revalidates the actual prepared aggregate's sealed canonical dataset projection before publication.
         * @param projection Projection gathered by Scene from the actual prepared roots at load time.
         * @return Success or typed unsupported composition; ordinary legacy predicates retain their existing behavior.
         */
        [[nodiscard]] virtual Result<void> ValidatePreparedComposition(SceneCanonicalDatasetProjection projection) const {
            (void)projection;
            return Result<void>::Success();
        }
    };

    /** @brief Host-injected subsystem participating in aggregate runtime-scene publication. */
    class SceneActivationParticipant {
    public:
        virtual ~SceneActivationParticipant() = default;
        /** @brief Prepares and fully finalizes detached subsystem state for an unpublished scene candidate.
         * @param definition Validated immutable scene definition.
         * @param scene Borrowed unpublished scene candidate.
         * @return Owned candidate ready for evidence validation and no-fail aggregate publication, or a typed error.
         */
        [[nodiscard]] virtual Result<std::unique_ptr<SceneActivationCandidate>> Prepare(const RuntimeSceneDefinition &definition,
                                                                                        RuntimeSceneView scene) = 0;
    };

    /** @brief Unique identity of one activated runtime-scene instance. */
    struct SceneRuntimeId {
        std::uint64_t value{};

        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return value != 0;
        }

        [[nodiscard]] constexpr auto operator<=>(const SceneRuntimeId &) const noexcept = default;
    };

    namespace ScenePublicationDetail {
        struct State;
    }
    /** @brief Terminal disposition of the exact Scene-owned queue operation. */
    enum class ScenePublicationStatus : std::uint8_t {
        Pending,
        Published,
        Rejected,
        Cancelled
    };

    /** @brief Value-only observation; only a published receipt has a nonzero scene and revision. */
    struct ScenePublicationSnapshot final {
        ScenePublicationStatus status{ScenePublicationStatus::Pending};
        SceneRuntimeId scene;
        std::uint64_t structuralRevision{};
        SceneCanonicalDatasetProjection datasets{SceneCanonicalDatasetProjection::Unqualified};
    };

    /** @brief Retained observer issued only by actual Scene queue admission, never a caller-provided identity assertion.
     * @details All access occurs on the queue owner thread. The observer retains no SceneView and survives service retirement.
     */
    class ScenePublicationReceipt final {
    public:
        ScenePublicationReceipt(const ScenePublicationReceipt &) noexcept = default;
        ScenePublicationReceipt &operator=(const ScenePublicationReceipt &) noexcept = default;
        ScenePublicationReceipt(ScenePublicationReceipt &&) noexcept = default;
        ScenePublicationReceipt &operator=(ScenePublicationReceipt &&) noexcept = default;
        /** @brief Observes the exact admitted operation on its owner thread.
         * @return Pending/terminal evidence, or typed wrong-thread/moved/ordinary queue error. Published identities are Scene-issued.
         */
        [[nodiscard]] Result<ScenePublicationSnapshot> Snapshot() const;

    private:
        friend class RuntimeSceneService;

        explicit ScenePublicationReceipt(std::shared_ptr<ScenePublicationDetail::State> state) noexcept : state_(std::move(state)) {}

        std::shared_ptr<ScenePublicationDetail::State> state_;
    };

    /** @brief Generation-checked entity slot identity within one runtime scene. */
    struct EntityId {
        std::uint32_t index{};
        std::uint32_t generation{};

        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return generation != 0;
        }

        [[nodiscard]] constexpr auto operator<=>(const EntityId &) const noexcept = default;
    };

    /** @brief Complete runtime entity reference including its owning scene domain. */
    struct EntityRef {
        SceneRuntimeId runtime;
        EntityId entity;

        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return runtime.IsValid() && entity.IsValid();
        }

        [[nodiscard]] constexpr auto operator<=>(const EntityRef &) const noexcept = default;
    };

    /** @brief Testable generation-retirement policy; production uses the uint32 maximum. */
    struct RuntimeSceneConfig {
        std::uint32_t maximumGeneration{std::numeric_limits<std::uint32_t>::max()};
    };

    /** @brief Bounded asset preparation limits for one runtime scene candidate. */
    struct RuntimeSceneAssetLimits {
        std::size_t maximumDependencies{1024};                         /**< Maximum required assets in one definition. */
        std::size_t maximumConcurrentLoads{8};                         /**< Maximum in-flight provider requests. */
        std::size_t maximumResidentBytes{1024ULL * 1024ULL * 1024ULL}; /**< Per-candidate logical byte budget. */
    };

    /** @brief Allocation-free borrowed cooked payload resolved for an active scene. */
    struct RuntimeSceneAssetView {
        Assets::AssetId id;                  /**< Stable identity of the resolved payload. */
        const Assets::AssetTypeId *type{};   /**< Borrowed validated type owned by the scene. */
        std::span<const std::uint8_t> bytes; /**< Borrowed immutable cooked bytes. */
    };

    /** @brief Exact named cooked envelope retained by every live entity in a structural group.
     * @details Owners validate the envelope against metadata before realization; the lease pins immutable cooked resource bytes,
     * not a source path or a mutable catalog lookup. Group retirement releases pins only after native owner retirement.
     */
    struct RuntimeGroupAssetLease final {
        Assets::AssetDependency metadata;
        Assets::AssetPayloadLease artifact;
    };

    /** @brief Complete initial topology and component state for a deferred runtime create. */
    struct RuntimeEntityCreateInfo {
        Math::Transform localTransform;
        std::optional<EntityRef> parent;
        std::optional<SceneObjectId> authoredObject;
        std::optional<PrimitiveMeshDescriptor> primitiveMesh;
        RuntimeComponentSet components;
    };

    /** @brief Typed Physics producer field receiving an exact runtime body reference after group reservation. */
    enum class GroupPhysicsReferenceKind {
        ColliderBody,
        ConstraintFirst,
        ConstraintSecond
    };

    /** @brief Dense group-local entity address, never an authored object ID or published runtime handle. */
    struct RuntimeGroupEntitySlot final {
        std::size_t index{};
    };

    /** @brief Exact existing member schema/occurrence inside one immutable runtime group. */
    struct RuntimeGroupMemberIdentity final {
        std::variant<Gameplay::ComponentTypeId, Gameplay::BehaviorTypeId> type;
        std::uint64_t instance{};
        [[nodiscard]] bool operator==(const RuntimeGroupMemberIdentity &) const noexcept = default;
    };

    /** @brief A dense target member resolved after every group entity has been reserved. */
    struct RuntimeGroupMemberSlot final {
        RuntimeGroupEntitySlot entity;
        std::size_t member{};
    };

    /** @brief Exact generation-qualified external relationship with optional component compatibility. */
    struct RuntimeGroupExternalReference final {
        EntityRef entity;
        std::optional<Gameplay::ComponentTypeId> component;
    };

    /** @brief One cook-owned typed reference interface; an absent optional binding is explicit Unbound. */
    struct RuntimeGroupReference final {
        std::size_t ownerMember{};
        std::uint64_t property{};
        std::variant<std::monostate, RuntimeGroupEntitySlot, RuntimeGroupMemberSlot, Assets::AssetDependency, RuntimeGroupExternalReference>
            target;
    };

    /** @brief Published typed member relationship. No ECS addresses or mutable component borrows are exposed. */
    struct RuntimeGroupMemberReference final {
        EntityRef entity;
        RuntimeGroupMemberIdentity member;
    };

    /** @brief Scene-owned immutable generation-qualified reference available only after complete publication. */
    struct RuntimeResolvedGroupReference final {
        RuntimeGroupMemberIdentity owner;
        std::uint64_t property{};
        std::variant<std::monostate, EntityRef, RuntimeGroupMemberReference, Assets::AssetDependency, RuntimeGroupExternalReference> target;
    };

    /** @brief One complete typed Physics reference fixup.
     * External targets must be live in the receiving committed Scene before this command buffer
     * and remain live in its final candidate. Group-local slots may refer forward within the group.
     */
    struct GroupPhysicsBodyReference final {
        GroupPhysicsReferenceKind kind;
        std::size_t component{}; /**< Collider/constraint occurrence in this entity's typed component array. */
        std::variant<RuntimeGroupEntitySlot, EntityRef> target;
        PhysicsBodySlotId body; /**< Exact body slot on the target; never selected by hierarchy or display name. */
    };

    /** @brief Scene-owned generation-qualified result of a typed group reference fixup. */
    struct ResolvedGroupPhysicsBodyReference final {
        GroupPhysicsReferenceKind kind;
        std::size_t component{};
        EntityRef target;
        PhysicsBodySlotId body;
    };

    /** @brief One complete projected entity in a parent-before-child structural group. */
    struct RuntimeEntityGroupEntry final {
        RuntimeEntityCreateInfo info;
        std::optional<std::size_t> parentInGroup;                 /**< Earlier group index; mutually exclusive with info.parent. */
        std::vector<GroupPhysicsBodyReference> physicsReferences; /**< Resolved only after the complete group is reserved. */
        std::vector<RuntimeGroupMemberIdentity> members;          /**< Complete cook-projected occurrence metadata. */
        std::vector<RuntimeGroupReference> references; /**< Declared reference interfaces, separate from initialization values. */
        std::vector<Assets::AssetId> spawnLineage;     /**< Bounded immutable inherited creation evidence, at most 16 unique assets. */
    };

    /** @brief Generation, catalog and cancellation evidence rechecked by Scene at structural publication. */
    struct SceneStructuralAdmission final {
        SceneRuntimeId scene;
        Assets::AssetRegistryRevision registry;
        CancellationToken cancellation;
        CancellationToken ownerCancellation; /**< Host owner retirement closes pending publication without callbacks. */
        CancellationToken scopeCancellation; /**< Module/capability revocation closes copied clients and queued work. */
    };

    /** @brief Stable token resolved to an EntityRef only after a successful structural commit. */
    struct DeferredEntity {
        std::uint64_t value{};

        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return value != 0;
        }

        [[nodiscard]] constexpr auto operator<=>(const DeferredEntity &) const noexcept = default;
    };

    /** @brief One deferred-create resolution emitted by a successful structural commit. */
    struct DeferredEntityResolution {
        DeferredEntity deferred;
        EntityRef entity;
    };

    /** @brief Atomic result of one committed structural command batch. */
    struct StructuralCommitResult {
        std::vector<DeferredEntityResolution> created;
        std::size_t destroyed{};
        std::size_t transformsUpdated{};
    };

    /** @brief Immutable observation of one Scene-owned transaction, retained safely through Scene/service retirement.
     * Observers use the Scene owner lane. Pending means unpublished; success remains success after entity retirement.
     */
    class SceneStructuralReceipt final {
    public:
        /** @brief Reports whether the transaction has completed. @return True for committed or rejected work. */
        [[nodiscard]] bool Complete() const noexcept {
            return result_.has_value() || error_.has_value();
        }

        /** @brief Borrows the exact committed result. @return Null while pending or rejected. */
        [[nodiscard]] const StructuralCommitResult *ResultValue() const noexcept {
            return result_ ? &*result_ : nullptr;
        }

        /** @brief Borrows the original typed rejection. @return Null while pending or committed. */
        [[nodiscard]] const Error *Failure() const noexcept {
            return error_ ? &*error_ : nullptr;
        }

    private:
        friend class RuntimeSceneService;
        std::optional<StructuralCommitResult> result_;
        std::optional<Error> error_;
    };

    /** @brief Owner-thread command buffer for structural changes at the lifecycle safe point. */
    class SceneCommandBuffer final {
    public:
        /** @brief Queues an entity with its complete initial component topology. @param createInfo Complete initial state
         * copied into the command. @return Batch-local deferred token. */
        [[nodiscard]] DeferredEntity Create(RuntimeEntityCreateInfo createInfo);
        /** @brief Queues a complete bounded group atomically, retaining exact verified resource allocations.
         * @param entries Nonempty parent-before-child complete runtime projections, at most 256 entities.
         * @param resources Canonical artifact pins required by the group, at most 257 nonempty leases.
         * @param admission Exact scene/catalog/cancellation evidence required at the safe point.
         * @return Deferred tokens or a typed rejection without changing this buffer.
         */
        [[nodiscard]] Result<std::vector<DeferredEntity>> CreateGroup(std::vector<RuntimeEntityGroupEntry> entries,
                                                                      std::vector<RuntimeGroupAssetLease> resources,
                                                                      const SceneStructuralAdmission &admission);
        /** @brief Queues retirement of an exact complete resource group in reverse topology order.
         * @param entities Original parent-before-child generation-qualified group identities.
         * @param admission Scene and cancellation evidence rechecked before publication. Retirement uses retained resource
         * ownership and does not require the creation catalog revision to remain current.
         * @return Success or rejection without queued work. Missing/reused entities and foreign children fail atomically.
         */
        [[nodiscard]] Result<void> DestroyGroup(std::vector<EntityRef> entities, const SceneStructuralAdmission &admission);
        /** @brief Queues destruction of an existing generation-checked entity. @param entity Reference validated when the
         * batch commits. */
        void Destroy(EntityRef entity);
        /** @brief Queues a local-transform replacement validated when the batch commits. */
        void SetLocalTransform(EntityRef entity, Math::Transform localTransform);
        /** @brief Reports whether no structural commands are queued. @return True when the buffer has no commands. */
        [[nodiscard]] bool Empty() const noexcept;

    private:
        friend class RuntimeScene;
        friend class RuntimeSceneService;

        struct CreateCommand {
            DeferredEntity deferred;
            RuntimeEntityCreateInfo info;
        };

        struct DestroyCommand {
            EntityRef entity;
        };

        struct SetLocalTransformCommand {
            EntityRef entity;
            Math::Transform localTransform;
        };

        struct CreateGroupCommand {
            std::vector<RuntimeEntityGroupEntry> entries;
            std::vector<RuntimeGroupAssetLease> resources;
            std::vector<DeferredEntity> deferred;
            SceneStructuralAdmission admission;
        };

        struct DestroyGroupCommand {
            std::vector<EntityRef> entities;
            SceneStructuralAdmission admission;
        };

        using Command = std::variant<CreateCommand, DestroyCommand, SetLocalTransformCommand, CreateGroupCommand, DestroyGroupCommand>;
        [[nodiscard]] Result<void> ValidateAdmission(SceneRuntimeId scene, Assets::AssetRegistryRevision registry) const;
        std::vector<Command> commands_;
        std::uint64_t nextDeferred_{1};
    };

    /** @brief Allocation-free value view of one active runtime entity slot. */
    struct RuntimeEntityView {
        EntityRef entity;
        std::optional<SceneObjectId> authoredObject;
        std::optional<EntityRef> parent;
        const Math::Transform *localTransform{};
        const std::optional<PrimitiveMeshDescriptor> *primitiveMesh{};
        const RuntimeComponentSet *components{};
        std::span<const ResolvedGroupPhysicsBodyReference> physicsReferences; /**< Runtime binding authority, not durable authoring. */
        std::span<const RuntimeGroupAssetLease> groupAssets;            /**< Exact named envelope pins, borrowed for this entity view. */
        std::span<const RuntimeResolvedGroupReference> groupReferences; /**< Borrowed immutable post-publication reference interfaces. */
        std::span<const Assets::AssetId> groupSpawnLineage; /**< Borrowed inherited creation evidence for host capability scoping. */
    };

    /** @brief Explicit subsystem ownership for staged structural changes; never backend discovery. */
    enum class SceneStructuralOwner {
        Physics,
        Gameplay,
        AI
    };

    /** @brief Unpublished subsystem additions/removals owned until aggregate commit or rollback. */
    class SceneStructuralCandidate {
    public:
        virtual ~SceneStructuralCandidate() = default;
        /** @brief Rechecks exact owner generations after every candidate has prepared. @return Typed failure without publication. */
        [[nodiscard]] virtual Result<void> ValidatePublication() const = 0;
        /** @brief Publishes fully reserved owner state at the drained lifecycle safe point; no callbacks/allocation/failure. */
        virtual void Publish() noexcept = 0;
        /** @brief Runs owner lifecycle notifications only after every owner and Scene is visible.
         * @details Behavior construction/hook faults are reported, never structural rollback after publication.
         * @return Notification result; failure leaves the already committed structural result available.
         */
        [[nodiscard]] virtual Result<void> AfterPublication() = 0;
    };

    /** @brief Application-composed subsystem participant for bounded Scene group transactions. */
    class SceneStructuralParticipant {
    public:
        virtual ~SceneStructuralParticipant() = default;
        /** @brief Names the one subsystem this participant owns. @return Stable owner category. */
        [[nodiscard]] virtual SceneStructuralOwner Owner() const noexcept = 0;
        /** @brief Prepares detached additions/removals without exposing native state or running gameplay hooks.
         * @param active Existing Scene, borrowed only for this call.
         * @param created Complete candidate entities, with generation-qualified parent references.
         * @param destroyed Existing entities retired by the same transaction.
         * @return Owned unpublished candidate. Its destruction rolls back uncommitted owner work.
         */
        [[nodiscard]] virtual Result<std::unique_ptr<SceneStructuralCandidate>> Prepare(RuntimeSceneView active,
                                                                                        std::span<const RuntimeEntityView> created,
                                                                                        std::span<const EntityRef> destroyed) = 0;
    };

    /** @brief Borrowed immutable view of one runtime scene; invalidated by structural commit or transition. */
    class RuntimeSceneView final {
    public:
        RuntimeSceneView() = default;
        /** @brief Reports whether this borrow still observes the structural revision captured at acquisition. */
        [[nodiscard]] bool IsCurrent() const noexcept;
        /** @brief Returns the exact committed structural revision captured by this current view.
         * @return Non-zero revision when current, or zero for an empty/stale view; the scene owner must remain alive.
         */
        [[nodiscard]] std::uint64_t StructuralRevision() const noexcept;
        /** @brief Returns the owning runtime identity. */
        [[nodiscard]] SceneRuntimeId RuntimeId() const noexcept;
        /** @brief Returns the logical definition identity. */
        [[nodiscard]] SceneDefinitionId DefinitionId() const noexcept;
        /** @brief Returns the activated authored revision. */
        [[nodiscard]] SceneDefinitionRevision DefinitionRevision() const noexcept;
        /** @brief Returns the asset-registry revision used to prepare this scene. */
        [[nodiscard]] Assets::AssetRegistryRevision AssetRegistryRevision() const noexcept;
        /** @brief Returns the number of allocated slots, including inactive and retired slots. */
        [[nodiscard]] std::size_t SlotCount() const noexcept;
        /** @brief Returns an active slot view or an empty value for inactive/out-of-range slots. @param slot Zero-based
         * slot index. @return Borrowed active entity view when present. */
        [[nodiscard]] std::optional<RuntimeEntityView> EntityAt(std::size_t slot) const noexcept;
        /** @brief Resolves a stable authored identity without allocation. @param object Non-zero authored identity. @return
         * Current runtime reference when mapped. */
        [[nodiscard]] std::optional<EntityRef> Find(SceneObjectId object) const noexcept;
        /** @brief Resolves one scene-owned cooked payload without allocation. @param id Stable asset identity. @return
         * Borrowed payload while this view remains current, or empty when absent. */
        [[nodiscard]] std::optional<RuntimeSceneAssetView> FindAsset(Assets::AssetId id) const noexcept;
        /** @brief Validates and returns one runtime entity view. @param entity Generation-checked reference to validate.
         * @return Borrowed entity view or a typed stale-reference error. */
        [[nodiscard]] Result<RuntimeEntityView> Get(EntityRef entity) const;

    private:
        friend class RuntimeScene;
        explicit RuntimeSceneView(const RuntimeScene &scene) noexcept;
        const RuntimeScene *scene_{};
        std::uint64_t structuralRevision_{};
    };

    /** @brief Owner of one instantiated scene registry and all typed component values. */
    class RuntimeScene final {
    public:
        /** @brief Instantiates an assetless validated definition into a fresh runtime domain. Asset-bearing definitions
         * must use RuntimeSceneService::QueuePreparation. @param definition Validated immutable construction input.
         * @param runtimeId Unique non-zero runtime identity. @param config Generation retirement policy. @return Owned
         * scene or a typed construction error. */
        [[nodiscard]] static Result<std::unique_ptr<RuntimeScene>> Create(const RuntimeSceneDefinition &definition,
                                                                          SceneRuntimeId runtimeId, RuntimeSceneConfig config = {});

        struct ResolvedAsset {
            SceneAssetDependency dependency;
            std::shared_ptr<const std::vector<std::uint8_t>> payload;
        };

        RuntimeScene(SceneRuntimeId runtimeId, SceneDefinitionId definitionId, SceneDefinitionRevision revision, RuntimeSceneConfig config,
                     Assets::AssetRegistryRevision assetRevision, std::vector<ResolvedAsset> assets) noexcept;
        RuntimeScene(const RuntimeScene &) = delete;
        RuntimeScene &operator=(const RuntimeScene &) = delete;
        RuntimeScene(RuntimeScene &&) = delete;
        RuntimeScene &operator=(RuntimeScene &&) = delete;
        /** @brief Returns an immutable borrowed scene view. */
        [[nodiscard]] RuntimeSceneView View() const noexcept;
        /** @brief Applies a structural batch atomically, leaving the scene unchanged on failure. @param commands
         * Owner-thread command batch consumed by the operation. @return Created-token resolutions and destroy count, or the
         * first typed error. Resource-bearing groups require RuntimeSceneService's authoritative registry path. */
        [[nodiscard]] Result<StructuralCommitResult> Commit(const SceneCommandBuffer &commands);

    private:
        friend class RuntimeSceneView;
        friend class RuntimeSceneService;

        [[nodiscard]] Result<StructuralCommitResult> CommitWithRegistry(
            const SceneCommandBuffer &commands, const Assets::AssetRegistry *registry,
            std::span<const std::unique_ptr<SceneStructuralParticipant>> participants = {},
            std::optional<Error> *notificationError = nullptr);

        [[nodiscard]] static Result<std::unique_ptr<RuntimeScene>> CreateResolved(const RuntimeSceneDefinition &definition,
                                                                                  SceneRuntimeId runtimeId, RuntimeSceneConfig config,
                                                                                  Assets::AssetRegistryRevision assetRevision,
                                                                                  std::vector<ResolvedAsset> assets);

        struct Slot {
            std::uint32_t generation{1};
            bool active{};
            bool retired{};
            std::optional<SceneObjectId> authoredObject;
            std::optional<EntityId> parent;
            Math::Transform localTransform;
            std::optional<PrimitiveMeshDescriptor> primitiveMesh;
            RuntimeComponentSet components;
            std::shared_ptr<const std::vector<RuntimeGroupAssetLease>> groupResources;
            std::shared_ptr<const std::vector<RuntimeResolvedGroupReference>> groupReferences;
            std::shared_ptr<const std::vector<Assets::AssetId>> groupSpawnLineage;
            std::shared_ptr<const std::vector<ResolvedGroupPhysicsBodyReference>> groupPhysicsReferences;
        };

        /** @brief Copyable transactional state without a runtime-domain identity. */
        struct RuntimeSceneStorage {
            std::vector<Slot> slots;
            std::vector<std::uint32_t> freeList;
            std::vector<std::pair<SceneObjectId, EntityId>> authoredIndex;
        };

        struct CommandApplier;
        [[nodiscard]] Result<EntityRef> CreateEntity(RuntimeSceneStorage &storage, const RuntimeEntityCreateInfo &info) const;
        [[nodiscard]] Result<void> DestroyEntity(RuntimeSceneStorage &storage, EntityRef entity) const;
        [[nodiscard]] bool IsValid(const RuntimeSceneStorage &storage, EntityRef entity) const noexcept;

        SceneRuntimeId runtimeId_;
        SceneDefinitionId definitionId_;
        SceneDefinitionRevision definitionRevision_;
        Assets::AssetRegistryRevision assetRegistryRevision_;
        std::vector<ResolvedAsset> assets_;
        RuntimeSceneConfig config_;
        RuntimeSceneStorage storage_;
        std::uint64_t structuralRevision_{1};
    };

    /** @brief Runtime lifecycle participant owning active, pending, and structural scene state. */
    class RuntimeSceneService final : public RuntimeLifecycleParticipant {
    public:
        /** @brief Creates an assetless scene service for editor previews and headless typed-scene tests. */
        RuntimeSceneService();
        /** @brief Creates a scene service with borrowed asset services. @param registry Registry that outlives this
         * service. @param loads Load service that outlives this service. @param limits Per-candidate preparation limits. */
        RuntimeSceneService(Assets::AssetRegistry &registry, Assets::AssetLoadService &loads, RuntimeSceneAssetLimits limits = {});
        ~RuntimeSceneService() override;
        /** @brief Adds one owned activation participant before startup. @param participant Non-null unique owner.
         * @return Success or a typed invalid-input/lifecycle error. */
        [[nodiscard]] Result<void> AddActivationParticipant(std::unique_ptr<SceneActivationParticipant> participant);
        /** @brief Queues validated preparation and later safe-point activation. Asset-bearing definitions pin the current
         * registry snapshot and load through the injected service. @param definition Immutable scene definition consumed by
         * the operation; lvalue callers retain source compatibility through a boundary copy. @param config Generation retirement policy.
         * @return Success when accepted, or a typed immediate validation/admission error. */
        [[nodiscard]] Result<void> QueuePreparation(RuntimeSceneDefinition definition, RuntimeSceneConfig config = {});
        /** @brief Queues detached preparation with an additional owned pure publication check.
         * @param definition Immutable complete runtime definition consumed by this operation.
         * @param publicationCheck Optional predicate retained through preparation and revalidated before aggregate publication.
         * @param config Generation retirement policy.
         * @return Owned receipt for a nonnull check; deferred failures preserve the active scene and reach TakeOperationError().
         * Ordinary null-check preparation succeeds with an unobservable receipt; QueuePreparation preserves its void result.
         * @details Existing cancellation, replacement and shutdown paths retire the predicate with the pending operation.
         */
        [[nodiscard]] Result<ScenePublicationReceipt> QueuePreparationWithPublicationCheck(
            RuntimeSceneDefinition definition, std::unique_ptr<ScenePublicationCheck> publicationCheck, RuntimeSceneConfig config = {});
        /** @brief Queues a composite save restore through the existing exclusive Scene admission path.
         * @param definition Validated compatible immutable authored defaults.
         * @param restore Non-null owned restore composition, retained through pending preparation/rollback.
         * @param config Generation retirement policy.
         * @return Typed admission result; failed/pending owner preparation never publishes a partial Scene.
         * @details Normal QueuePreparation callers retain their existing block preparation behavior.
         */
        [[nodiscard]] Result<void> QueuePreparationWithRestore(RuntimeSceneDefinition definition,
                                                               std::unique_ptr<SceneAggregateRestore> restore,
                                                               RuntimeSceneConfig config = {});
        /** @brief Queues active-scene unload; repeated unload with no pending transition is harmless. */
        [[nodiscard]] Result<void> QueueUnload();
        /** @brief Queues one structural batch against the current active scene. @param commands Batch consumed on success.
         * @return Success or a typed state/pending-operation error. */
        [[nodiscard]] Result<void> QueueStructuralCommands(SceneCommandBuffer commands);
        /** @brief Queues a Scene transaction with dedicated completion evidence without consuming global host notifications.
         * @param commands Complete owner-lane command batch.
         * @return Retainable read-only receipt or rejection without queued work. No callback is invoked by observation.
         */
        [[nodiscard]] Result<std::shared_ptr<const SceneStructuralReceipt>> QueueTrackedStructuralCommands(SceneCommandBuffer commands);
        /** @brief Registers exactly one explicit owner adapter before service startup.
         * @param participant Owned adapter; its borrowed subsystem authority outlives the service.
         * @return Success or null/duplicate/late registration failure without replacing another owner.
         */
        [[nodiscard]] Result<void> AddStructuralParticipant(std::unique_ptr<SceneStructuralParticipant> participant);
        /** @brief Returns the current immutable active scene view. */
        [[nodiscard]] std::optional<RuntimeSceneView> ActiveScene() const noexcept;
        /**
         * @brief Clones the active prepared scene into a separate runtime domain.
         * @param runtimeId Unique identity for the clone; must not match the active domain.
         * @return Independent topology/component state sharing only immutable cooked asset payloads.
         */
        [[nodiscard]] Result<std::unique_ptr<RuntimeScene>> CloneActive(SceneRuntimeId runtimeId) const;
        /** @brief Returns and clears the newest structural commit result. */
        [[nodiscard]] std::optional<StructuralCommitResult> TakeStructuralCommitResult();
        /** @brief Returns and clears the newest recoverable operation error. */
        [[nodiscard]] std::optional<Error> TakeOperationError();

        [[nodiscard]] Result<void> Startup(const CancellationToken &cancellation) override;
        [[nodiscard]] Result<void> OnPhase(RuntimePhase phase, const FrameContext &context) override;
        [[nodiscard]] Result<void> OnFixedUpdate(const FixedStepContext &context) override;
        void Shutdown() noexcept override;

    private:
        enum class TransitionKind : std::uint8_t {
            None,
            Activate,
            Unload
        };

        struct Preparation;
        struct AggregateTransfer;
        struct LifecycleMutation;

        struct SceneAggregate final {
            SceneAggregate() = default;
            SceneAggregate(const SceneAggregate &) = delete;
            SceneAggregate &operator=(const SceneAggregate &) = delete;
            SceneAggregate(SceneAggregate &&) noexcept = default;
            SceneAggregate &operator=(SceneAggregate &&) noexcept = default;

            std::unique_ptr<RuntimeScene> scene;
            std::vector<std::unique_ptr<SceneActivationCandidate>> candidates;
            SceneCanonicalDatasetProjection datasets{SceneCanonicalDatasetProjection::Absent};
            std::unique_ptr<SceneAggregateRestore> restore; /**< Pins committed identity/fixup/module roots until Scene retirement. */
        };

        [[nodiscard]] Result<void> BeginPreparation(RuntimeSceneDefinition definition, RuntimeSceneConfig config);
        /** @brief Stages the validated runtime scene and its detached participant candidates without publication. */
        [[nodiscard]] Result<void> StageCandidate(const RuntimeSceneDefinition &definition, RuntimeSceneConfig config,
                                                  Assets::AssetRegistryRevision revision, std::vector<RuntimeScene::ResolvedAsset> assets);
        [[nodiscard]] Result<void> PopulatePreparationEntries(Preparation &prep, const RuntimeSceneDefinition &definition) const;
        [[nodiscard]] Result<void> ProcessCompletedPreparationLoads();
        [[nodiscard]] Result<void> FinalizePreparation();
        void AdvancePreparation();
        void CancelPreparation(bool waitForCompletion) noexcept;
        [[nodiscard]] Result<void> SubmitPreparationLoads();
        [[nodiscard]] Result<void> PrepareParticipants(const RuntimeSceneDefinition &definition);
        static void ShutdownCandidates(std::vector<std::unique_ptr<SceneActivationCandidate>> &candidates) noexcept;
        [[nodiscard]] Result<void> CommitDeferredChanges();
        /** @brief Waits for owners and validates every candidate before any ownership transfer.
         * @return Ready, pending, or the original typed preparation/validation failure. */
        [[nodiscard]] Result<bool> PreparePendingPublication();
        /** @brief Transfers all prepared roots and retains the retired aggregate through completion observers.
         * @return Success or the original pre-transfer gate failure. */
        [[nodiscard]] Result<void> PublishPendingAggregate();
        /** @brief Records rejection and retires unpublished candidates while retaining the authority through cleanup. */
        void RejectPendingPublication(Error error);
        /** @brief Retires only an unpublished queue receipt; already published identities remain immutable. */
        void RetirePublicationReceipt(ScenePublicationStatus status) noexcept;

        SceneAggregate active_;
        SceneAggregate pending_;
        std::vector<std::unique_ptr<SceneActivationParticipant>> participants_;
        std::vector<std::unique_ptr<SceneStructuralParticipant>> structuralParticipants_;
        std::unique_ptr<Preparation> preparation_;
        std::unique_ptr<ScenePublicationCheck> publicationCheck_;
        std::shared_ptr<ScenePublicationDetail::State> publicationReceipt_;
        std::unique_ptr<SceneAggregateRestore> aggregateRestore_;
        std::optional<SceneCommandBuffer> structuralCommands_;
        std::shared_ptr<SceneStructuralReceipt> structuralReceipt_;
        std::optional<StructuralCommitResult> structuralResult_;
        std::optional<Error> operationError_;
        std::uint64_t nextRuntimeId_{1};
        Assets::AssetRegistry *assetRegistry_{};
        Assets::AssetLoadService *assetLoads_{};
        RuntimeSceneAssetLimits assetLimits_{};
        TransitionKind transition_{TransitionKind::None};
        bool started_{};
        bool shutdown_{};
        bool mutatingLifecycle_{}; /**< Rejects reentrant mutation during owned lifecycle callbacks and retirement. */
        bool shutdownDeferred_{};  /**< Shutdown waits until the current scoped lifecycle mutation returns. */
    };
}  // namespace Horo::Runtime
