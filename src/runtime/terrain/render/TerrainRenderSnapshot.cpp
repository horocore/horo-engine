#include "Horo/TerrainRenderIntegration/TerrainRenderSnapshot.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <tuple>

namespace Horo::TerrainRender {
    struct TerrainRenderExtractionSnapshot::State final {
        TerrainRenderSnapshotHeader header;
        std::vector<TerrainTileRenderCandidate> tiles;
        std::uint64_t bytes{};
    };

    namespace {
        using namespace Terrain;

        /** @brief Rejects overflow before incrementing independent finite capture charges. */
        bool Charge(std::uint64_t &used, const std::uint64_t amount, const std::uint64_t limit) noexcept {
            if (used > limit || amount > limit - used)
                return false;
            used += amount;
            return true;
        }

        /** @brief Checks host-lowered ceilings against fixed hard limits before reading roots. */
        bool ValidLimits(const TerrainRenderSnapshotLimits &l) noexcept {
            return l.maximumTiles > 0 && l.maximumTiles <= 256 && l.maximumVertices > 0 && l.maximumVertices <= 1'048'576 &&
                   l.maximumIndices > 0 && l.maximumIndices <= 6'291'456 && l.maximumOwnedBytes > 0 &&
                   l.maximumOwnedBytes <= 128ULL * 1024 * 1024 && l.maximumWorkItems > 0 && l.maximumWorkItems <= 8'388'608;
        }

        /** @brief Matches complete cook provenance, preventing same-tile substitution of another root. */
        bool MatchesRoot(const TerrainRenderSnapshotRequest &r) {
            const auto &s = r.source.Tiles();
            const auto &p = r.manifest.Provenance();
            const auto &h = r.header;
            return std::tuple{s.dataset,
                              s.sourceAsset,
                              s.sourceRevision,
                              s.sourceDigest,
                              s.fingerprint,
                              s.manifestDigest,
                              r.source.Fingerprint(),
                              r.source.ManifestDigest(),
                              r.source.Capability(),
                              s.profile.targetDigest} == std::tuple{p.dataset,
                                                                    p.sourceAsset,
                                                                    p.source,
                                                                    p.sourceDigest,
                                                                    p.tileFingerprint,
                                                                    p.tileManifestDigest,
                                                                    p.geometryFingerprint,
                                                                    p.geometryManifestDigest,
                                                                    p.capability,
                                                                    p.targetDigest} &&
                   p.dataset == h.terrain.dataset && p.content == h.revisions.content && p.capability == h.revisions.capability &&
                   p.targetDigest == h.target && r.manifest.Digest() == h.manifest;
        }

        /** @brief Validates lifecycle, exact capability and placement before traversing source data. */
        Result<void> ValidateRequest(const TerrainRenderSnapshotRequest &r, const CancellationToken &cancellation) {
            if (r.lifecycle == TerrainRuntimeLifecycle::Closed || r.lifecycle == TerrainRuntimeLifecycle::Closing)
                return Result<void>::Failure(MakeError(TerrainErrors::LifecycleUnavailable));
            if (r.lifecycle == TerrainRuntimeLifecycle::Cancelled || cancellation.IsCancellationRequested())
                return Result<void>::Failure(MakeError(TerrainErrors::CompositionCancelled));
            const auto &h = r.header;
            if (r.lifecycle != TerrainRuntimeLifecycle::Active || h.schema != 1 || !h.terrain.IsValid() || !h.revisions.IsValid() ||
                !h.world.IsValid() || !h.origin.IsValid() || (h.cell && (!h.cell->IsValid() || h.cell->partition != h.world)) ||
                h.generation == 0 || !h.configuration.IsValid() || h.target == Sha256Digest{} || h.manifest == Sha256Digest{} ||
                !h.capabilities.IsValid() || !h.datasetToWorld.TryToMatrix().HasValue() || h.datasetToWorld.scale.x <= 0 ||
                h.datasetToWorld.scale.y <= 0 || h.datasetToWorld.scale.z <= 0 || !ValidLimits(r.limits))
                return Result<void>::Failure(MakeError(TerrainErrors::DescriptorInvalid));
            if (!h.capabilities.Contains(TerrainFoliageCapability::RenderExtraction) ||
                !h.capabilities.Contains(TerrainFoliageCapability::TerrainRuntime))
                return Result<void>::Failure(MakeError(TerrainErrors::CapabilityUnsupported));
            if (r.tiles.size() > r.limits.maximumTiles)
                return Result<void>::Failure(MakeError(TerrainErrors::CapacityExceeded));
            const auto &config = r.configuration.Data();
            if (!MatchesRoot(r) || config.configuration != h.configuration || config.capability != h.revisions.capability ||
                config.tier != r.source.Tiles().profile.tier)
                return Result<void>::Failure(MakeError(TerrainErrors::RevisionStale));
            return Result<void>::Success();
        }

