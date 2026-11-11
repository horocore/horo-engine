#pragma once
/** @file TerrainDocumentHost.h @brief Private workspace-owned asset-rooted terrain authoring and source publication. */
#include "FoliagePaletteModel.h"
#include "Horo/Assets/AssetSourcePublication.h"
#include "Horo/Editor/EditorSurfaceIdentity.h"
#include "Horo/Editor/ProjectMutation.h"
#include "TerrainAssetFormat.h"
#include "TerrainPaintProducer.h"
#include "TerrainWorkspaceTypes.h"

#include <thread>

namespace Horo::Editor {
    /** @brief Captured complete source-layer binding from an actual tracked layer-set asset. */
    struct TerrainLayerSetChoice final {
        Terrain::TerrainAuthoringLayerBinding binding;
        std::string label;
    };

    /** @brief Accepted saved-source publication facts copied only by explicit cook commands. */
    struct TerrainSavedSource final {
        std::filesystem::path root;
        std::filesystem::path path;
        Assets::AssetId asset;
        Assets::AssetId workspace;
        TerrainEditFence fence;
        Sha256Digest sourceDigest;
        Sha256Digest metadataDigest;
        Sha256Digest layerDigest;
    };

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
                                                                               const ProjectMutationCoordinator &mutations,
                                                                               const Assets::AssetRegistrySnapshot &registry = {});
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
        /** @brief Owner-lane immutable document borrow for bounded selection/placement intent production. */
        [[nodiscard]] const TerrainAuthoringDocument *BorrowDocument() const noexcept;
        /** @brief Accepted ordered layer meaning; null for height-only sources. */
        [[nodiscard]] const Terrain::TerrainAuthoringLayerBinding *BorrowLayerBinding() const noexcept;

        /** @brief Canonical derived tile layout used by the sole document owner. */
        [[nodiscard]] std::uint32_t TileCells() const noexcept {
            return TileCellCount;
        }

        /** @brief Actual validated layer-set source choices, retaining the full authored order. */
        [[nodiscard]] std::span<const TerrainLayerSetChoice> BorrowLayerSets() const noexcept;
        /** @brief Actual decoded tracked definition assets; no filesystem work in Draw. */
        [[nodiscard]] std::span<const FoliagePaletteItem> BorrowFoliageCatalog() const noexcept;
        /** @brief Produce and commit normalized weight/hole patches against the complete captured layer meaning. */
        [[nodiscard]] Result<TerrainEditChange> Paint(const TerrainPaintFence &fence, TerrainEditOperationId operation,
                                                      const TerrainPaintBrush &brush, std::span<const TerrainSculptStamp> stamps,
                                                      const ProjectMutationCoordinator &mutations,
                                                      const CancellationToken &cancellation = {});
        /** @brief Commit a detached exact source operation (including erase) through the canonical document owner. */
        [[nodiscard]] Result<TerrainEditChange> Execute(const TerrainEditFence &fence, const TerrainEditOperation &operation,
                                                        const CancellationToken &cancellation = {});
        /** @brief Validate captured actual definition source under the shared project lease before placement assignment. */
        [[nodiscard]] Result<TerrainEditChange> ExecuteFoliage(const TerrainEditFence &fence, const TerrainEditOperation &operation,
                                                               const Terrain::FoliageAuthoringDefinitionReference &definition,
                                                               const ProjectMutationCoordinator &mutations,
                                                               const CancellationToken &cancellation = {});
        /** @brief Explicit clean conversion prepares a fresh session before publishing the complete authoring container.
         * All channels and placements are preserved when no new binding is supplied; a selected layer set initializes the first ordered
         * layer to full weight. Accepted payload identity advances even for directory-sync uncertainty, retaining dirty state and its
         * receipt.
         */
        [[nodiscard]] Result<Assets::AssetSourcePublication> Convert(const TerrainEditFence &fence,
                                                                     const std::optional<Terrain::TerrainAuthoringLayerBinding> &layers,
                                                                     const ProjectMutationCoordinator &mutations, DurableFileSystem &files);
        /** @brief Refresh actual layer/type captures on an explicit owner command, never during Draw. */
        [[nodiscard]] Result<void> RefreshResources(const Assets::AssetRegistrySnapshot &registry,
                                                    const ProjectMutationCoordinator &mutations);
        /** @brief Replay exact bounded patch history; no brush algorithm replay. */
        [[nodiscard]] Result<TerrainEditChange> History(const TerrainEditFence &fence, bool redo);
        /** @brief Publish only canonical height bytes; both source and metadata identity must remain accepted.
         * @param fence Exact viewed fence. @param mutations Existing shared project authority. @param files Host durability authority.
         * @return Existing native publication receipt. Uncertainty advances accepted identity but keeps authored state dirty.
         */
        [[nodiscard]] Result<Assets::AssetSourcePublication> Save(const TerrainEditFence &fence,
                                                                  const ProjectMutationCoordinator &mutations, DurableFileSystem &files);
        /** @brief Capture accepted saved identity for an explicit offline cook; dirty/uncertain/legacy sources reject. */
        [[nodiscard]] Result<TerrainSavedSource> CaptureSavedSource(const TerrainEditFence &fence) const;
        /** @brief Revoke edits/picking; retained const views remain safely inert. */
        [[nodiscard]] Result<void> Close();

    private:
        TerrainDocumentHost(std::filesystem::path root, std::filesystem::path path, DocumentIdentity identity, Assets::AssetId workspace,
                            TerrainAuthoringDocument document, Assets::AssetSourceIdentity source, Assets::AssetSourceIdentity metadata);
        [[nodiscard]] Result<void> Admit(const TerrainEditFence &fence) const;
        [[nodiscard]] Result<void> LoadResources(std::span<const Terrain::FoliageAuthoringDefinitionReference> required);
        [[nodiscard]] Result<Terrain::TerrainAuthoringSource> CaptureCurrent() const;
        [[nodiscard]] Result<void> ValidateDefinition(const Terrain::FoliageAuthoringDefinitionReference &definition) const;
        [[nodiscard]] Result<std::vector<std::byte>> EncodeCurrent() const;
        [[nodiscard]] Result<void> ValidateResources() const;
        [[nodiscard]] Result<void> ValidateLayer(const Terrain::TerrainAuthoringLayerBinding &binding) const;
        [[nodiscard]] Result<Assets::AssetSourcePublication> Publish(std::span<const std::byte> bytes,
                                                                     const std::optional<Terrain::TerrainAuthoringLayerBinding> &layers,
                                                                     const ProjectMutationCoordinator &mutations, DurableFileSystem &files);
        static constexpr std::uint32_t TileCellCount = 128;
        std::filesystem::path root_;
        std::filesystem::path path_;
        std::filesystem::path metadataPath_;
        DocumentIdentity identity_;
        Assets::AssetId workspace_;
        TerrainAuthoringDocument document_;
        Assets::AssetSourceIdentity accepted_;
        Assets::AssetSourceIdentity acceptedMetadata_;
        std::optional<Terrain::TerrainAuthoringLayerBinding> layers_;
        std::vector<FoliagePaletteItem> foliageCatalog_;
        std::vector<TerrainLayerSetChoice> layerSets_;
        Assets::AssetRegistrySnapshot registry_;
        bool containerSource_{};
        bool durabilityUnconfirmed_{};
        std::thread::id owner_;
        bool busy_{};
        bool conflict_{};
    };
}  // namespace Horo::Editor
