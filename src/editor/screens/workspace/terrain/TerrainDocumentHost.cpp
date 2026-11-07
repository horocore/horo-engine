#include "TerrainDocumentHost.h"

#include "Horo/Editor/SourceDocumentService.h"
#include "TerrainDocumentHostInternal.h"

#include <new>

namespace Horo::Editor {
    namespace {
        constexpr std::size_t MaximumHeightBytes = 64 * 1024 * 1024;
        constexpr std::size_t MaximumMetadataBytes = 256 * 1024;

        struct SourceCapture final {
            Terrain::TerrainAuthoringSource value;
            Assets::AssetSourceIdentity source, metadata;
            bool container{};
        };

        /** @brief Admit the asset document identity only for its exact workspace source path. */
        Result<void> ValidateOpening(const std::filesystem::path &root, const std::filesystem::path &path, const DocumentIdentity &identity,
                                     Assets::AssetId workspace) {
            if (!identity.IsValid() || identity.key.kind != DocumentKind::Asset || !workspace.IsValid() ||
                path != root / std::filesystem::path(std::u8string(identity.key.source.Value().begin(), identity.key.source.Value().end())))
                return Result<void>::Failure(MakeError(TerrainEditErrors::Invalid));
            return Result<void>::Success();
        }

        /** @brief Container bytes are authoritative after conversion; legacy RAW retains its exact importer settings authority. */
        Result<Terrain::TerrainAuthoringSource> DecodeCaptured(std::span<const std::byte> bytes, const std::filesystem::path &metadata,
                                                               Assets::AssetId asset, Terrain::TerrainCapabilityRevision capability,
                                                               bool container) {
            using Return = Result<Terrain::TerrainAuthoringSource>;
            if (container)
                return Terrain::DecodeTerrainAuthoringSource(bytes, asset, capability);
            auto imported = Assets::ReadAssetImportMetadata(metadata);
            if (imported.HasError())
                return Return::Failure(std::move(imported).ErrorValue());
            auto decoded = DecodeTerrainAsset(imported.Value(), bytes, Terrain::TerrainSourceRevision::Create(1).Value());
            if (decoded.HasError())
                return Return::Failure(std::move(decoded).ErrorValue());
            auto source = std::move(decoded).Value();
            source.capability = capability;
            return Return::Success({std::move(source), {}, {}});
        }

        /** @brief Check actual source identity/type through the canonical registry sidecar decoder. */
        Result<void> SourceIdentity(const std::filesystem::path &root, const std::filesystem::path &path, Assets::AssetId asset,
                                    std::span<const std::byte> bytes) {
            const auto text = std::string_view(reinterpret_cast<const char *>(bytes.data()), bytes.size());
            const auto relative = path.lexically_relative(root).generic_u8string();
            auto identity = Assets::DecodeAssetIdentitySidecar(std::string(relative.begin(), relative.end()), text);
            if (identity.HasError())
                return Result<void>::Failure(std::move(identity).ErrorValue());
            if (identity.Value().id != asset || identity.Value().type.Value() != "core.terrain")
                return Result<void>::Failure(MakeError(Assets::AssetSourceErrors::Conflict));
            return Result<void>::Success();
        }