        /** @brief Returns exact selected visual membership; absence cannot substitute collision/navigation geometry. */
        Result<const TerrainSourceArtifact *> SelectArtifact(const TerrainRenderSnapshotRequest &r,
                                                             const TerrainRenderTileSelection &selection, std::uint64_t &work) {
            const TerrainTilePayloadEntry *membership = nullptr;
            for (const auto &entry : r.manifest.Tiles()) {
                if (!Charge(work, 1, r.limits.maximumWorkItems))
                    return Result<const TerrainSourceArtifact *>::Failure(MakeError(TerrainErrors::CapacityExceeded));
                if (entry.tile == selection.tile)
                    membership = &entry;
            }
            if (!membership || !membership->consumers[0] || membership->consumers[0]->digest != selection.visual)
                return Result<const TerrainSourceArtifact *>::Failure(MakeError(TerrainErrors::DescriptorInvalid));
            for (const auto &artifact : r.source.Artifacts()) {
                if (!Charge(work, 1, r.limits.maximumWorkItems))
                    return Result<const TerrainSourceArtifact *>::Failure(MakeError(TerrainErrors::CapacityExceeded));
                if (artifact.tile == selection.tile && artifact.role == TerrainSourceArtifactRole::Visual) {
                    if (artifact.digest != selection.visual || artifact.payload.size() != membership->consumers[0]->byteSize ||
                        artifact.seams != membership->seams || artifact.maximumGeometricError != membership->maximumGeometricError ||
                        artifact.requiresSameLodNeighbors != membership->requiresSameLodNeighbors)
                        return Result<const TerrainSourceArtifact *>::Failure(MakeError(TerrainErrors::DescriptorInvalid));
                    return Result<const TerrainSourceArtifact *>::Success(&artifact);
                }
            }
            return Result<const TerrainSourceArtifact *>::Failure(MakeError(TerrainErrors::DescriptorInvalid));
        }

        /** @brief Builds finite tile-local float positions with explicit double anchors and precision rejection. */
        Result<void> CopyVertices(const TerrainSourceArtifact &source, const TerrainSourceCoordinates &coordinates,
                                  TerrainTileRenderCandidate &tile, const CancellationToken &cancellation) {
            if (source.vertices.empty())
                return Result<void>::Failure(MakeError(TerrainErrors::DescriptorInvalid));
            const auto &first = source.vertices.front();
            tile.anchor = {first.x, first.y, first.z};
            tile.mesh.vertices.reserve(source.vertices.size());
            for (const auto &v : source.vertices) {
                if (cancellation.IsCancellationRequested())
                    return Result<void>::Failure(MakeError(TerrainErrors::CompositionCancelled));
                const std::array delta{v.x - first.x, v.y - first.y, v.z - first.z};
                Math::Vec3 position{static_cast<float>(delta[0]), static_cast<float>(delta[1]), static_cast<float>(delta[2])};
                const std::array converted{position.x, position.y, position.z};
                for (std::size_t axis = 0; axis < delta.size(); ++axis)
                    if (!std::isfinite(delta[axis]) || !std::isfinite(converted[axis]) ||
                        std::abs(static_cast<double>(converted[axis]) - delta[axis]) > coordinates.maximumPrecisionError)
                        return Result<void>::Failure(MakeError(TerrainErrors::DescriptorInvalid));
                const Math::Vec2 uv{static_cast<float>((v.x - coordinates.originX) / coordinates.spacingX),
                                    static_cast<float>((v.z - coordinates.originZ) / coordinates.spacingZ)};
                if (!Math::IsFinite(uv))
                    return Result<void>::Failure(MakeError(TerrainErrors::DescriptorInvalid));
                tile.mesh.vertices.push_back({position, {}, uv});
            }
            tile.mesh.localBounds = {tile.mesh.vertices.front().position, tile.mesh.vertices.front().position};
            for (const auto &v : tile.mesh.vertices) {
                auto &b = tile.mesh.localBounds;
                b.minimum = {std::min(b.minimum.x, v.position.x), std::min(b.minimum.y, v.position.y), std::min(b.minimum.z, v.position.z)};
                b.maximum = {std::max(b.maximum.x, v.position.x), std::max(b.maximum.y, v.position.y), std::max(b.maximum.z, v.position.z)};
            }
            return Result<void>::Success();
        }

