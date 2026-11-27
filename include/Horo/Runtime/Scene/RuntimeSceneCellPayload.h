#pragma once

/** @file RuntimeSceneCellPayload.h
 * @brief Scene-owned immutable flattened cell baseline and fenced RuntimeScene admission.
 */
#include "Horo/Runtime/Scene/SceneInstances.h"
#include "Horo/WorldStreaming/WorldPartitionDescriptor.h"

#include <array>

namespace Horo::Runtime {
    namespace SceneCellPayloadErrors {
        /** @brief Malformed cell identity, revision, limits or source. */
        extern const ErrorCodeDescriptor Invalid;
        /** @brief Captured scene content or streaming attempt was superseded. */
        extern const ErrorCodeDescriptor Stale;
        /** @brief Complete baseline exceeds its explicit storage ceilings. */
        extern const ErrorCodeDescriptor CapacityExceeded;
        /** @brief A core component mode or required gameplay schema is unsupported by the captured cook capabilities. */
        extern const ErrorCodeDescriptor Unsupported;
        /** @brief Cooperative cancellation suppressed unpublished output. */
        extern const ErrorCodeDescriptor Cancelled;
    }  // namespace SceneCellPayloadErrors

    /** @brief Durable baseline identity; runtime generations never enter cooked content. */
    struct SceneCellPayloadIdentity final {
        WorldStreaming::WorldPartitionId partition; /**< Stable world dataset. */
        WorldStreaming::StreamingCellId cell;       /**< Exact owning cell. */
        SceneDefinitionId scene;                    /**< Logical identity of the flattened cell scene. */
        SceneDefinitionRevision revision;           /**< Exact non-zero cooked source revision. */
        [[nodiscard]] constexpr auto operator<=>(const SceneCellPayloadIdentity &) const noexcept = default;
    };

    /** @brief Mandatory load/cook-time storage ceilings, separate from live streaming reservations. */
    struct SceneCellPayloadLimits final {
        std::size_t maximumEntities{};      /**< Positive maximum entity count; an empty cell remains valid. */
        std::size_t maximumDependencies{};  /**< Positive maximum complete asset dependency count. */
        std::size_t maximumRetainedBytes{}; /**< Positive logical owned bytes, including nested vector/string contents. */
    };

    /** @brief Exact supported gameplay component schema from the captured project cook configuration. */
    struct SceneCellComponentSchema final {
        Gameplay::ComponentTypeId type;
        std::uint32_t version{};
    };

    /** @brief Exact supported behavior schema; cook never discovers or loads implementations. */
    struct SceneCellBehaviorSchema final {
        Gameplay::BehaviorTypeId type;
        std::uint32_t version{};
    };

    /** @brief Immutable global position authority tied to one exact object in the same cooked baseline. */
    struct SceneCellWorldAnchor final {
        SceneObjectId object;
        std::array<std::int64_t, 3> positionMillimeters;
        std::optional<std::array<std::int64_t, 3>> cameraTargetMillimeters;
        [[nodiscard]] auto operator<=>(const SceneCellWorldAnchor &) const noexcept = default;
    };

    /** @brief Borrowed complete offline-expanded runtime input for one cell, with no editor state or nested prefab loads. */
    struct SceneCellPayloadSource final {
        SceneCellPayloadIdentity identity;
        std::span<const RuntimeEntityDefinition> entities;
        std::span<const SceneAssetDependency> dependencies;
        std::span<const SceneCellComponentSchema> componentSchemas;
        std::span<const SceneCellBehaviorSchema> behaviorSchemas;
        std::span<const TriggerVolumeMigrationEvidence> triggerVolumeEvidence; /**< Complete portable binding constraints. */
        std::span<const SceneCellWorldAnchor> worldAnchors; /**< Explicit authored authority. An empty list grants no global positions. */
    };

