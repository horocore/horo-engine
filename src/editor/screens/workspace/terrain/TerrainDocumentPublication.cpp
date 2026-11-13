#include "TerrainDocumentHost.h"
#include "TerrainDocumentHostInternal.h"

#include <algorithm>
#include <limits>
#include <new>

namespace Horo::Editor {
    namespace {
        /** @brief Source and historical metadata native authority must coexist through the final replacement. */
        Result<ExclusiveFileLock> LockSource(const std::filesystem::path &root, const std::filesystem::path &path,
                                             DurableFileSystem &files) {
            auto lockPath = Assets::AssetSourcePublicationLockPath(root, path);
            if (lockPath.HasError())
                return Result<ExclusiveFileLock>::Failure(std::move(lockPath).ErrorValue());
            return files.TryAcquireExclusive(lockPath.Value(), "terrain.source.publish");
        }

        Result<void> CheckMetadata(const std::filesystem::path &root, const std::filesystem::path &path,
                                   const Assets::AssetSourceIdentity &accepted) {
            auto current = Assets::CaptureAssetSource(root, path, 256 * 1024);
            if (current.HasError())
                return Result<void>::Failure(std::move(current).ErrorValue());
            if (current.Value().identity != accepted)
                return Result<void>::Failure(MakeError(Assets::AssetSourceErrors::Conflict));
            return Result<void>::Success();
        }

        /** @brief Preserve the document's visible conflict fact without translating the originating native diagnostic. */
        void ObserveConflict(bool &conflict, const Error &error) noexcept {
            conflict = conflict || error.code.Value() == Assets::AssetSourceErrors::Conflict.code.Value();
        }

        /** @brief An explicit rebind initializes complete normalized channels; no old weight slot is silently relabelled. */
        Result<void> Rebind(Terrain::TerrainAuthoringSource &value, const std::optional<Terrain::TerrainAuthoringLayerBinding> &binding) {
            if (binding == value.layers)
                return Result<void>::Success();
            std::uint8_t count{};
            if (binding) {
                while (count < binding->order.size() && binding->order[count].IsValid())
                    ++count;
                if (count == 0)
                    return Result<void>::Failure(MakeError(TerrainEditErrors::Invalid));
            }
            value.layers = binding;
            value.source.layerCount = count;
            value.source.weights.assign(value.source.heightsMeters.size() * count, 0);
            if (count != 0)
                for (std::size_t sample = 0; sample < value.source.heightsMeters.size(); ++sample)
                    value.source.weights[sample * count] = 65535;
            return Result<void>::Success();
        }

        /** @brief The replacement owner and source revision are both advanced before native publication can begin. */
        Result<TerrainAuthoringDocument> ConvertedDocument(Terrain::TerrainAuthoringSource &value, const TerrainEditFence &fence,
                                                           const std::optional<Terrain::TerrainAuthoringLayerBinding> &binding,
                                                           std::uint32_t tileCells) {
            using Return = Result<TerrainAuthoringDocument>;
            if (fence.session.Value() == std::numeric_limits<std::uint64_t>::max())
                return Return::Failure(MakeError(TerrainEditErrors::Exhausted));
            auto revision = Terrain::AdvanceTerrainRevision(value.source.revision);
            if (revision.HasError())
                return Return::Failure(std::move(revision).ErrorValue());
            if (auto rebound = Rebind(value, binding); rebound.HasError())
                return Return::Failure(std::move(rebound).ErrorValue());
            value.source.revision = revision.Value();
            if (value.source.holes.empty())
                value.source.holes.resize(value.source.heightsMeters.size(), 0);
            return TerrainAuthoringDocument::Open(TerrainDocumentSessionId::Create(fence.session.Value() + 1).Value(), value.source,
                                                  tileCells, TerrainAuthoringCapability::Edit, TerrainHostEditLimits(),
                                                  value.foliage.placements);
        }

        struct PreparedConversion final {
            TerrainAuthoringDocument document;
            std::vector<std::byte> bytes;
        };