        /** @brief Preserves hole exclusions, validates winding/coverage, and derives bounded area-weighted normals. */
        Result<void> CopyTriangles(const TerrainSourceArtifact &source, const CookedTerrainTileSet &root, TerrainTileRenderCandidate &tile,
                                   const CancellationToken &cancellation) {
            tile.mesh.indices.reserve(source.triangles.size() * 3);
            for (const auto &t : source.triangles) {
                if (cancellation.IsCancellationRequested())
                    return Result<void>::Failure(MakeError(TerrainErrors::CompositionCancelled));
                if (t.beginX >= t.endX || t.beginZ >= t.endZ || t.endX >= root.sourceWidth || t.endZ >= root.sourceHeight ||
                    std::ranges::any_of(t.indices, [&](const auto i) {
                    return i >= tile.mesh.vertices.size();
                }))
                    return Result<void>::Failure(MakeError(TerrainErrors::DescriptorInvalid));
                auto &a = tile.mesh.vertices[t.indices[0]], &b = tile.mesh.vertices[t.indices[1]], &c = tile.mesh.vertices[t.indices[2]];
                const auto ab = b.position - a.position, ac = c.position - a.position;
                const Math::Vec3 n{ab.y * ac.z - ab.z * ac.y, ab.z * ac.x - ab.x * ac.z, ab.x * ac.y - ab.y * ac.x};
                if (!Math::IsFinite(n) || n.y <= 0)
                    return Result<void>::Failure(MakeError(TerrainErrors::DescriptorInvalid));
                a.normal = a.normal + n;
                b.normal = b.normal + n;
                c.normal = c.normal + n;
                tile.mesh.indices.insert(tile.mesh.indices.end(), t.indices.begin(), t.indices.end());
            }
            for (auto &v : tile.mesh.vertices) {
                const double length =
                    std::sqrt(static_cast<double>(v.normal.x) * v.normal.x + static_cast<double>(v.normal.y) * v.normal.y +
                              static_cast<double>(v.normal.z) * v.normal.z);
                if (!std::isfinite(length))
                    return Result<void>::Failure(MakeError(TerrainErrors::DescriptorInvalid));
                v.normal = length > 0 ? v.normal * static_cast<float>(1 / length) : Math::Vec3{0, 1, 0};
            }
            return Result<void>::Success();
        }

        /** @brief Tracks independent capture ceilings across all admitted tiles. */
        struct CaptureBudget {
            std::uint64_t work{}, bytes{}, vertices{}, indices{};
        };

