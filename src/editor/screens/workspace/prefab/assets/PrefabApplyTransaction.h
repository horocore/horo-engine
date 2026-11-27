#pragma once

/** @file PrefabApplyTransaction.h @brief Paired Prefab and Scene owner publication. */
#include "Horo/Editor/SceneDocumentTransaction.h"
#include "editor/document/PrefabApplyPreparation.h"
#include "editor/screens/workspace/prefab/document/PrefabDocumentSession.h"
#include "editor/screens/workspace/prefab/hierarchy/PrefabHierarchySources.h"

#include <array>

namespace Horo::Editor {
    /** @brief Remaining compensation evidence when the exact installed bytes could not be restored. */
    struct PrefabApplyRecovery final {
        std::filesystem::path path;
        Assets::AssetSourceCapture original;
        Assets::AssetSourceIdentity installed;
        std::optional<Assets::AssetSourceIdentity> restoredUnconfirmed;
        Error failure;
    };

    /** @brief Application outcome preserving the original failure and each unconfirmed compensation separately. */
    struct PrefabApplyOutcome final {
        std::optional<SceneTransactionResult> committed;
        std::optional<PrefabApplyHistoryGroup> group;
        Assets::AssetSourceIdentity sceneIdentity;
        std::string sceneChecksum;
        std::optional<Error> error;
        std::array<std::optional<PrefabApplyRecovery>, 2> recovery;
        std::optional<Error> recoveryPersistenceError;
        bool pendingRecovery{};
    };

    /** @brief The explicit resolution allowed by the reviewed durable boundary. */
    enum class PrefabApplyRecoveryAction : std::uint8_t {
        RestoreOriginals,
        KeepPublished,
    };

    /** @brief The exact planned, original, and last reviewed byte identities of one destination. */
    struct PrefabApplyRecoveryFile final {
        std::filesystem::path path;
        Assets::AssetSourceIdentity original;
        Assets::AssetSourceIdentity planned;
        Assets::AssetSourceIdentity observed;

        bool operator==(const PrefabApplyRecoveryFile &) const = default;
    };

    /** @brief Immutable recovery evidence for a review of the two guarded canonical files. */
    struct PrefabApplyRecoveryCandidate final {
        Assets::AssetSourceIdentity capsule;
        PrefabApplyRecoveryFile prefab;
        PrefabApplyRecoveryFile scene;
        PrefabApplyRecoveryAction action;

        bool operator==(const PrefabApplyRecoveryCandidate &) const = default;
    };

    /** @brief Exact matching owner and disk evidence for one paired undo or redo. */
    struct PrefabApplyReplayRequest final {
        SceneTransactionContext scene;
        PrefabDocumentFence source;
        std::filesystem::path scenePath;
        Assets::AssetSourceIdentity sceneIdentity;
        PrefabApplyHistoryGroup group;
        bool redo{};
    };

    /** @brief Exact live Scene and source evidence for a single override promotion. */
    struct PrefabApplyTransactionRequest final {
        SceneTransactionContext scene;
        RevertScenePrefabOverridesCommand selection;
        PrefabDocumentFence source;
        std::filesystem::path scenePath;
        Assets::AssetSourceIdentity sceneIdentity;
    };

    /** @brief Coordinates the real source and Scene owners under one project mutation lease.
     * @details The planned pair and exact originals are recorded before either canonical file changes.
     * Committed bytes remain authoritative across restart. Incomplete operations can compensate only their own writes.
     */
    class PrefabApplyTransaction final {
    public:
        /** @brief Applies the captured selection and returns a durable owner commit or explicit recovery evidence. */
        [[nodiscard]] static PrefabApplyOutcome Execute(SceneDocumentTransactions &scene, PrefabDocumentSession &prefab,
                                                        const PrefabApplyTransactionRequest &request,
                                                        const Assets::AssetRegistrySnapshot &registry,
                                                        const PrefabHierarchySources &sources,
                                                        std::shared_ptr<const Prefab::PrefabValueSchemaSnapshot> schemas,
                                                        const Prefab::PrefabLimitProfile &limits,
                                                        const ProjectMutationCoordinator &mutations, DurableFileSystem &files);
        /** @brief Reviews durable intent and both current source identities without making any changes. */
        [[nodiscard]] static Result<std::optional<PrefabApplyRecoveryCandidate>> InspectPendingRecovery(const std::filesystem::path &root);
        /** @brief Rechecks reviewed evidence before restoring an incomplete pair or retaining a fully published pair.
         * @param root Canonical project root. @param expected Exact reviewed capsule and source identities.
         * @param mutations Shared writer authority. @param files Durable native source owner.
         * @return Success once the selected pair is durable and the journal is retired, or a typed failure.
         */
        [[nodiscard]] static Result<void> ResolvePendingRecovery(const std::filesystem::path &root,
                                                                 const PrefabApplyRecoveryCandidate &expected,
                                                                 const ProjectMutationCoordinator &mutations, DurableFileSystem &files);

        /** @brief Returns the next exact coordinated Prefab group from Scene history, if one is at its head. */
        [[nodiscard]] static Result<std::optional<PrefabApplyHistoryGroup>> NextSceneGroup(const SceneDocumentTransactions &scene,
                                                                                           const SceneTransactionContext &context,
                                                                                           bool redo);

        /** @brief Retires unreachable paired boundaries on both owners, preserving contiguous reachable history. */
        static void ReconcileHistory(SceneDocumentTransactions &scene, const SceneTransactionContext &context,
                                     std::span<const std::unique_ptr<PrefabDocumentSession>> sources);
        /** @brief Audits accepted source bytes. A confirmed external replacement retires dependent source history. */
        [[nodiscard]] static Result<void> InspectSource(PrefabDocumentSession &source);

        /** @brief Replays the same matched group in both owners and writes both resulting files together. */
        [[nodiscard]] static PrefabApplyOutcome Replay(SceneDocumentTransactions &scene, PrefabDocumentSession &prefab,
                                                       const PrefabApplyReplayRequest &request, const ProjectMutationCoordinator &mutations,
                                                       DurableFileSystem &files);

    private:
        /** @brief Publishes the pair of reserved owners, compensating owned bytes on any early failure. */
        [[nodiscard]] static PrefabApplyOutcome Publish(SceneDocumentTransactions &scene, PrefabDocumentSession &prefab,
                                                        SceneDocumentTransactions::Prepared &preparedScene,
                                                        PrefabDocumentSession::PreparedApply &preparedSource,
                                                        const PrefabApplyReplayRequest &request, DurableFileSystem &files);
    };
}  // namespace Horo::Editor
