#pragma once

/** @file SceneInstances.h @brief Typed child scene identity and parent-owned structural activation. */
#include "Horo/Runtime/Scene/RuntimeScene.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace Horo::Runtime {
    /** @brief Separates child composition policies at the Scene ownership boundary. */
    enum class SceneInstanceMode : std::uint8_t {
        Embedded,  /**< Child entities share the parent's canonical runtime. */
        Streamed,  /**< A streaming authority controls residency relative to the parent. */
        Activated, /**< Independent child runtime linked to its parent's lifetime. */
    };

    struct SceneInstanceIdentityTag;
    /** @brief Stable authored child occurrence identity within one parent. */
    using SceneInstanceId = SceneStableId<SceneInstanceIdentityTag>;

    /** @brief Logical occurrence scoped to an exact live parent and captured source definition. */
    struct SceneInstanceAddress final {
        SceneRuntimeId parent;
        SceneInstanceId instance;
        SceneDefinitionId definition;
        SceneInstanceMode mode{SceneInstanceMode::Embedded};
        constexpr auto operator<=>(const SceneInstanceAddress &) const noexcept = default;
    };

    /** @brief Exact attach, replace or detach operation. Copies observe the same immutable completion. */
    class SceneInstanceOperation final {
    public:
        /** @brief Borrows the aggregate commit result. @return Exact structural receipt, or null after a move. */
        [[nodiscard]] const SceneStructuralReceipt *Receipt() const noexcept {
            return receipt_.get();
        }

        /**
         * @brief Cancels only this still-queued operation.
         * @param service Exact Scene owner holding the pending structural candidate.
         * @return Success or typed stale/lifecycle failure. A published child remains owned by its parent.
         */
        [[nodiscard]] Result<void> Cancel(RuntimeSceneService &service) const;

    private:
        friend Result<SceneInstanceOperation> QueueEmbeddedSceneInstance(RuntimeSceneService &, const SceneInstanceAddress &,
                                                                         RuntimeSceneDefinition, std::vector<RuntimeGroupAssetLease>,
                                                                         const SceneStructuralAdmission &, SceneBaselineAttachmentLimits,
                                                                         const SceneInstanceOperation *,
                                                                         std::shared_ptr<const ScenePublicationCheck>);
        friend Result<SceneInstanceOperation> QueueEmbeddedSceneRemoval(RuntimeSceneService &, const SceneInstanceOperation &,
                                                                        const SceneStructuralAdmission &);

        SceneInstanceOperation(SceneInstanceAddress address, SceneDefinitionRevision revision,
                               std::shared_ptr<const SceneBaselineOwnership> ownership,
                               std::shared_ptr<const SceneStructuralReceipt> receipt) noexcept;
        SceneInstanceAddress address_;
        SceneDefinitionRevision revision_;
        std::shared_ptr<const SceneBaselineOwnership> ownership_;
        std::shared_ptr<const SceneStructuralReceipt> receipt_;
    };

    /**
     * @brief Queues a prepared embedded child for the current parent's structural commit.
     * @param service Actual parent Scene service.
     * @param address Exact parent, occurrence and source. Only Embedded is admitted by this entry point.
     * @param definition Complete decoded immutable child source. Parent and child definitions must be distinct.
     * @param resources Exact verified dependency leases in the child definition's order.
     * @param admission Parent, registry and cancellation evidence revalidated before publication.
     * @param limits Bounded aggregate attachment capacity.
     * @param replaces Null to attach. For replacement, the exact published current child operation.
     * @param sourceCheck Retainable pure check of the source owner's current publication.
     * @return Cancellable exact structural receipt or immediate typed failure. A parent admits one resident occurrence per definition.
     */
    [[nodiscard]] Result<SceneInstanceOperation> QueueEmbeddedSceneInstance(
        RuntimeSceneService &service, const SceneInstanceAddress &address, RuntimeSceneDefinition definition,
        std::vector<RuntimeGroupAssetLease> resources, const SceneStructuralAdmission &admission, SceneBaselineAttachmentLimits limits,
        const SceneInstanceOperation *replaces, std::shared_ptr<const ScenePublicationCheck> sourceCheck);

    /**
     * @brief Retires only the exact published embedded occurrence at the structural commit.
     * @param service Actual parent Scene service. @param current Exact published attach or replacement operation.
     * @param admission Current parent, registry and cancellation evidence.
     * @return Exact cancellable receipt or typed failure. No other baseline is removed.
     */
    [[nodiscard]] Result<SceneInstanceOperation> QueueEmbeddedSceneRemoval(RuntimeSceneService &service,
                                                                           const SceneInstanceOperation &current,
                                                                           const SceneStructuralAdmission &admission);

    /**
     * @brief Observes a currently published embedded child in the exact parent.
     * @param parent Current owner-lane Scene view. @param address Expected exact occurrence.
     * @return Borrowed current attachment or absence after removal or parent retirement.
     */
    [[nodiscard]] std::optional<SceneBaselineAttachmentView> FindEmbeddedSceneInstance(const RuntimeSceneView &parent,
                                                                                       const SceneInstanceAddress &address) noexcept;
}  // namespace Horo::Runtime