        /** @brief Read source and historical metadata under the caller's project lease; compare the actual metadata generation. */
        Result<SourceCapture> Capture(const std::filesystem::path &root, const std::filesystem::path &path, Assets::AssetId asset,
                                      Terrain::TerrainCapabilityRevision capability) {
            using Return = Result<SourceCapture>;
            auto sidecar = path;
            sidecar += ".horo";
            auto source = Assets::CaptureAssetSource(root, path, MaximumHeightBytes);
            if (source.HasError())
                return Return::Failure(std::move(source).ErrorValue());
            auto metadata = Assets::CaptureAssetSource(root, sidecar, MaximumMetadataBytes);
            if (metadata.HasError())
                return Return::Failure(std::move(metadata).ErrorValue());
            if (!source.Value().identity.exists || !metadata.Value().identity.exists)
                return Return::Failure(MakeError(Assets::AssetSourceErrors::ReadFailed));
            if (auto valid = SourceIdentity(root, path, asset, metadata.Value().bytes); valid.HasError())
                return Return::Failure(std::move(valid).ErrorValue());
            SourceCapture capture;
            capture.source = source.Value().identity;
            capture.metadata = metadata.Value().identity;
            capture.container = Terrain::IsTerrainAuthoringSource(source.Value().bytes);
            auto decoded = DecodeCaptured(source.Value().bytes, sidecar, asset, capability, capture.container);
            if (decoded.HasError())
                return Return::Failure(std::move(decoded).ErrorValue());
            auto current = Assets::CaptureAssetSource(root, sidecar, MaximumMetadataBytes);
            if (current.HasError())
                return Return::Failure(std::move(current).ErrorValue());
            if (current.Value().identity != capture.metadata)
                return Return::Failure(MakeError(Assets::AssetSourceErrors::Conflict));
            capture.value = std::move(decoded).Value();
            return Return::Success(std::move(capture));
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
                                                                           const ProjectMutationCoordinator &mutations,
                                                                           const Assets::AssetRegistrySnapshot &registry) {
        using Return = Result<std::shared_ptr<TerrainDocumentHost>>;
        if (auto admitted = ValidateOpening(root, path, identity, workspace); admitted.HasError())
            return Return::Failure(std::move(admitted).ErrorValue());
        try {
            auto lease = mutations.TryAcquire({root, ProjectMutationOwner::Save, "terrain.source.open"});
            if (lease.HasError())
                return Return::Failure(std::move(lease).ErrorValue());
            auto capture = Capture(root, path, asset, Terrain::TerrainCapabilityRevision::Create(identity.instance.Value()).Value());
            if (capture.HasError())
                return Return::Failure(std::move(capture).ErrorValue());
            auto value = std::move(capture).Value();
            auto document = TerrainAuthoringDocument::Open(TerrainDocumentSessionId::Create(identity.instance.Value()).Value(),
                                                           std::move(value.value.source), TileCellCount, TerrainAuthoringCapability::Edit,
                                                           TerrainHostEditLimits(), value.value.foliage.placements);
            if (document.HasError())
                return Return::Failure(std::move(document).ErrorValue());
            auto host =
                std::shared_ptr<TerrainDocumentHost>(new TerrainDocumentHost(root, path, std::move(identity), workspace,
                                                                             std::move(document).Value(), value.source, value.metadata));
            host->containerSource_ = value.container;
            host->layers_ = std::move(value.value.layers);
            host->registry_ = registry;
            auto resources = host->LoadResources(value.value.foliage.definitions);
            if (resources.HasError())
                return Return::Failure(std::move(resources).ErrorValue());
            return Return::Success(std::move(host));
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
                document_.IsClosed(),
                containerSource_,
                layers_.has_value()};
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
        const TerrainHostBusy guard(busy_);
        auto produced = ProduceTerrainSculpt(document_.Source(), fence, operation, brush, stamps, {}, cancellation);
        if (produced.HasError())
            return Result<TerrainEditChange>::Failure(std::move(produced).ErrorValue());
        return document_.Execute(produced.Value(), cancellation);
    }

    /** @copydoc TerrainDocumentHost::History */
    Result<TerrainEditChange> TerrainDocumentHost::History(const TerrainEditFence &fence, const bool redo) {
        if (auto admitted = Admit(fence); admitted.HasError())
            return Result<TerrainEditChange>::Failure(std::move(admitted).ErrorValue());
        const TerrainHostBusy guard(busy_);
        return redo ? document_.Redo(fence) : document_.Undo(fence);
    }

    /** @copydoc TerrainDocumentHost::Save */
    Result<Assets::AssetSourcePublication> TerrainDocumentHost::Save(const TerrainEditFence &fence,
                                                                     const ProjectMutationCoordinator &mutations,
                                                                     DurableFileSystem &files) {
        using Return = Result<Assets::AssetSourcePublication>;
        if (auto admitted = Admit(fence); admitted.HasError())
            return Return::Failure(std::move(admitted).ErrorValue());
        const TerrainHostBusy guard(busy_);
        try {
            auto encoded = EncodeCurrent();
            if (encoded.HasError())
                return Return::Failure(std::move(encoded).ErrorValue());
            auto publication = Publish(encoded.Value(), layers_, mutations, files);
            if (publication.HasError())
                return publication;
            if (publication.Value().status == Assets::AssetSourcePublicationStatus::Durable) {
                auto saved = document_.AcceptSavedState(fence.session, document_.State());
                if (saved.HasError())
                    return Return::Failure(std::move(saved).ErrorValue());
            }
            return publication;
        } catch (const std::bad_alloc &) {
            return Return::Failure(MakeError(TerrainEditErrors::LimitExceeded));
        } catch (...) {
            return Return::Failure(MakeError(Assets::AssetSourceErrors::Authority));
        }
    }

    /** @copydoc TerrainDocumentHost::CaptureSavedSource */
    Result<TerrainSavedSource> TerrainDocumentHost::CaptureSavedSource(const TerrainEditFence &fence) const {
        using Return = Result<TerrainSavedSource>;
        if (auto admitted = Admit(fence); admitted.HasError())
            return Return::Failure(std::move(admitted).ErrorValue());
        if (!containerSource_ || document_.IsDirty() || durabilityUnconfirmed_)
            return Return::Failure(MakeError(TerrainEditErrors::CapabilityUnavailable));
        try {
            return Return::Success({root_, path_, document_.Source().sourceAsset, workspace_, fence, accepted_.digest,
                                    acceptedMetadata_.digest, layers_ ? layers_->digest : Sha256Digest{}});
        } catch (const std::bad_alloc &) {
            return Return::Failure(MakeError(TerrainEditErrors::LimitExceeded));
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
