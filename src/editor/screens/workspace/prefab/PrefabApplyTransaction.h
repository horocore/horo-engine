#pragma once

/** @file PrefabApplyTransaction.h @brief Paired Prefab and Scene owner publication. */
#include "Horo/Editor/SceneDocumentTransaction.h"
#include "editor/document/PrefabApplyPreparation.h"
#include "editor/screens/workspace/prefab/PrefabDocumentSession.h"
#include "editor/screens/workspace/prefab/PrefabHierarchySources.h"

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
    };

    /** @brief Exact original identity of one reviewed canonical destination. */
    struct PrefabApplyRecoveryFile final {
        std::filesystem::path path;
        Assets::AssetSourceIdentity original;
    };

    /** @brief Immutable recovery evidence for a review of the two guarded canonical files. */
    struct PrefabApplyRecoveryCandidate final {
        Assets::AssetSourceIdentity capsule;
        PrefabApplyRecoveryFile prefab;
        PrefabApplyRecoveryFile scene;
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
     * @details Both semantic histories and encoded candidates are staged before writing. Ordinary failures
     * compensate owned replacements in reverse order. An unprovable or failed compensation retains evidence.
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
        /** @brief Inspects the exact durable compensation capsule without changing either source. */
        [[nodiscard]] static Result<std::optional<PrefabApplyRecoveryCandidate>> InspectPendingRecovery(const std::filesystem::path &root);
        /** @brief Explicitly retries pending compensation against the inspected capsule in a fresh project session.
         * @param root Canonical project root. @param expected Exact inspected capsule identity.
         * @param mutations Shared writer authority. @param files Durable native source owner.
         * @return Success after both originals are durable, or a typed failure preserving the capsule and foreign edits.
         */
        [[nodiscard]] static Result<void> RestorePendingRecovery(const std::filesystem::path &root,
                                                                 const Assets::AssetSourceIdentity &expected,
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
