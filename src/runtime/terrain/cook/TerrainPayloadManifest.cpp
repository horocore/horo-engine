#include "Horo/Terrain/TerrainPayloadManifest.h"

#include "TerrainPayloadManifestInternal.h"
#include "TerrainTileCookCodec.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <tuple>
#include <utility>

namespace Horo::Terrain {
    namespace {
        using Detail::CanonicalWriter;
        using Detail::WriteBounds;
        using Detail::WriteCluster;
        using Detail::WriteRequirements;
        using Detail::WriteTile;

        /** @brief Returns portable canonical tile order rather than structure member order. */
        auto TileOrder(const TerrainTileId &id) {
            return std::tuple{id.tile.lod, id.tile.z, id.tile.x};
        }

        /** @brief Checks the closed requirement vocabulary without inventing fallback. */
        bool ValidRequirements(const TerrainPayloadRequirements &requirements) {
            using enum TerrainPayloadRequirement;
            return requirements.visual < Count && requirements.collision < Count && requirements.navigation < Count;
        }

        /** @brief Enforces fixed safety ceilings before inspecting or allocating bulk data. */
        bool ValidLimits(const TerrainPayloadManifestLimits &limits) {
            const std::array<std::pair<std::uint64_t, std::uint64_t>, 6> ceilings{
                {{limits.maximumTiles, TerrainDescriptorHardLimits::ActiveTerrainTiles},
                 {limits.maximumClusters, TerrainDescriptorHardLimits::ActiveFoliageClusters},
                 {limits.maximumDependencies, 4'096},
                 {limits.maximumInputBytes, TerrainDescriptorHardLimits::StagingBytes},
                 {limits.maximumOwnedBytes, 16ULL * 1024 * 1024},
                 {limits.maximumWorkItems, TerrainDescriptorHardLimits::WorkItems}}};
            return std::ranges::all_of(ceilings, [](const auto &ceiling) {
                return ceiling.first > 0 && ceiling.first <= ceiling.second;
            });
        }

        /** @brief Encodes the exact dependency closure with stable AssetId order. */
        void WriteDependencies(CanonicalWriter &writer, const std::span<const TerrainTileCookDependency> dependencies) {
            writer.Unsigned(dependencies.size(), 4);
            for (const auto &dependency : dependencies) {
                writer.Bytes(dependency.asset.Bytes());
                writer.Bytes(dependency.artifactDigest.bytes);
            }
        }

        /** @brief Binds closure independently of input order and allocation addresses. */
        Sha256Digest DependencyDigest(const std::span<const TerrainTileCookDependency> dependencies) {
            Sha256Builder hash;
            CanonicalWriter writer{nullptr, &hash};
            writer.Text("horo.terrain.payload-dependencies.v1");
            WriteDependencies(writer, dependencies);
            writer.Flush();
            return hash.Finalize();
        }

        /** @brief Normalizes a bounded exact artifact closure; duplicate identity is always ambiguous. */
        bool CopyDependencies(const std::span<const TerrainTileCookDependency> input, std::vector<TerrainTileCookDependency> &output) {
            output.assign(input.begin(), input.end());
            std::ranges::sort(output, {}, &TerrainTileCookDependency::asset);
            for (std::size_t index = 0; index < output.size(); ++index) {
                if (!output[index].asset.IsValid() || !Detail::Nonzero(output[index].artifactDigest) ||
                    (index != 0 && output[index - 1].asset == output[index].asset))
                    return false;
            }
            return true;
        }

        /** @brief Conservative union including flat geometry and finite world coordinates. */
        void Extend(TerrainPayloadBounds &bounds, const TerrainPayloadBounds &other) {
            for (std::size_t axis = 0; axis < 3; ++axis) {
                bounds.minimum[axis] = std::min(bounds.minimum[axis], other.minimum[axis]);
                bounds.maximum[axis] = std::max(bounds.maximum[axis], other.maximum[axis]);
            }
        }

        /** @brief Derives finite geometry bounds including the conservative unsampled-height error. */
        std::optional<TerrainPayloadBounds> GeometryBounds(const TerrainSourceArtifact &artifact, const CancellationToken &cancellation) {
            if (artifact.vertices.empty() || !std::isfinite(artifact.maximumGeometricError) || artifact.maximumGeometricError < 0)
                return std::nullopt;
            const auto &first = artifact.vertices.front();
            TerrainPayloadBounds bounds{{first.x, first.y, first.z}, {first.x, first.y, first.z}};
            for (const auto &vertex : artifact.vertices) {
                if (cancellation.IsCancellationRequested())
                    return std::nullopt;
                const std::array values{vertex.x, vertex.y, vertex.z};
                for (std::size_t axis = 0; axis < 3; ++axis) {
                    if (!std::isfinite(values[axis]))
                        return std::nullopt;
                    bounds.minimum[axis] = std::min(bounds.minimum[axis], values[axis]);
                    bounds.maximum[axis] = std::max(bounds.maximum[axis], values[axis]);
                }
            }
            bounds.minimum[1] -= artifact.maximumGeometricError;
            bounds.maximum[1] += artifact.maximumGeometricError;
            if (!std::isfinite(bounds.minimum[1]) || !std::isfinite(bounds.maximum[1]))
                return std::nullopt;
            return bounds;
        }

        /** @brief Preserves exact integer cluster bounds; outward conversion bounds floating rounding in the dataset union. */
        TerrainPayloadBounds ClusterEnvelope(const FoliageClusterBounds &bounds) {
            TerrainPayloadBounds result;
            for (std::size_t axis = 0; axis < 3; ++axis) {
                result.minimum[axis] =
                    std::nextafter(static_cast<double>(bounds.minimum[axis]) / 1'000.0, -std::numeric_limits<double>::infinity());
                result.maximum[axis] =
                    std::nextafter(static_cast<double>(bounds.maximum[axis]) / 1'000.0, std::numeric_limits<double>::infinity());
            }
            return result;
        }

    }  // namespace

    /** @brief Invocation-owned manifest assembly; inputs are borrowed synchronously and never retained. */
    struct TerrainPayloadManifestBuilder final {
        const TerrainPayloadManifestRequest &request;
        const CancellationToken &cancellation;
        TerrainPayloadManifest root;
        std::uint64_t inputBytes{};
        std::uint64_t work{};

        /** @brief Construct the cook-issued root in an explicit friend context, not aggregate initialization. */
        TerrainPayloadManifestBuilder(const TerrainPayloadManifestRequest &input, const CancellationToken &token)
            : request(input), cancellation(token), root() {}

        /** @brief Returns typed cancellation first when cooperative cancellation won. */
        Result<void> Fail(const ErrorCodeDescriptor &error = TerrainPayloadManifestErrors::Invalid) const {
            return Result<void>::Failure(
                MakeError(cancellation.IsCancellationRequested() ? TerrainPayloadManifestErrors::Cancelled : error));
        }

        /** @brief Charges input hash chunks and semantic element visits before their verifiers run. */
        bool Charge(const std::uint64_t bytes, const std::uint64_t elements) {
            if (bytes > request.limits.maximumInputBytes - inputBytes || elements > request.limits.maximumWorkItems - work)
                return false;
            inputBytes += bytes;
            work += elements;
            const auto chunks = bytes / 4'096 + (bytes % 4'096 != 0 ? 1 : 0);
            if (chunks > request.limits.maximumWorkItems - work)
                return false;
            work += chunks;
            return !cancellation.IsCancellationRequested();
        }

        /** @brief Checks request vocabulary before dereferencing any borrowed root. */
        Result<void> CheckRequest() const {
            if (cancellation.IsCancellationRequested())
                return Fail();
            if (!request.terrain || !request.content.IsValid() || !ValidRequirements(request.terrainRequirements) ||
                !ValidRequirements(request.foliageRequirements) || !ValidLimits(request.limits))
                return Fail();
            return Result<void>::Success();
        }

        /** @brief Bounds all input collection sizes before arithmetic or allocation. */
        Result<void> CheckCounts() const {
            const auto &tiles = request.terrain->Tiles();
            if (const auto clusters = request.foliage ? request.foliage->Clusters().size() : 0;
                tiles.tiles.empty() || tiles.tiles.size() > request.limits.maximumTiles || clusters > request.limits.maximumClusters ||
                request.terrainDependencies.size() > request.limits.maximumDependencies ||
                request.foliageDependencies.size() > request.limits.maximumDependencies ||
                request.terrain->Artifacts().size() > tiles.tiles.size() * 3 || tiles.coordinates.projectedCrs.size() > 65'535)
                return Fail(TerrainPayloadManifestErrors::LimitExceeded);
            return Result<void>::Success();
        }

        /** @brief Admits simultaneous typed/encoded storage and aggregate bounded work. */
        Result<void> Admit() {
            if (const auto checked = CheckRequest(); checked.HasError())
                return checked;
            if (const auto checked = CheckCounts(); checked.HasError())
                return checked;
            const auto &tiles = request.terrain->Tiles();
            const auto clusters = request.foliage ? request.foliage->Clusters().size() : 0;
            const auto dependencyCount = request.terrainDependencies.size() + request.foliageDependencies.size();
            // Fixed v1 record upper bounds cover both typed entries and the simultaneous encoding.
            const auto owned = sizeof(TerrainPayloadManifest) + 1'024 + tiles.coordinates.projectedCrs.size() * 2 +
                               tiles.tiles.size() * (sizeof(TerrainTilePayloadEntry) + 1'024) +
                               clusters * (sizeof(FoliageClusterPayloadEntry) + 512) +
                               dependencyCount * (sizeof(TerrainTileCookDependency) + 48);
            if (owned > request.limits.maximumOwnedBytes)
                return Fail(TerrainPayloadManifestErrors::LimitExceeded);
            if (!Charge(request.terrain->Manifest().size(), 0) || !Charge(0, (owned + 4'095) / 4'096))
                return Fail(TerrainPayloadManifestErrors::LimitExceeded);
            // Includes closure lookup, repeated consumer-coverage scans and bounded binary membership searches.
            if (!Charge(0, clusters * request.foliageDependencies.size() + tiles.tiles.size() * 64 + dependencyCount * 16))
                return Fail(TerrainPayloadManifestErrors::LimitExceeded);
            return ChargeInputs();
        }

        /** @brief Charges all admitted bulk verifiers before their work starts. */
        Result<void> ChargeInputs() {
            const auto &tiles = request.terrain->Tiles();
            if (request.previous && !Charge(request.previous->Bytes().size(), request.previous->Tiles().size()))
                return Fail(TerrainPayloadManifestErrors::LimitExceeded);
            for (const auto &tile : tiles.tiles)
                if (!Charge(tile.payload.size(),
                            static_cast<std::uint64_t>(tile.samplesX) * tile.samplesZ + 4ULL * (tile.samplesX + tile.samplesZ)))
                    return Fail(TerrainPayloadManifestErrors::LimitExceeded);
            for (const auto &artifact : request.terrain->Artifacts())
                if (!Charge(artifact.payload.size(), 2 * artifact.vertices.size() + artifact.triangles.size()))
                    return Fail(TerrainPayloadManifestErrors::LimitExceeded);
            if (request.foliage)
                for (const auto &cluster : request.foliage->Clusters())
                    if (!Charge(cluster.payload.size(), 4 * cluster.instances.size() + 2 * (cluster.payload.size() / 4'096 + 1)))
                        return Fail(TerrainPayloadManifestErrors::LimitExceeded);
            return Result<void>::Success();
        }

        /** @brief Copies exact provenance and proves that dependencies reproduce the selected Terrain cook. */
        Result<void> Capture() {
            const auto &source = *request.terrain;
            const auto &tiles = source.Tiles();
            if (!source.Capability().IsValid() || !CopyDependencies(request.terrainDependencies, root.terrainDependencies_) ||
                !CopyDependencies(request.foliageDependencies, root.foliageDependencies_))
                return Fail();
            TerrainCanonicalSource identity;
            identity.dataset = tiles.dataset;
            identity.sourceAsset = tiles.sourceAsset;
            identity.revision = tiles.sourceRevision;
            if (Detail::Fingerprint(identity, tiles.sourceDigest, tiles.profile, root.terrainDependencies_) != tiles.fingerprint)
                return Fail();
            if (const auto verified = VerifyCookedTerrainTiles(tiles, cancellation); verified.HasError())
                return Result<void>::Failure(verified.ErrorValue());
            const auto manifestDigest = Detail::HashPayload(source.Manifest(), cancellation);
            if (manifestDigest.HasError())
                return Fail();
            if (manifestDigest.Value() != source.ManifestDigest())
                return Fail();
            root.provenance_ = {tiles.dataset,
                                request.content,
                                tiles.sourceAsset,
                                tiles.sourceRevision,
                                source.Capability(),
                                tiles.profile.tier,
                                tiles.sourceDigest,
                                tiles.profile.targetDigest,
                                tiles.profile.toolchainDigest,
                                tiles.fingerprint,
                                tiles.manifestDigest,
                                source.Fingerprint(),
                                source.ManifestDigest(),
                                {},
                                {},
                                DependencyDigest(root.terrainDependencies_),
                                DependencyDigest(root.foliageDependencies_)};
            root.coordinates_ = tiles.coordinates;
            root.terrainRequirements_ = request.terrainRequirements;
            root.foliageRequirements_ = request.foliageRequirements;
            return CheckPrevious();
        }

        /** @brief Preserves exact predecessor provenance without mutating the retained root. */
        Result<void> CheckPrevious() const {
            if (request.previous) {
                if (const auto checked = VerifyTerrainPayloadManifest(*request.previous, request.previous->Bytes(), cancellation);
                    checked.HasError())
                    return checked;
                if (!Detail::PayloadManifestSuccessor(request.previous->Provenance(), root.provenance_))
                    return Fail(TerrainPayloadManifestErrors::Stale);
            }
            return Result<void>::Success();
        }

        /** @brief Finds exact selected tile membership in canonical order, with no coordinate-only aliasing. */
        const TerrainTilePayloadEntry *Find(const TerrainTileId &tile) const {
            if (tile.dataset != root.provenance_.dataset)
                return nullptr;
            const auto found = std::ranges::lower_bound(root.tiles_, TileOrder(tile), {}, [](const auto &entry) {
                return TileOrder(entry.tile);
            });
            return found != root.tiles_.end() && found->tile == tile ? std::to_address(found) : nullptr;
        }

        /** @brief Copies complete verified sample membership without retaining source bytes. */
        Result<void> CopyTiles() {
            const auto &tiles = request.terrain->Tiles();
            root.tiles_.reserve(tiles.tiles.size());
            for (const auto &tile : tiles.tiles) {
                if (cancellation.IsCancellationRequested())
                    return Fail();
                TerrainTilePayloadEntry entry;
                entry.tile = tile.id;
                entry.samplesX = tile.samplesX;
                entry.samplesZ = tile.samplesZ;
                entry.seams = tile.seams;
                const auto samples = static_cast<std::uint64_t>(tile.samplesX) * tile.samplesZ;
                entry.samples = {CurrentTerrainTileCookSchema, tile.digest, tile.payload.size(),
                                 samples * (sizeof(float) + tiles.layerCount * sizeof(std::uint16_t) + sizeof(std::uint8_t)), samples};
                root.tiles_.push_back(entry);
            }
            return Result<void>::Success();
        }

        /** @brief Verifies and attaches one exact tile/role artifact to selected membership. */
        Result<void> AddGeometry(const TerrainSourceArtifact &artifact) {
            const auto *found = Find(artifact.tile);
            auto *entry = found ? root.tiles_.data() + (found - root.tiles_.data()) : nullptr;
            const auto role = static_cast<std::size_t>(artifact.role);
            if (!entry || role >= entry->consumers.size() || entry->consumers[role] || artifact.seams != entry->seams ||
                !artifact.requiresSameLodNeighbors)
                return Fail();
            if (const auto verified = VerifyTerrainSourceArtifactPayload(artifact.payload, artifact.digest, cancellation);
                verified.HasError())
                return Result<void>::Failure(verified.ErrorValue());
            const auto bounds = GeometryBounds(artifact, cancellation);
            if (!bounds)
                return Fail();
            if (role == 0) {
                entry->bounds = *bounds;
                entry->maximumGeometricError = artifact.maximumGeometricError;
            }
            entry->consumers[role] = {CurrentTerrainSourceArtifactSchema, artifact.digest, artifact.payload.size(),
                                      artifact.vertices.size() * sizeof(TerrainSourceVertex) +
                                          artifact.triangles.size() * sizeof(TerrainSourceTriangle),
                                      artifact.vertices.size() + artifact.triangles.size()};
            return Result<void>::Success();
        }

        /** @brief Associates every role artifact and derives the complete terrain envelope. */
        Result<void> BuildTiles() {
            if (const auto copied = CopyTiles(); copied.HasError())
                return copied;
            for (const auto &artifact : request.terrain->Artifacts()) {
                if (cancellation.IsCancellationRequested())
                    return Fail();
                if (const auto added = AddGeometry(artifact); added.HasError())
                    return added;
            }
            for (const auto &entry : root.tiles_)
                if (!entry.consumers[0])
                    return Fail();
            root.bounds_ = root.tiles_.front().bounds;
            for (const auto &entry : root.tiles_)
                Extend(root.bounds_, entry.bounds);
            return CheckSeams();
        }

        /** @brief Validates same-LOD neighbours and complete collision/navigation coverage at one common declared LOD. */
        Result<void> CheckSeams() {
            constexpr std::array<std::size_t, 2> facing{1, 3};
            constexpr std::array<std::size_t, 2> opposite{0, 2};
            for (const auto &entry : root.tiles_) {
                if (cancellation.IsCancellationRequested())
                    return Fail();
                for (std::size_t axis = 0; axis < 2; ++axis) {
                    auto neighbour = entry.tile;
                    auto &coordinate = axis == 0 ? neighbour.tile.x : neighbour.tile.z;
                    if (coordinate == std::numeric_limits<std::int32_t>::max())
                        continue;
                    ++coordinate;
                    if (const auto *adjacent = Find(neighbour); adjacent && entry.seams[facing[axis]] != adjacent->seams[opposite[axis]])
                        return Fail();
                }
            }
            return CheckCoverage();
        }

        /** @brief Tests one role/LOD coverage without silently dropping missing tiles. */
        bool CompleteCoverage(const std::size_t role, const std::uint8_t lod) const {
            bool present = false;
            bool missing = false;
            for (const auto &entry : root.tiles_)
                if (entry.tile.tile.lod == lod) {
                    present = true;
                    missing = missing || !entry.consumers[role];
                }
            return present && !missing;
        }

        /** @brief Requires complete requested role membership at one common LOD. */
        Result<void> CheckCoverage() const {
            const auto &profile = request.terrain->Tiles().profile;
            const std::array requirements{request.terrainRequirements.visual, request.terrainRequirements.collision,
                                          request.terrainRequirements.navigation};
            for (std::size_t role = 0; role < requirements.size(); ++role) {
                if (requirements[role] == TerrainPayloadRequirement::NotRequested)
                    continue;
                bool complete = false;
                for (std::uint8_t lod = 0; lod < profile.lodLevels; ++lod) {
                    if (cancellation.IsCancellationRequested())
                        return Fail();
                    complete = complete || CompleteCoverage(role, lod);
                }
                if (!complete)
                    return Fail();
            }
            return Result<void>::Success();
        }

        /** @brief Matches foliage target and capability policy without fallback. */
        bool CompatibleFoliageProfile() const {
            const auto &profile = request.foliage->Profile();
            const auto &provenance = root.provenance_;
            return profile.targetDigest == provenance.targetDigest && profile.toolchainDigest == provenance.toolchainDigest &&
                   profile.configuration.Data().tier == provenance.tier && profile.configuration.Data().capability == provenance.capability;
        }

        /** @brief Matches externally authenticated closure and complete foliage root identity. */
        bool CompatibleFoliageMembership() const {
            const auto &foliage = *request.foliage;
            const auto &provenance = root.provenance_;
            return Detail::Nonzero(request.verifiedFoliageDependencyClosure) &&
                   request.verifiedFoliageDependencyClosure == provenance.foliageDependencies && foliage.Dataset() == provenance.dataset &&
                   foliage.ContentRevision() == provenance.content &&
                   foliage.Clusters().size() == foliage.Footprint().activeFoliageClusters && Detail::Nonzero(foliage.Fingerprint()) &&
                   Detail::Nonzero(foliage.ManifestDigest());
        }

        /** @brief Admits the independently verified external closure and exact root provenance. */
        Result<void> CheckFoliageRoot() {
            if (!request.foliage) {
                if (!request.foliageDependencies.empty() || Detail::Nonzero(request.verifiedFoliageDependencyClosure) ||
                    request.foliageRequirements != TerrainPayloadRequirements{})
                    return Fail();
                return Result<void>::Success();
            }
            const auto &foliage = *request.foliage;
            if (!CompatibleFoliageProfile() || !CompatibleFoliageMembership())
                return Fail();
            if (const auto complete = foliage.Validate(cancellation); complete.HasError())
                return Result<void>::Failure(complete.ErrorValue());
            root.provenance_.foliageFingerprint = foliage.Fingerprint();
            root.provenance_.foliageManifestDigest = foliage.ManifestDigest();
            return Result<void>::Success();
        }

        /** @brief Matches one cluster to selected source/capability and authenticated geometry closure. */
        bool CompatibleCluster(const CookedFoliageCluster &cluster) const {
            const auto &provenance = root.provenance_;
            return Find(cluster.tile) && cluster.sourceRevision == provenance.source && cluster.capability == provenance.capability &&
                   std::ranges::any_of(root.foliageDependencies_, [&cluster](const auto &dependency) {
                return dependency.artifactDigest == cluster.geometryDigest;
            });
        }

        /** @brief Retains every selected cluster's exact placement and geometry provenance. */
        Result<void> BuildClusters() {
            if (const auto checked = CheckFoliageRoot(); checked.HasError() || !request.foliage)
                return checked;
            const auto &foliage = *request.foliage;
            root.clusters_.reserve(foliage.Clusters().size());
            std::uint64_t instances{};
            for (const auto &cluster : foliage.Clusters()) {
                if (cancellation.IsCancellationRequested())
                    return Fail();
                if (!CompatibleCluster(cluster))
                    return Fail();
                if (const auto verified = VerifyFoliageClusterPayload(cluster, cluster.payload, cancellation); verified.HasError())
                    return Result<void>::Failure(verified.ErrorValue());
                FoliageClusterPayloadEntry entry{cluster.tile,
                                                 cluster.type,
                                                 cluster.id,
                                                 cluster.sourceRevision,
                                                 cluster.definitionRevision,
                                                 cluster.placementRevision,
                                                 cluster.placementFingerprint,
                                                 cluster.placementDigest,
                                                 cluster.geometryDigest,
                                                 cluster.profileFingerprint,
                                                 cluster.bounds,
                                                 cluster.geometryRadiusMillimeters,
                                                 cluster.instances.size(),
                                                 {1, cluster.digest, cluster.payload.size(),
                                                  cluster.instances.size() * sizeof(CookedFoliageInstance), cluster.instances.size()}};
                root.clusters_.push_back(entry);
                instances += cluster.instances.size();
                Extend(root.bounds_, ClusterEnvelope(cluster.bounds));
            }
            if (instances != foliage.Footprint().activeFoliageInstances)
                return Fail();
            return Result<void>::Success();
        }

        /** @brief Serializes a complete bounded root; input/worker ordering cannot affect the output. */
        Result<void> Encode() {
            const auto &p = root.provenance_;
            const auto reserve = 1'024 + root.coordinates_.projectedCrs.size() + root.tiles_.size() * 1'024 + root.clusters_.size() * 512 +
                                 (root.terrainDependencies_.size() + root.foliageDependencies_.size()) * 48;
            root.bytes_.reserve(reserve);
            if (root.OwnedBytes() > request.limits.maximumOwnedBytes)
                return Fail(TerrainPayloadManifestErrors::LimitExceeded);
            CanonicalWriter writer{&root.bytes_};
            writer.Text("HTPM");
            writer.Unsigned(CurrentTerrainPayloadManifestSchema, 4);
            writer.Bytes(p.dataset.Bytes());
            writer.Unsigned(p.content.Value(), 8);
            writer.Bytes(p.sourceAsset.Bytes());
            writer.Unsigned(p.source.Value(), 8);
            writer.Unsigned(p.capability.Value(), 8);
            writer.Byte(static_cast<std::uint8_t>(p.tier));
            for (const auto &digest :
                 {p.sourceDigest, p.targetDigest, p.toolchainDigest, p.tileFingerprint, p.tileManifestDigest, p.geometryFingerprint,
                  p.geometryManifestDigest, p.foliageFingerprint, p.foliageManifestDigest, p.terrainDependencies, p.foliageDependencies})
                writer.Bytes(digest.bytes);
            Detail::WriteCoordinates(writer, root.coordinates_);
            WriteBounds(writer, root.bounds_);
            WriteRequirements(writer, root.terrainRequirements_);
            WriteRequirements(writer, root.foliageRequirements_);
            WriteDependencies(writer, root.terrainDependencies_);
            WriteDependencies(writer, root.foliageDependencies_);
            writer.Unsigned(root.tiles_.size(), 4);
            for (const auto &entry : root.tiles_) {
                if (cancellation.IsCancellationRequested())
                    return Fail();
                WriteTile(writer, entry);
            }
            writer.Unsigned(root.clusters_.size(), 4);
            for (const auto &entry : root.clusters_) {
                if (cancellation.IsCancellationRequested())
                    return Fail();
                WriteCluster(writer, entry);
            }
            if (root.OwnedBytes() > request.limits.maximumOwnedBytes)
                return Fail(TerrainPayloadManifestErrors::LimitExceeded);
            const auto digest = Detail::HashPayload(root.bytes_, cancellation);
            if (digest.HasError())
                return Fail();
            root.digest_ = digest.Value();
            return cancellation.IsCancellationRequested() ? Fail() : Result<void>::Success();
        }
    };

    /** @copydoc TerrainPayloadManifest::OwnedBytes */
    std::uint64_t TerrainPayloadManifest::OwnedBytes() const noexcept {
        return sizeof(*this) + coordinates_.projectedCrs.capacity() + tiles_.capacity() * sizeof(TerrainTilePayloadEntry) +
               clusters_.capacity() * sizeof(FoliageClusterPayloadEntry) +
               (terrainDependencies_.capacity() + foliageDependencies_.capacity()) * sizeof(TerrainTileCookDependency) + bytes_.capacity();
    }

    /** @copydoc GenerateTerrainPayloadManifest */
    Result<TerrainPayloadManifest> GenerateTerrainPayloadManifest(const TerrainPayloadManifestRequest &request,
                                                                  const CancellationToken &cancellation) {
        TerrainPayloadManifestBuilder builder{request, cancellation};
        for (const auto step :
             {&TerrainPayloadManifestBuilder::Admit, &TerrainPayloadManifestBuilder::Capture, &TerrainPayloadManifestBuilder::BuildTiles,
              &TerrainPayloadManifestBuilder::BuildClusters, &TerrainPayloadManifestBuilder::Encode}) {
            const auto result = (builder.*step)();
            if (result.HasError())
                return Result<TerrainPayloadManifest>::Failure(
                    cancellation.IsCancellationRequested() ? MakeError(TerrainPayloadManifestErrors::Cancelled) : result.ErrorValue());
        }
        return Result<TerrainPayloadManifest>::Success(std::move(builder.root));
    }

    /** @copydoc TerrainPayloadDependencyClosureDigest */
    Result<Sha256Digest> TerrainPayloadDependencyClosureDigest(const std::span<const TerrainTileCookDependency> dependencies) {
        if (dependencies.size() > 4'096)
            return Result<Sha256Digest>::Failure(MakeError(TerrainPayloadManifestErrors::LimitExceeded));
        std::vector<TerrainTileCookDependency> sorted;
        if (!CopyDependencies(dependencies, sorted))
            return Result<Sha256Digest>::Failure(MakeError(TerrainPayloadManifestErrors::Invalid));
        return Result<Sha256Digest>::Success(DependencyDigest(sorted));
    }
}  // namespace Horo::Terrain