        /** @brief Own both the replacement session and its complete encoded payload before native publication. */
        Result<PreparedConversion> PrepareConversion(Terrain::TerrainAuthoringSource current, const TerrainEditFence &fence,
                                                     const std::optional<Terrain::TerrainAuthoringLayerBinding> &binding,
                                                     std::uint32_t tileCells) {
            using Return = Result<PreparedConversion>;
            auto document = ConvertedDocument(current, fence, binding, tileCells);
            if (document.HasError())
                return Return::Failure(std::move(document).ErrorValue());
            auto encoded = Terrain::EncodeTerrainAuthoringSource(current);
            if (encoded.HasError())
                return Return::Failure(std::move(encoded).ErrorValue());
            return Return::Success({std::move(document).Value(), std::move(encoded).Value()});
        }
    }  // namespace

    /** @copydoc TerrainDocumentHost::Publish */
    Result<Assets::AssetSourcePublication> TerrainDocumentHost::Publish(std::span<const std::byte> bytes,
                                                                        const std::optional<Terrain::TerrainAuthoringLayerBinding> &layers,
                                                                        const ProjectMutationCoordinator &mutations,
                                                                        DurableFileSystem &files) {
        using Return = Result<Assets::AssetSourcePublication>;
        if (conflict_)
            return Return::Failure(MakeError(Assets::AssetSourceErrors::Conflict));
        auto lease = mutations.TryAcquire({root_, ProjectMutationOwner::Save, "terrain.source.save"});
        if (lease.HasError())
            return Return::Failure(std::move(lease).ErrorValue());
        auto sourceLock = LockSource(root_, path_, files);
        if (sourceLock.HasError())
            return Return::Failure(std::move(sourceLock).ErrorValue());
        auto metadataLock = LockSource(root_, metadataPath_, files);
        if (metadataLock.HasError())
            return Return::Failure(std::move(metadataLock).ErrorValue());
        if (auto metadata = CheckMetadata(root_, metadataPath_, acceptedMetadata_); metadata.HasError()) {
            ObserveConflict(conflict_, metadata.ErrorValue());
            return Return::Failure(std::move(metadata).ErrorValue());
        }
        if (auto resources = ValidateResources(); resources.HasError())
            return Return::Failure(std::move(resources).ErrorValue());
        if (layers) {
            if (auto valid = ValidateLayer(*layers); valid.HasError())
                return Return::Failure(std::move(valid).ErrorValue());
        }
        auto publication = Assets::PublishAssetSource(root_, path_, accepted_, bytes, sourceLock.Value(), files);
        if (publication.HasError()) {
            ObserveConflict(conflict_, publication.ErrorValue());
            return publication;
        }
        accepted_ = publication.Value().identity;
        durabilityUnconfirmed_ = publication.Value().status != Assets::AssetSourcePublicationStatus::Durable;
        return publication;
    }

    /** @copydoc TerrainDocumentHost::Convert */
    Result<Assets::AssetSourcePublication> TerrainDocumentHost::Convert(const TerrainEditFence &fence,
                                                                        const std::optional<Terrain::TerrainAuthoringLayerBinding> &layers,
                                                                        const ProjectMutationCoordinator &mutations,
                                                                        DurableFileSystem &files) {
        using Return = Result<Assets::AssetSourcePublication>;
        if (auto admitted = Admit(fence); admitted.HasError())
            return Return::Failure(std::move(admitted).ErrorValue());
        if (document_.IsDirty() || durabilityUnconfirmed_)
            return Return::Failure(MakeError(TerrainEditErrors::Invalid));
        const TerrainHostBusy busy(busy_);
        try {
            const auto binding = layers ? layers : layers_;
            auto captured = CaptureCurrent();
            if (captured.HasError())
                return Return::Failure(std::move(captured).ErrorValue());
            auto prepared = PrepareConversion(std::move(captured).Value(), fence, binding, TileCellCount);
            if (prepared.HasError())
                return Return::Failure(std::move(prepared).ErrorValue());
            auto candidate = std::move(prepared).Value();
            auto publication = Publish(candidate.bytes, binding, mutations, files);
            if (publication.HasError())
                return publication;
            document_ = std::move(candidate.document);
            layers_ = binding;
            containerSource_ = true;
            return publication;
        } catch (const std::bad_alloc &) {
            return Return::Failure(MakeError(TerrainEditErrors::LimitExceeded));
        } catch (...) {
            return Return::Failure(MakeError(Assets::AssetSourceErrors::Authority));
        }
    }
}  // namespace Horo::Editor
