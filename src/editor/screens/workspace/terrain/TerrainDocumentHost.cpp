#include "TerrainDocumentHost.h"

#include "Horo/Editor/SourceDocumentService.h"

#include <new>

namespace Horo::Editor {
    namespace {
        constexpr std::size_t MaximumHeightBytes = 4 * 1024 * 1024;
        constexpr std::size_t MaximumMetadataBytes = 256 * 1024;

        /** @brief Protect every callback-capable mutation/publication against reentry. */
        class Busy final {
        public:
            explicit Busy(bool &value) : value_(value) {
                value_ = true;
            }

            ~Busy() {
                value_ = false;
            }

            Busy(const Busy &) = delete;
            Busy &operator=(const Busy &) = delete;

        private:
            bool &value_;
        };

        /** @brief Explicit per-document authoring reservations keep four workspace owners bounded. */
        TerrainEditLimits Limits() {
            TerrainEditLimits value;
            value.maximumPatches = 1;
            value.maximumSamples = 16'384;
            value.maximumTransactionBytes = 512 * 1024;
            value.maximumHistoryBytes = 8 * 1024 * 1024;
            value.maximumHistoryItems = 64;
            return value;
        }
    }  // namespace

    /** @copydoc TerrainDocumentHost::TerrainDocumentHost */
    TerrainDocumentHost::TerrainDocumentHost(std::filesystem::path root, std::filesystem::path path, DocumentIdentity identity,
                                             Assets::AssetId workspace, TerrainAuthoringDocument document,
                                             Assets::AssetSourceIdentity source, Assets::AssetSourceIdentity metadata)
        : root_(std::move(root)), path_(std::move(path)), metadataPath_(path_), identity_(std::move(identity)), workspace_(workspace),
          document_(std::move(document)), accepted_(source), acceptedMetadata_(metadata), owner_(std::this_thread::get_id()) {
        metadataPath_ += ".horo";
    }

    /** @copydoc TerrainDocumentHost::Open */
    Result<std::shared_ptr<TerrainDocumentHost>> TerrainDocumentHost::Open(const std::filesystem::path &root,
                                                                           const std::filesystem::path &path, Assets::AssetId asset,
                                                                           DocumentIdentity identity, Assets::AssetId workspace,
                                                                           const ProjectMutationCoordinator &mutations) {
        using Return = Result<std::shared_ptr<TerrainDocumentHost>>;
        if (!identity.IsValid() || identity.key.kind != DocumentKind::Asset || !workspace.IsValid() ||
            path != root / std::filesystem::path(std::u8string(identity.key.source.Value().begin(), identity.key.source.Value().end())))
            return Return::Failure(MakeError(TerrainEditErrors::Invalid));
        try {
            auto lease = mutations.TryAcquire({root, ProjectMutationOwner::Save, "terrain.source.open"});
            if (lease.HasError())
                return Return::Failure(std::move(lease).ErrorValue());
            auto sidecar = path;
            sidecar += ".horo";
            auto source = Assets::CaptureAssetSource(root, path, MaximumHeightBytes);
            if (source.HasError())
                return Return::Failure(std::move(source).ErrorValue());
            auto metadataCapture = Assets::CaptureAssetSource(root, sidecar, MaximumMetadataBytes);
            if (metadataCapture.HasError())
                return Return::Failure(std::move(metadataCapture).ErrorValue());
            if (!source.Value().identity.exists || !metadataCapture.Value().identity.exists)
                return Return::Failure(MakeError(Assets::AssetSourceErrors::ReadFailed));
            auto metadata = Assets::ReadAssetImportMetadata(sidecar);
            if (metadata.HasError())
                return Return::Failure(std::move(metadata).ErrorValue());
            if (metadata.Value().assetId != asset)
                return Return::Failure(MakeError(Assets::AssetSourceErrors::Conflict));
            auto currentMetadata = Assets::CaptureAssetSource(root, sidecar, MaximumMetadataBytes);
            if (currentMetadata.HasError())
                return Return::Failure(std::move(currentMetadata).ErrorValue());
            if (currentMetadata.Value().identity != metadataCapture.Value().identity)
                return Return::Failure(MakeError(Assets::AssetSourceErrors::Conflict));
            auto decoded = DecodeTerrainAsset(metadata.Value(), source.Value().bytes, Terrain::TerrainSourceRevision::Create(1).Value());
            if (decoded.HasError())
                return Return::Failure(std::move(decoded).ErrorValue());
            auto document = TerrainAuthoringDocument::Open(TerrainDocumentSessionId::Create(identity.instance.Value()).Value(),
                                                           std::move(decoded).Value(), 128, TerrainAuthoringCapability::Edit, Limits());
            if (document.HasError())
                return Return::Failure(std::move(document).ErrorValue());
            return Return::Success(std::shared_ptr<TerrainDocumentHost>(
                new TerrainDocumentHost(root, path, std::move(identity), workspace, std::move(document).Value(), source.Value().identity,
                                        metadataCapture.Value().identity)));
        } catch (const std::bad_alloc &) {
            return Return::Failure(MakeError(TerrainEditErrors::LimitExceeded));
        } catch (...) {
            return Return::Failure(MakeError(Assets::AssetSourceErrors::Authority));
        }
    }

    /** @copydoc TerrainDocumentHost::BorrowSource */
    const Terrain::TerrainCanonicalSource *TerrainDocumentHost::BorrowSource() const noexcept {
        return owner_ == std::this_thread::get_id() && !busy_ && !document_.IsClosed() ? &document_.Source() : nullptr;
    }

    /** @copydoc TerrainDocumentHost::View */
    TerrainDocumentView TerrainDocumentHost::View() const {
        return {document_.Fence(),
                workspace_,
                identity_.instance,
                document_.IsDirty() || durabilityUnconfirmed_,
                document_.UndoCount() != 0,
                document_.RedoCount() != 0,
                conflict_,
                document_.IsClosed()};
    }