        /** @brief Charges geometry, copied semantics and verification work before allocation. */
        bool AdmitTile(const TerrainRenderSnapshotRequest &r, const TerrainSourceArtifact &a, const TerrainCookedTile &samples,
                       CaptureBudget &budget, const std::size_t captured) {
            const auto count = a.triangles.size() * 3ULL;
            return Charge(budget.vertices, a.vertices.size(), r.limits.maximumVertices) &&
                   Charge(budget.indices, count, r.limits.maximumIndices) &&
                   Charge(budget.bytes,
                          a.vertices.size() * sizeof(Render::MeshVertex) + count * sizeof(std::uint32_t) + samples.payload.size(),
                          r.limits.maximumOwnedBytes) &&
                   Charge(budget.work,
                          a.vertices.size() * 3ULL + count + a.payload.size() / 4096 + samples.payload.size() / 4096 +
                              r.source.Tiles().tiles.size() + captured + 2,
                          r.limits.maximumWorkItems);
        }

        /** @brief Verifies exact sample/layer/hole bytes with bounded cancellation checkpoints. */
        Result<void> VerifySamples(const TerrainCookedTile &samples, const CancellationToken &cancellation) {
            Sha256Builder hash;
            for (std::size_t offset = 0; offset < samples.payload.size();) {
                if (cancellation.IsCancellationRequested())
                    return Result<void>::Failure(MakeError(TerrainErrors::CompositionCancelled));
                const auto chunk = std::min<std::size_t>(4096, samples.payload.size() - offset);
                if (!hash.Update(std::as_bytes(std::span{samples.payload}.subspan(offset, chunk))))
                    return Result<void>::Failure(MakeError(TerrainErrors::DescriptorInvalid));
                offset += chunk;
            }
            return hash.Finalize() == samples.digest ? Result<void>::Success()
                                                     : Result<void>::Failure(MakeError(TerrainErrors::DescriptorInvalid));
        }

        /** @brief Produces one complete detached tile after membership, budget and semantic verification. */
        Result<TerrainTileRenderCandidate> CaptureTile(const TerrainRenderSnapshotRequest &r, const TerrainRenderTileSelection &selection,
                                                       CaptureBudget &budget, const std::size_t captured,
                                                       const CancellationToken &cancellation) {
            using Capture = Result<TerrainTileRenderCandidate>;
            const auto selected = SelectArtifact(r, selection, budget.work);
            if (selected.HasError())
                return Capture::Failure(selected.ErrorValue());
            const auto &a = *selected.Value();
            const auto layers = TerrainMaterialLayerSet::Create(selection.layers, r.configuration);
            if (layers.HasError())
                return Capture::Failure(layers.ErrorValue());
            const auto &root = r.source.Tiles();
            if (layers.Value().Data().layerCount != root.layerCount)
                return Capture::Failure(MakeError(TerrainErrors::DescriptorInvalid));
            const auto samples = std::ranges::find(root.tiles, selection.tile, &TerrainCookedTile::id);
            if (samples == root.tiles.end())
                return Capture::Failure(MakeError(TerrainErrors::DescriptorInvalid));
            if (!AdmitTile(r, a, *samples, budget, captured))
                return Capture::Failure(MakeError(TerrainErrors::CapacityExceeded));
            if (const auto verified = VerifyTerrainSourceArtifactPayload(a.payload, a.digest, cancellation); verified.HasError())
                return Capture::Failure(verified.ErrorValue());
            if (const auto verified = VerifySamples(*samples, cancellation); verified.HasError())
                return Capture::Failure(verified.ErrorValue());
            TerrainTileRenderCandidate tile{.tile = a.tile,
                                            .visual = a.digest,
                                            .samples = samples->digest,
                                            .seams = a.seams,
                                            .maximumGeometricError = a.maximumGeometricError,
                                            .requiresSameLodNeighbors = a.requiresSameLodNeighbors,
                                            .holes = root.hasHoles,
                                            .layers = selection.layers};
            if (const auto copied = CopyVertices(a, root.coordinates, tile, cancellation); copied.HasError())
                return Capture::Failure(copied.ErrorValue());
            if (const auto copied = CopyTriangles(a, root, tile, cancellation); copied.HasError())
                return Capture::Failure(copied.ErrorValue());
            tile.samplePayload = samples->payload;
            return Capture::Success(std::move(tile));
        }
    }  // namespace

