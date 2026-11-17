#pragma once
/** @file PrefabRestoreEntities.h
 * @brief Exact cooked-recipe reconstruction behind the existing Scene restore gate.
 */
#include "Horo/Prefab/PrefabPersistentState.h"
#include "Horo/Prefab/PrefabSpawnService.h"
#include "Horo/Runtime/Scene/SceneRestoreBundle.h"

namespace Horo::Prefab {
    /** @brief One privately reconstructed occurrence; bindings become live only with aggregate publication. */
    struct PrefabRestoredInstance final {
        Runtime::SavePrefabInstanceId identity;
        PrefabTemplateLease source;
        std::vector<Runtime::EntityRef> members; /**< Saved member order; tombstones remain empty. */
    };

    /** @brief Owner-lane adapter retaining actual cooked leases through preparation/publication/retirement.
     * No loader, active Scene mutation, behavior callbacks or second transaction authority lives here.
     */
    class PrefabRestoreEntities final : public Runtime::ISceneRestoreDynamicEntities {
        struct State;

    public:
        /** @brief Validates complete detached recipes and actual completed provider leases before Scene admission.
         * @param saved Owned admitted recipes. @param leases Same recipe order; tombstones may have empty leases.
         * @param provider Same host provider, outliving the adapter. @param components Frozen current schema owner.
         * @param projector Canonical built-in codec, outliving preparation. @param schemas Same typed override schemas.
         * @param limits Aggregate budget. @param cancellation Original restore ancestry.
         * @return Whole owned adapter or original typed incompatibility/budget/allocation failure.
         */
        [[nodiscard]] static Result<std::shared_ptr<PrefabRestoreEntities>> Create(
            PrefabPersistentState saved, std::vector<PrefabTemplateLease> leases, PrefabTemplateProvider &provider,
            const Gameplay::ComponentRegistry *components, const PrefabComponentProjector *projector,
            std::shared_ptr<const PrefabValueSchemaSnapshot> schemas, PrefabPersistenceLimits limits, CancellationToken cancellation = {});
        ~PrefabRestoreEntities() override;
        /** @copydoc Runtime::ISceneRestoreDynamicEntities::Prepare */
        [[nodiscard]] Result<std::vector<Runtime::PersistentEntityRuntimeBinding>> Prepare(
            Runtime::RuntimeScene &scene, std::span<const Runtime::PersistentEntityRecord> records, std::size_t maximumEntities,
            Runtime::SceneRestoreCandidateCommands &commands) override;
        /** @copydoc Runtime::ISceneRestoreDynamicEntities::ValidatePublication */
        [[nodiscard]] Result<void> ValidatePublication() const override;
        /** @copydoc Runtime::ISceneRestoreDynamicEntities::PublishPrepared */
        void PublishPrepared() noexcept override;
        /** @copydoc Runtime::ISceneRestoreDynamicEntities::Cancel */
        void Cancel() noexcept override;
        /** @brief Borrows exact unpublished occurrence bindings for explicit candidate service composition.
         * @return Private bindings after successful preparation, empty before preparation or after cancellation.
         * @details Does not issue Gameplay capabilities or publish inventory; borrow ends with this adapter.
         */
        [[nodiscard]] std::span<const PrefabRestoredInstance> PreparedInstances() const noexcept;

    private:
        /** @brief Adopts only fully validated detached state. */
        explicit PrefabRestoreEntities(std::unique_ptr<State> state) noexcept;
        std::unique_ptr<State> state_;
    };
}  // namespace Horo::Prefab
