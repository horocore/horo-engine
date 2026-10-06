#pragma once

/** @file SceneRestoreBundle.h
 * @brief Concrete Scene-owned identity, component, reference and staged-owner restore composition.
 */

#include "Horo/Runtime/Save/SaveRestoreTransaction.h"
#include "Horo/Runtime/Scene/RestoreReferenceGraph.h"

namespace Horo::Runtime {
    /** @brief Core transform override addressed through one exact stable incarnation. */
    struct SceneRestoreTransform final {
        PersistentEntityId entity;
        PersistentEntityGeneration generation;
        Math::Transform transform;
    };

    /** @brief Component payload and the exact schema references owned by that component record. */
    struct SceneRestoreComponent final {
        SavedComponentStateRecord record;
        SaveParticipantId owner;
        std::vector<SaveRestoreReferenceId> references;
    };

    /** @brief Sole operation producer and actual detached owner receipts returned when archive preparation is ready. */
    struct SceneRestorePreparedOwners final {
        SaveOperationController operation;
        std::vector<std::unique_ptr<IStagedRestoreParticipant>> receipts;
    };

    /**
     * @brief Explicit application-owned source of archive-validated inactive restore receipts.
     * @details The source retains the sole controller, source bytes and worker lifetimes until transfer.
     *          Pending does not release operation authority. Destruction cancels/retires its owned work.
     */
    class ISceneRestoreOwnerSource {
    public:
        virtual ~ISceneRestoreOwnerSource() = default;
        /** @brief Transfers ready receipts, or reports that bounded detached preparation is still pending.
         * @param candidate Actual unpublished Scene domain; borrowed only for this call.
         * @return Ready owned producer/receipts, pending absence, or original typed worker failure.
         * @details Before returning pending, the sole producer must observe caller/parent/deadline
         *          cancellation and return its original terminal cause if cancellation won. Pending
         *          cannot conceal an expired or cancelled operation behind unfinished worker readiness.
         */
        [[nodiscard]] virtual Result<std::optional<SceneRestorePreparedOwners>> TakeReady(RuntimeSceneView candidate) = 0;
        /** @brief Closes admission and cooperatively cancels/joins or retains owned worker lifetimes safely. */
        virtual void Cancel() noexcept = 0;
    };

    /** @brief Live application/session evidence, never derived by echoing captured source generations. */
    class ISceneRestoreGenerationAuthority {
    public:
        virtual ~ISceneRestoreGenerationAuthority() = default;
        /** @brief Reads current owner generations at the lifecycle boundary. @return Exact current evidence. */
        [[nodiscard]] virtual StagedRestoreActivationEvidence Current() const noexcept = 0;
    };

    /** @brief Owned immutable inputs qualified by archive and compatibility preflight before Scene admission. */
    struct SceneRestoreBundleRequest final {
        StagedRestoreContext context;
        SaveWorldId world;
        SceneDefinitionId definition;
        SceneDefinitionRevision revision;
        SaveParticipantRegistrySnapshot participants;
        std::vector<PersistentEntityRecord> entities;
        std::vector<SceneRestoreTransform> transforms;
        std::vector<SceneRestoreComponent> components;
        std::vector<RestoreReferenceTarget> nodes;
        std::vector<RestoreReferenceRequest> references;
        std::shared_ptr<const SaveableComponentAdapterRegistry> componentAdapters;
        const Gameplay::GameServiceRegistry *services{}; /**< Frozen owner pinned by serviceLease. */
        std::vector<Gameplay::GameplayServiceId> activeServices;
        std::uint64_t serviceGeneration{};
        std::shared_ptr<void> serviceLease;
        RestoreReferenceGraphLimits graphLimits;
        std::size_t maximumEntities{16'384};
        std::size_t maximumComponentBytes{16U * 1024U * 1024U};
    };

    /** @brief Actual unpublished Scene composition whose owner roots share the staged save transaction gate. */
    class SceneRestoreBundle final : public SceneAggregateRestore {
        struct State;

        /** @brief Nonaggregate admission key constructible only by the validated factory. */
        class ConstructionKey final {
            friend class SceneRestoreBundle;
            ConstructionKey() noexcept = default;

        public:
            ConstructionKey(const ConstructionKey &) noexcept = default;
        };

    public:
        /** @brief Admits owned bounded inputs and explicit source/current authority without invoking owner callbacks.
         * @param request Archive-qualified immutable input and pinned registry/module evidence.
         * @param source Sole operation producer and detached receipt source.
         * @param authority Shared live generation authority retained through rollback/retirement.
         * @return Owned bundle or typed invalid/budget/allocation failure; no active-state mutation.
         */
        [[nodiscard]] static Result<std::unique_ptr<SceneRestoreBundle>> Create(
            SceneRestoreBundleRequest request, std::unique_ptr<ISceneRestoreOwnerSource> source,
            std::shared_ptr<const ISceneRestoreGenerationAuthority> authority);
        /** @brief Transfers validated factory-owned state using an inaccessible admission key.
         * @param key Factory-only proof that admission completed; callers cannot construct this key.
         * @param state Sole owned bounded candidate state; supplied only by Create.
         */
        explicit SceneRestoreBundle(ConstructionKey key, std::unique_ptr<State> state) noexcept;
        ~SceneRestoreBundle() override;
        /** @copydoc SceneAggregateRestore::PrepareScene */
        [[nodiscard]] Result<void> PrepareScene(RuntimeScene &scene) override;
        /** @copydoc SceneAggregateRestore::PrepareOwners */
        [[nodiscard]] Result<bool> PrepareOwners() override;
        /** @copydoc SceneAggregateRestore::ValidatePublication */
        [[nodiscard]] Result<void> ValidatePublication(const RuntimeSceneView *active) const override;
        /** @copydoc SceneAggregateRestore::Commit */
        [[nodiscard]] Result<void> Commit(IStagedRestoreAggregatePublication &sceneTransfer) override;
        /** @copydoc SceneAggregateRestore::Rollback */
        void Rollback() noexcept override;
        /** @brief Returns committed or prepared exact identity bindings while this bundle is retained.
         * @return Borrowed map, or nullptr before successful Scene preparation. */
        [[nodiscard]] const PersistentEntityIdentityMap *Identities() const noexcept;

    private:
        std::unique_ptr<State> state_;
    };
}  // namespace Horo::Runtime