    /** @copydoc CaptureTerrainRenderSnapshot */
    Result<TerrainRenderExtractionSnapshot> CaptureTerrainRenderSnapshot(const TerrainRenderSnapshotRequest &r,
                                                                         const CancellationToken &cancellation) {
        using Capture = Result<TerrainRenderExtractionSnapshot>;
        if (const auto valid = ValidateRequest(r, cancellation); valid.HasError())
            return Capture::Failure(valid.ErrorValue());
        CaptureBudget budget{.bytes = sizeof(TerrainRenderExtractionSnapshot::State)};
        if (!Charge(budget.bytes, r.tiles.size() * sizeof(TerrainTileRenderCandidate), r.limits.maximumOwnedBytes) ||
            !Charge(budget.work, r.manifest.Bytes().size() / 4096 + 1, r.limits.maximumWorkItems))
            return Capture::Failure(MakeError(TerrainErrors::CapacityExceeded));
        if (const auto verified = VerifyTerrainPayloadManifest(r.manifest, r.manifest.Bytes(), cancellation); verified.HasError())
            return Capture::Failure(verified.ErrorValue());
        auto state = std::make_shared<TerrainRenderExtractionSnapshot::State>();
        state->header = r.header;
        state->tiles.reserve(r.tiles.size());
        for (const auto &selection : r.tiles) {
            if (cancellation.IsCancellationRequested())
                return Capture::Failure(MakeError(TerrainErrors::CompositionCancelled));
            if (std::ranges::any_of(state->tiles, [&](const auto &tile) {
                return tile.tile == selection.tile;
            }))
                return Capture::Failure(MakeError(TerrainErrors::IdentityConflict));
            auto tile = CaptureTile(r, selection, budget, state->tiles.size(), cancellation);
            if (tile.HasError())
                return Capture::Failure(tile.ErrorValue());
            state->tiles.push_back(std::move(tile).Value());
        }
        std::ranges::sort(state->tiles, [](const auto &a, const auto &b) {
            return std::tie(a.tile.tile.lod, a.tile.tile.z, a.tile.tile.x) < std::tie(b.tile.tile.lod, b.tile.tile.z, b.tile.tile.x);
        });
        if (cancellation.IsCancellationRequested())
            return Capture::Failure(MakeError(TerrainErrors::CompositionCancelled));
        state->bytes = budget.bytes;
        return Capture::Success(TerrainRenderExtractionSnapshot{std::move(state)});
    }

    /** @copydoc TerrainRenderExtractionSnapshot::IsValid */
    bool TerrainRenderExtractionSnapshot::IsValid() const noexcept {
        return static_cast<bool>(state_);
    }

    /** @copydoc TerrainRenderExtractionSnapshot::Header */
    const TerrainRenderSnapshotHeader &TerrainRenderExtractionSnapshot::Header() const noexcept {
        static const TerrainRenderSnapshotHeader empty{};
        return state_ ? state_->header : empty;
    }

    /** @copydoc TerrainRenderExtractionSnapshot::Tiles */
    std::span<const TerrainTileRenderCandidate> TerrainRenderExtractionSnapshot::Tiles() const noexcept {
        return state_ ? std::span<const TerrainTileRenderCandidate>{state_->tiles} : std::span<const TerrainTileRenderCandidate>{};
    }

    /** @copydoc TerrainRenderExtractionSnapshot::OwnedBytes */
    std::uint64_t TerrainRenderExtractionSnapshot::OwnedBytes() const noexcept {
        return state_ ? state_->bytes : 0;
    }

    /** @copydoc TerrainRenderExtractionSnapshot::ValidateCurrent */
    Result<void> TerrainRenderExtractionSnapshot::ValidateCurrent(const TerrainRenderSnapshotHeader &current) const {
        return state_ && state_->header == current ? Result<void>::Success()
                                                   : Result<void>::Failure(MakeError(Terrain::TerrainErrors::RevisionStale));
    }
}  // namespace Horo::TerrainRender
