#pragma once
/** @file PrefabCreation.h
 * @brief Bounded create-from-selection application transaction over canonical Scene and source owners.
 */
#include "Horo/Assets/AssetSourcePublication.h"
#include "Horo/Editor/SceneDocumentTransaction.h"
#include "Horo/Gameplay/BehaviorRegistry.h"
#include "Horo/Prefab/PrefabDocument.h"

#include <array>

namespace Horo::Editor {
    class ProjectMutationCoordinator;

    /** @brief Immutable gesture captured before opening the transient filename workflow. */
    struct PrefabCreationIntent final {
        SceneSource::SceneObjectId root;
        std::uint64_t documentRevision{};
        std::uint64_t replacementEpoch{};
        std::filesystem::path destination;
    };

    /** @brief One unsupported/crossing authored site; no opaque bytes are interpreted. */
    struct PrefabCreationBoundary final {
        SceneSource::SceneObjectId source;
        std::optional<SceneSource::SceneObjectId> target;
        std::string property;
    };

    /** @brief Retained recovery evidence for exactly one attempted exclusive file creation. */
    struct PrefabCreationReceipt final {
        std::filesystem::path path;
        Assets::AssetSourceIdentity content;
        std::optional<FileEntryInfo> identity;
        bool created{};
        bool removed{};
        std::optional<Error> recoveryError;
    };

    /** @brief Complete owned outcome; failed cleanup receipts remain available for explicit recovery. */
    struct PrefabCreationOutcome final {
        bool committed{};
        Assets::AssetId asset;
        Prefab::PrefabInstanceId instance;
        std::optional<Error> error;
        std::vector<PrefabCreationBoundary> boundaries;
        std::array<PrefabCreationReceipt, 2> files;
    };

    /** @brief Read-only source preparation; no filesystem mutation, scene publication, or provider callback.
     * @param snapshot Exact canonical document capture. @param root Complete ordinary subtree root.
     * @param asset Fresh host-issued asset identity. @param version Captured admitted project version.
     * @param registry Frozen asset identities/types. @param behaviors Exact available schema metadata, or null.
     * @param boundaries Receives every unsupported or crossing site before any source is produced.
     * @return Canonical portable source or typed zero-mutation rejection. Reference-bearing unavailable schemas fail closed.
     */
    [[nodiscard]] Result<Prefab::PrefabDocument> PreparePrefabSelectionSource(const SceneTransactionSnapshot &snapshot,
                                                                              SceneSource::SceneObjectId root, Assets::AssetId asset,
                                                                              const Application::HoroVersion &version,
                                                                              const Assets::AssetRegistrySnapshot &registry,
                                                                              const Gameplay::BehaviorRegistry *behaviors,
                                                                              std::vector<PrefabCreationBoundary> &boundaries);

    /** @brief Last accepted persisted Scene bytes, independent of its dirty in-memory authoring state. */
    struct PrefabCreationSourceFence final {
        std::filesystem::path path;
        Assets::AssetSourceIdentity accepted;
    };

    /** @brief Complete captured create gesture; borrowed behavior authority must remain frozen for this invocation. */
    struct PrefabCreationRequest final {
        SceneTransactionContext context;
        SceneSource::SceneObjectId root;
        std::filesystem::path projectRoot;
        std::filesystem::path destination;
        Assets::AssetId asset;
        Application::HoroVersion version;
        Assets::AssetRegistrySnapshot registry;
        const Gameplay::BehaviorRegistry *behaviors{};
        std::optional<PrefabCreationSourceFence> sceneSource; /**< Absent only for a deliberately unbacked headless document. */
    };

    /** @brief Creates one exclusive source/sidecar pair and commits one fully prepared canonical Scene/history replacement.
     * @param scene Live canonical Scene port. @param request Captured owner fence, source identity, schemas and destination.
     * @param mutations Shared project writer coordinator. @param files Composed durable authority.
     * @return Explicit commit or failure with retained created-file identities and recovery diagnostics.
     * @details Runtime failure rollback only; no cross-file power-loss atomicity is claimed. Undo restores the authored Scene;
     * the newly created reusable asset remains. Foreign replacements are never removed by rollback.
     */
    [[nodiscard]] PrefabCreationOutcome CreatePrefabFromSelection(SceneDocumentTransactions &scene, const PrefabCreationRequest &request,
                                                                  const ProjectMutationCoordinator &mutations, DurableFileSystem &files);
}  // namespace Horo::Editor