    /** @copydoc TerrainDocumentHost::Admit */
    Result<void> TerrainDocumentHost::Admit(const TerrainEditFence &fence) const {
        if (owner_ != std::this_thread::get_id())
            return Result<void>::Failure(MakeError(SourceDocumentErrors::WrongThread));
        if (busy_)
            return Result<void>::Failure(MakeError(SourceDocumentErrors::Busy));
        if (document_.IsClosed())
            return Result<void>::Failure(MakeError(TerrainEditErrors::Closed));
        const auto current = document_.Fence();
        if (fence.session != current.session || fence.dataset != current.dataset || fence.expectedRevision != current.expectedRevision ||
            fence.capabilityRevision != current.capabilityRevision)
            return Result<void>::Failure(MakeError(TerrainEditErrors::StaleRevision));
        return Result<void>::Success();
    }

    /** @copydoc TerrainDocumentHost::Sculpt */
    Result<TerrainEditChange> TerrainDocumentHost::Sculpt(const TerrainEditFence &fence, TerrainEditOperationId operation,
                                                          const TerrainSculptBrush &brush, std::span<const TerrainSculptStamp> stamps,
                                                          const CancellationToken &cancellation) {
        if (auto admitted = Admit(fence); admitted.HasError())
            return Result<TerrainEditChange>::Failure(std::move(admitted).ErrorValue());
        const Busy guard(busy_);
        auto produced = ProduceTerrainSculpt(document_.Source(), fence, operation, brush, stamps, {}, cancellation);
        if (produced.HasError())
            return Result<TerrainEditChange>::Failure(std::move(produced).ErrorValue());
        return document_.Execute(produced.Value(), cancellation);
    }

    /** @copydoc TerrainDocumentHost::History */
    Result<TerrainEditChange> TerrainDocumentHost::History(const TerrainEditFence &fence, const bool redo) {
        if (auto admitted = Admit(fence); admitted.HasError())
            return Result<TerrainEditChange>::Failure(std::move(admitted).ErrorValue());
        const Busy guard(busy_);
        return redo ? document_.Redo(fence) : document_.Undo(fence);
    }

    /** @copydoc TerrainDocumentHost::Save */
    Result<Assets::AssetSourcePublication> TerrainDocumentHost::Save(const TerrainEditFence &fence,
                                                                     const ProjectMutationCoordinator &mutations,
                                                                     DurableFileSystem &files) {
        using Return = Result<Assets::AssetSourcePublication>;
        if (auto admitted = Admit(fence); admitted.HasError())
            return Return::Failure(std::move(admitted).ErrorValue());
        if (conflict_)
            return Return::Failure(MakeError(Assets::AssetSourceErrors::Conflict));
        const Busy guard(busy_);
        try {
            auto encoded = EncodeTerrainHeights(document_.Source());
            if (encoded.HasError())
                return Return::Failure(std::move(encoded).ErrorValue());
            const auto state = document_.State();
            auto project = mutations.TryAcquire({root_, ProjectMutationOwner::Save, "terrain.source.save"});
            if (project.HasError())
                return Return::Failure(std::move(project).ErrorValue());
            auto sourceLock = Assets::AssetSourcePublicationLockPath(root_, path_);
            if (sourceLock.HasError())
                return Return::Failure(std::move(sourceLock).ErrorValue());
            auto sourceLease = files.TryAcquireExclusive(sourceLock.Value(), "terrain.source.save");
            if (sourceLease.HasError())
                return Return::Failure(std::move(sourceLease).ErrorValue());
            auto metadataLock = Assets::AssetSourcePublicationLockPath(root_, metadataPath_);
            if (metadataLock.HasError())
                return Return::Failure(std::move(metadataLock).ErrorValue());
            auto metadataLease = files.TryAcquireExclusive(metadataLock.Value(), "terrain.metadata.fence");
            if (metadataLease.HasError())
                return Return::Failure(std::move(metadataLease).ErrorValue());
            auto metadata = Assets::CaptureAssetSource(root_, metadataPath_, MaximumMetadataBytes);
            if (metadata.HasError())
                return Return::Failure(std::move(metadata).ErrorValue());
            if (metadata.Value().identity != acceptedMetadata_) {
                conflict_ = true;
                return Return::Failure(MakeError(Assets::AssetSourceErrors::Conflict));
            }
            auto publication = Assets::PublishAssetSource(root_, path_, accepted_, encoded.Value(), sourceLease.Value(), files);
            if (publication.HasError()) {
                if (publication.ErrorValue().code.Value() == Assets::AssetSourceErrors::Conflict.code.Value())
                    conflict_ = true;
                return publication;
            }
            accepted_ = publication.Value().identity;
            if (publication.Value().status == Assets::AssetSourcePublicationStatus::Durable) {
                const auto saved = document_.AcceptSavedState(fence.session, state);
                if (saved.HasError())
                    return Return::Failure(saved.ErrorValue());
                durabilityUnconfirmed_ = false;
            } else
                durabilityUnconfirmed_ = true;
            return publication;
        } catch (const std::bad_alloc &) {
            return Return::Failure(MakeError(TerrainEditErrors::LimitExceeded));
        } catch (...) {
            return Return::Failure(MakeError(Assets::AssetSourceErrors::Authority));
        }
    }

    /** @copydoc TerrainDocumentHost::Close */
    Result<void> TerrainDocumentHost::Close() {
        if (auto admitted = Admit(document_.Fence()); admitted.HasError())
            return admitted;
        document_.Close();
        return Result<void>::Success();
    }
}  // namespace Horo::Editor