    /** @brief Owned immutable CoreEcs semantic baseline; encoding remains the Scene provider's responsibility. */
    class RuntimeSceneCellPayload final {
    public:
        RuntimeSceneCellPayload(const RuntimeSceneCellPayload &) = default;
        RuntimeSceneCellPayload(RuntimeSceneCellPayload &&) noexcept = default;
        RuntimeSceneCellPayload &operator=(const RuntimeSceneCellPayload &) = delete;
        RuntimeSceneCellPayload &operator=(RuntimeSceneCellPayload &&) = delete;
        /** @brief Returns durable content identity. @return Immutable payload-owned identity. */
        [[nodiscard]] const SceneCellPayloadIdentity &Identity() const noexcept;
        /** @brief Returns the source-free, fully validated runtime definition. @return Borrowed immutable baseline. */
        [[nodiscard]] const RuntimeSceneDefinition &Definition() const noexcept;
        /** @brief Returns host logical owned storage charge. @return Bytes checked against the captured cook ceiling. */
        [[nodiscard]] std::size_t RetainedBytes() const noexcept;
        /** @brief Returns explicit global anchors in object identity order. @return Borrowed immutable owner evidence. */
        [[nodiscard]] std::span<const SceneCellWorldAnchor> WorldAnchors() const noexcept;

    private:
        friend Result<RuntimeSceneCellPayload> CookRuntimeSceneCellPayload(const WorldStreaming::WorldPartitionDescriptor &,
                                                                           const SceneCellPayloadSource &, const SceneCellPayloadIdentity &,
                                                                           SceneCellPayloadLimits, const CancellationToken &);
        /** @brief Owns the complete validated baseline and checked logical storage charge. */
        RuntimeSceneCellPayload(SceneCellPayloadIdentity identity, RuntimeSceneDefinition definition, std::size_t retainedBytes,
                                std::vector<SceneCellWorldAnchor> worldAnchors) noexcept;
        SceneCellPayloadIdentity identity_;
        RuntimeSceneDefinition definition_;
        std::size_t retainedBytes_{};
        std::vector<SceneCellWorldAnchor> worldAnchors_;
    };

    /**
     * @brief Cooks one complete flattened baseline without touching live Scene or authoring state.
     * @param partition Immutable topology authority; the owning cell must be present.
     * @param source Complete typed cell snapshot after offline prefab expansion and schema projection.
     * @param expected Exact source publication captured by the cook request; mismatch returns Stale.
     * @param limits Mandatory entity, dependency and logical retained-storage ceilings.
     * @param cancellation Observed between entity/dependency units and before publication.
     * @return Complete immutable baseline, or a typed invalid, stale, capacity, unsupported or cancellation failure.
     * @details Preserves authored entity order, IDs, hierarchy and all supported typed/opaque component data.
     * Missing external parents fail rather than silently flattening transforms. Required gameplay schemas must be declared;
     * no migration or nested prefab admission occurs. All borrowed data is copied before success. No wire schema is introduced.
     * @throws std::bad_alloc on bounded ownership or validation scratch allocation failure; no output is published.
     */
    [[nodiscard]] Result<RuntimeSceneCellPayload> CookRuntimeSceneCellPayload(const WorldStreaming::WorldPartitionDescriptor &partition,
                                                                              const SceneCellPayloadSource &source,
                                                                              const SceneCellPayloadIdentity &expected,
                                                                              SceneCellPayloadLimits limits,
                                                                              const CancellationToken &cancellation = {});

    /** @brief Host-composed owner-thread authority retained by a shared lease until the queued Scene operation retires. */
    class SceneCellPayloadAuthority {
    public:
        virtual ~SceneCellPayloadAuthority() = default;
        /** @brief Validates current content, full fence, admission/reservations and required-provider publication barrier.
         * @param identity Exact immutable baseline publication.
         * @param fence Exact mounted partition/cell residency attempt.
         * @return Success or the authority's typed stale, cancelling, closed or readiness rejection.
         * @details Pure, bounded owner-thread check. Retained through the pending operation; no I/O or mutation.
         */
        [[nodiscard]] virtual Result<void> ValidatePublication(const SceneCellPayloadIdentity &identity,
                                                               const WorldStreaming::StreamingFence &fence) const = 0;
    };

