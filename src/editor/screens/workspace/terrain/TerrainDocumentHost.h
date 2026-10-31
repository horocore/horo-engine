#pragma once
/** @file TerrainDocumentHost.h @brief Private workspace-owned asset-rooted terrain authoring and source publication. */
#include "Horo/Assets/AssetSourcePublication.h"
#include "Horo/Editor/EditorSurfaceIdentity.h"
#include "Horo/Editor/ProjectMutation.h"
#include "TerrainAssetFormat.h"
#include "TerrainWorkspaceTypes.h"

#include <thread>

namespace Horo::Editor {
    /** @brief One owner of an existing imported height-only terrain asset; canonical mutation remains TerrainAuthoringDocument. */
    class TerrainDocumentHost final {
    public:
        /** @brief Read/validate actual canonical bytes and typed Assets metadata under the project mutation authority.
         * @param root Authorized project root. @param path Exact tracked source path. @param asset Expected Assets identity.
         * @param identity Existing workspace document identity of Asset kind. @param workspace Host-issued unique workspace owner.
         * @param mutations Shared project authority. @return New owner or original typed safety/format/conflict failure.
         */
        [[nodiscard]] static Result<std::shared_ptr<TerrainDocumentHost>> Open(const std::filesystem::path &root,
                                                                               const std::filesystem::path &path, Assets::AssetId asset,
                                                                               DocumentIdentity identity, Assets::AssetId workspace,
                                                                               const ProjectMutationCoordinator &mutations);
        /** @brief Read the cached canonical source only during the owner-thread Draw invocation.
         * @return Const borrow expiring on the next owner command; null for closed/foreign/busy access.
         */
        [[nodiscard]] const Terrain::TerrainCanonicalSource *BorrowSource() const noexcept;
        /** @brief Read scalar document facts without I/O. @return Exact current owner projection. */
        [[nodiscard]] TerrainDocumentView View() const;

        /** @brief Borrow the constant workspace document identity for owner-side tab routing. */
        [[nodiscard]] const DocumentIdentity &Identity() const noexcept {
            return identity_;
        }

        /** @brief Apply one detached stroke through the sole existing semantic transaction owner. */
        [[nodiscard]] Result<TerrainEditChange> Sculpt(const TerrainEditFence &fence, TerrainEditOperationId operation,
                                                       const TerrainSculptBrush &brush, std::span<const TerrainSculptStamp> stamps,
                                                       const CancellationToken &cancellation = {});
        /** @brief Replay exact bounded patch history; no brush algorithm replay. */
        [[nodiscard]] Result<TerrainEditChange> History(const TerrainEditFence &fence, bool redo);
        /** @brief Publish only canonical height bytes; both source and metadata identity must remain accepted.
         * @param fence Exact viewed fence. @param mutations Existing shared project authority. @param files Host durability authority.
         * @return Existing native publication receipt. Uncertainty advances accepted identity but keeps authored state dirty.
         */
        [[nodiscard]] Result<Assets::AssetSourcePublication> Save(const TerrainEditFence &fence,
                                                                  const ProjectMutationCoordinator &mutations, DurableFileSystem &files);
        /** @brief Revoke edits/picking; retained const views remain safely inert. */
        [[nodiscard]] Result<void> Close();

    private:
        TerrainDocumentHost(std::filesystem::path root, std::filesystem::path path, DocumentIdentity identity, Assets::AssetId workspace,
                            TerrainAuthoringDocument document, Assets::AssetSourceIdentity source, Assets::AssetSourceIdentity metadata);
        [[nodiscard]] Result<void> Admit(const TerrainEditFence &fence) const;
        std::filesystem::path root_, path_, metadataPath_;
        DocumentIdentity identity_;
        Assets::AssetId workspace_;
        TerrainAuthoringDocument document_;
        Assets::AssetSourceIdentity accepted_, acceptedMetadata_;
        bool durabilityUnconfirmed_{};
        std::thread::id owner_;
        bool busy_{}, conflict_{};
    };
}  // namespace Horo::Editor