    /** @brief Complete immutable owner evidence and explicit resident limits for a cell attachment. */
    struct SceneCellAttachmentRequest final {
        SceneRuntimeId runtime; /**< Exact receiving domain: live ordinarily, next unpublished ID for startup. */
        WorldStreaming::StreamingFence fence;
        Assets::AssetRegistryRevision registry;
        SceneBaselineAttachmentLimits limits;
        SceneDefinitionRevision expectedRevision; /**< Zero requires absence; otherwise exact replacement revision. */
        CancellationToken cancellation;
        CancellationToken ownerCancellation;
        std::shared_ptr<const SceneGroupPublication> publication; /**< Optional feature owner for live Streamed attachment or removal. */
    };

    /** @brief Finds exact committed cell ownership in an immutable Scene view.
     * @param scene Borrowed canonical Scene publication. @param identity Exact durable content publication.
     * @param epoch Exact mounted partition incarnation.
     * @return Borrowed ownership only for the matching cell, content revision and partition incarnation.
     */
    [[nodiscard]] std::optional<SceneBaselineAttachmentView> FindRuntimeSceneCellAttachment(RuntimeSceneView scene,
                                                                                            const SceneCellPayloadIdentity &identity,
                                                                                            WorldStreaming::PartitionEpoch epoch) noexcept;

    /** @brief Cancels only the exact pending cell operation; published resident state is preserved.
     * @param service Canonical Scene owner. @param identity Captured pending content identity and revision.
     * @param request Captured runtime and complete residency attempt fence.
     * @return Success or typed stale/foreign-operation rejection without cancelling another attempt.
     */
    [[nodiscard]] Result<void> CancelRuntimeSceneCellOperation(RuntimeSceneService &service, const SceneCellPayloadIdentity &identity,
                                                               const SceneCellAttachmentRequest &request);

    /** @brief Attaches or replaces an independent cell through the canonical Scene structural transaction.
     * @param service Existing canonical world Scene service. @param payload Complete immutable CoreEcs baseline.
     * @param request Exact runtime/residency/catalog/cancellation evidence and positive aggregate ceilings.
     * @param resources Prepared named artifact closure in payload dependency order, consumed on admission.
     * @param authority Non-null shared owner lease validating complete provider readiness and full residency fence.
     * @return Typed admission result; safe-point failures reach TakeOperationError without partial ownership.
     * @details Baseline Scene IDs and authored object IDs must be unique in the final world. Assets/provider work is
     * prepared by existing owners; this seam performs no I/O or backend discovery. Caller content may retire after return.
     */
    [[nodiscard]] Result<void> QueueRuntimeSceneCellAttachment(RuntimeSceneService &service, const RuntimeSceneCellPayload &payload,
                                                               const SceneCellAttachmentRequest &request,
                                                               std::vector<RuntimeGroupAssetLease> resources,
                                                               std::shared_ptr<const SceneCellPayloadAuthority> authority);

    /** @brief Queues a streamed child under one exact live parent using real cell residency authority.
     * @param service Canonical parent Scene owner.
     * @param address Exact Streamed mode, authored occurrence, parent runtime and prepared baseline definition.
     * @param payload Complete immutable cooked cell baseline. @param request Current residency/catalog and bounds.
     * @param resources Prepared verified dependency leases. @param authority Current shared streaming owner.
     * @param replaces Exact currently published operation, or null to require absence.
     * @return Exact cancellable receipt or typed failure. The canonical safe point owns publication.
     * @details The parent admits one resident baseline per definition. Replacement must advance its content revision.
     */
    [[nodiscard]] Result<SceneInstanceOperation> QueueStreamedSceneInstance(
        RuntimeSceneService &service, const SceneInstanceAddress &address, const RuntimeSceneCellPayload &payload,
        const SceneCellAttachmentRequest &request, std::vector<RuntimeGroupAssetLease> resources,
        std::shared_ptr<const SceneCellPayloadAuthority> authority, const SceneInstanceOperation *replaces = nullptr);

    /** @brief Prepares one Streamed baseline inside the exact unpublished root before Scene activation callbacks.
     * @param service Receiving Scene service on its construction lane.
     * @param parent Immutable root definition, using normal Scene preparation and asset discovery.
     * @param address Exact next Scene runtime, Streamed occurrence and child definition.
     * @param payload Verified assetless Core ECS cell baseline. Camera, light and procedural mesh data are supported.
     * @param request Exact catalog and streaming admission evidence, cancellation and attachment limits. Replacement must be absent.
     * @param authority Shared streaming readiness authority retained until root publication or rejection.
     * @return Tracked root publication receipt or typed admission error. Deferred failure preserves the current active Scene.
     * @details The cell is visible to activation participants in the private root. The regular root safe point commits both together.
     * The caller owns its streaming reservation and observes the receipt before exposing Active residency.
     */
    [[nodiscard]] Result<ScenePublicationReceipt> QueuePreparationWithInitialStreamedScene(
        RuntimeSceneService &service, const RuntimeSceneDefinition &parent, const SceneInstanceAddress &address,
        const RuntimeSceneCellPayload &payload, const SceneCellAttachmentRequest &request,
        std::shared_ptr<const SceneCellPayloadAuthority> authority);

    /** @brief Observes an exact current streamed instance through its parent's canonical cell attachment.
     * @param parent Current parent view. @param address Expected Streamed occurrence.
     * @param identity Exact committed cell content. @param epoch Exact mounted partition.
     * @return Borrowed baseline evidence, or absence after parent retirement or mismatch.
     */
    [[nodiscard]] std::optional<SceneBaselineAttachmentView> FindStreamedSceneInstance(const RuntimeSceneView &parent,
                                                                                       const SceneInstanceAddress &address,
                                                                                       const SceneCellPayloadIdentity &identity,
                                                                                       WorldStreaming::PartitionEpoch epoch) noexcept;

    /** @brief Retires the exact streamed occurrence at the canonical Scene safe point.
     * @param service Canonical parent Scene owner. @param current Exact committed operation.
     * @param request Current exact eviction fence and parent/catalog/cancellation evidence.
     * @param authority Retained streaming authority for eviction.
     * @return Exact cancellable receipt or typed failure, preserving all unmatched resident instances.
     */
    [[nodiscard]] Result<SceneInstanceOperation> QueueStreamedSceneRemoval(RuntimeSceneService &service,
                                                                           const SceneInstanceOperation &current,
                                                                           const SceneCellAttachmentRequest &request,
                                                                           std::shared_ptr<const SceneCellPayloadAuthority> authority);

    /** @brief Retires exactly one attached baseline at the canonical Scene safe point.
     * @param service Canonical Scene owner. @param identity Exact committed baseline identity and revision.
     * @param request Current canonical runtime/residency/catalog/cancellation evidence.
     * @param authority Retained owner lease revalidated immediately before publication.
     * @return Typed result; stale revision, cancellation and outside hierarchy dependencies preserve the complete cell.
     */
    [[nodiscard]] Result<void> QueueRuntimeSceneCellDetachment(RuntimeSceneService &service, const SceneCellPayloadIdentity &identity,
                                                               const SceneCellAttachmentRequest &request,
                                                               std::shared_ptr<const SceneCellPayloadAuthority> authority);

    /**
     * @brief Queues a source-free baseline through RuntimeSceneService's existing detached aggregate preparation.
     * @param service Scene owner for this cell candidate; this replaces that domain, never merges into an unrelated scene.
     * @param payload Immutable cooked content; retained by boundary copy, caller may immediately retire it.
     * @param fence Exact activation attempt; must match the payload's partition and cell.
     * @param authority Non-null shared authority lease revalidated at admission and immediately before aggregate publication.
     * @param cancellation Retained observer; cancellation before commit discards the candidate and preserves active Scene.
     * @return Typed admission failure or success; deferred failure is returned by service.TakeOperationError().
     * @details Publication occurs only at CommitDeferredLifecycleChanges. Existing asset-resolution, replacement, unload
     * and shutdown ownership stays in RuntimeSceneService. The host owns eviction of an already committed cell.
     */
    [[nodiscard]] Result<void> QueueRuntimeSceneCellPayload(RuntimeSceneService &service, const RuntimeSceneCellPayload &payload,
                                                            const WorldStreaming::StreamingFence &fence,
                                                            std::shared_ptr<const SceneCellPayloadAuthority> authority,
                                                            const CancellationToken &cancellation = {});
}  // namespace Horo::Runtime
