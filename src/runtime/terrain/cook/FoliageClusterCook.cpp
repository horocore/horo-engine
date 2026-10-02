#include "Horo/Terrain/FoliageClusterCook.h"

#include "FoliageClusterCookInternal.h"

#include <algorithm>
#include <limits>
#include <string_view>
#include <tuple>
#include <utility>

namespace Horo::Terrain {

    namespace {
        constexpr std::uint64_t InstanceBytes = 128;
        constexpr std::uint64_t HeaderBytes = 289;
        constexpr std::uint32_t MaximumGeometryRadius = 1'000'000'000;

        /** @brief Emits fixed-width network-order schema bytes while computing their digest, without struct padding. */
        class Writer final {
        public:
            explicit Writer(std::vector<std::uint8_t> *bytes = nullptr) : bytes_(bytes) {}

            void Bytes(const std::span<const std::uint8_t> bytes) {
                static_cast<void>(hash_.Update(std::as_bytes(bytes)));
                if (bytes_)
                    bytes_->insert(bytes_->end(), bytes.begin(), bytes.end());
            }

            void Integer(const std::uint64_t value) {
                std::array<std::uint8_t, 8> bytes{};
                for (std::size_t index = 0; index < bytes.size(); ++index)
                    bytes[index] = static_cast<std::uint8_t>(value >> ((7U - index) * 8U));
                Bytes(bytes);
            }

            void Domain(const std::string_view value) {
                static_cast<void>(hash_.Update(std::as_bytes(std::span{value.data(), value.size()})));
            }

            [[nodiscard]] Sha256Digest Finish() noexcept {
                return hash_.Finalize();
            }

        private:
            Sha256Builder hash_{};
            std::vector<std::uint8_t> *bytes_{};
        };

        /** @brief Hashes every byte-affecting profile field, including the exact project-lowered tier table. */
        [[nodiscard]] Sha256Digest ProfileDigest(const FoliageClusterCookProfile &profile) {
            Writer writer;
            writer.Domain("horo.foliage.cluster.profile.v1");
            const auto &data = profile.configuration.Data();
            const auto &limits = data.limits;
            for (const std::uint64_t value :
                 {std::uint64_t{data.contractVersion}, std::uint64_t{data.tierProfileRevision}, data.configuration.Value(),
                  data.capability.Value(), static_cast<std::uint64_t>(data.tier), std::uint64_t{limits.maximumSamplesPerAxis},
                  std::uint64_t{limits.maximumTileInteriorQuads}, std::uint64_t{limits.maximumLodLevels},
                  std::uint64_t{limits.maximumLayersPerTile}, std::uint64_t{limits.maximumActiveTerrainTiles},
                  std::uint64_t{limits.maximumActiveFoliageClusters}, limits.maximumActiveFoliageInstances,
                  limits.maximumResidentTerrainBytes, limits.maximumResidentFoliageBytes, limits.maximumStagingBytes,
                  limits.maximumRetiringBytes, limits.maximumWorkItems})
                writer.Integer(value);
            writer.Bytes(profile.targetDigest.bytes);
            writer.Bytes(profile.toolchainDigest.bytes);
            return writer.Finish();
        }

        /** @brief Writes the complete neutral record; ordinal and surface evidence are provenance, not array identity. */
        void EncodeInstance(Writer &writer, const CookedFoliageInstance &instance) {
            writer.Bytes(instance.id.Bytes());
            writer.Bytes(instance.cluster.Bytes());
            writer.Bytes(instance.type.Bytes());
            writer.Integer(static_cast<std::uint64_t>(instance.xMillimeters));
            writer.Integer(static_cast<std::uint64_t>(instance.altitudeMillimeters));
            writer.Integer(static_cast<std::uint64_t>(instance.zMillimeters));
            writer.Integer(instance.scalePermille);
            writer.Integer(instance.yawMilliDegrees);
            writer.Integer(instance.slopeMilliDegrees);
            writer.Integer(instance.candidateOrdinal);
            writer.Integer(static_cast<std::uint16_t>(instance.normalXPermille));
            writer.Integer(static_cast<std::uint16_t>(instance.normalYPermille));
            writer.Integer(static_cast<std::uint16_t>(instance.normalZPermille));
        }

        /** @brief Writes a schema-v1 self-describing cluster envelope before its bounded records. */
        void EncodeHeader(Writer &writer, const CookedFoliageCluster &cluster) {
            writer.Integer(1);
            writer.Bytes(SerializeTerrainTileId(cluster.tile));
            writer.Bytes(cluster.type.Bytes());
            writer.Bytes(cluster.id.Bytes());
            writer.Integer(cluster.sourceRevision.Value());
            writer.Integer(cluster.definitionRevision.Value());
            writer.Integer(cluster.capability.Value());
            writer.Integer(cluster.placementRevision.Value());
            writer.Bytes(cluster.placementFingerprint.bytes);
            writer.Bytes(cluster.placementDigest.bytes);
            writer.Bytes(cluster.geometryDigest.bytes);
            writer.Integer(cluster.geometryRadiusMillimeters);
            writer.Bytes(cluster.profileFingerprint.bytes);
            for (const auto value : cluster.bounds.minimum)
                writer.Integer(static_cast<std::uint64_t>(value));
            for (const auto value : cluster.bounds.maximum)
                writer.Integer(static_cast<std::uint64_t>(value));
            writer.Integer(cluster.instances.size());
        }

        /** @brief Builds manifest integrity from exact stable membership and independently hashed payloads. */
        [[nodiscard]] Sha256Digest ComputeManifestDigest(const TerrainDatasetId dataset, const TerrainContentRevision content,
                                                         const Sha256Digest &fingerprint,
                                                         const std::span<const CookedFoliageCluster> clusters) {
            Writer writer;
            writer.Domain("horo.foliage.cluster.manifest.v1");
            writer.Bytes(dataset.Bytes());
            writer.Integer(content.Value());
            writer.Bytes(fingerprint.bytes);
            writer.Integer(clusters.size());
            for (const auto &cluster : clusters) {
                writer.Bytes(SerializeTerrainTileId(cluster.tile));
                writer.Bytes(cluster.type.Bytes());
                writer.Bytes(cluster.id.Bytes());
                writer.Integer(cluster.payload.size());
                writer.Integer(cluster.instances.size());
                writer.Bytes(cluster.digest.bytes);
            }
            return writer.Finish();
        }

        /** @brief Tests nonzero opaque artifact identity; zero never means an inferred target. */
        [[nodiscard]] bool HasDigest(const Sha256Digest &digest) noexcept {
            return std::ranges::any_of(digest.bytes, [](const auto byte) {
                return byte != 0;
            });
        }

        /** @brief Computes exact conservative scaled-sphere bounds, failing before signed overflow. */
        [[nodiscard]] bool IncludeBounds(FoliageClusterBounds &bounds, const CookedFoliageInstance &instance, const std::uint32_t radius,
                                         const bool first) noexcept {
            const std::uint64_t extent = (std::uint64_t{radius} * instance.scalePermille + 999U) / 1'000U;
            if (extent > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
                return false;
            const auto delta = static_cast<std::int64_t>(extent);
            const std::array position{instance.xMillimeters, instance.altitudeMillimeters, instance.zMillimeters};
            for (std::size_t axis = 0; axis < position.size(); ++axis) {
                if (position[axis] < std::numeric_limits<std::int64_t>::min() + delta ||
                    position[axis] > std::numeric_limits<std::int64_t>::max() - delta)
                    return false;
                const auto lower = position[axis] - delta;
                const auto upper = position[axis] + delta;
                bounds.minimum[axis] = first ? lower : std::min(bounds.minimum[axis], lower);
                bounds.maximum[axis] = first ? upper : std::max(bounds.maximum[axis], upper);
            }
            return true;
        }

        /** @brief One bounded sort reference; borrowed sources remain alive for this synchronous call. */
        struct Reference final {
            const FoliageClusterCookSource *source{};
            const CookedFoliageInstance *instance{};
        };

        /** @brief Canonical typed address; worker/input order cannot affect cluster membership or record order. */
        [[nodiscard]] auto Address(const Reference &reference) {
            return std::tuple{reference.source->placement->Tile(), reference.source->placement->Type(), reference.instance->cluster,
                              reference.instance->id};
        }

        /** @brief Exact work ledger: one source/record visit, merge output or cluster visit is one work item. */
        struct Work final {
            const CancellationToken &cancellation;
            std::uint64_t limit{};
            std::uint64_t used{};
            const ErrorCodeDescriptor *error{};

            [[nodiscard]] bool Spend(const std::uint64_t count = 1) noexcept {
                if (cancellation.IsCancellationRequested())
                    error = &FoliageClusterCookErrors::Cancelled;
                else if (count > limit - used)
                    error = &FoliageClusterCookErrors::LimitExceeded;
                if (error)
                    return false;
                used += count;
                return true;
            }
        };

        /** @brief Bounded bottom-up merge sort with cooperative cancellation during every merge, not just around sorting. */
        [[nodiscard]] bool Merge(const std::span<const Reference> values, const std::span<Reference> scratch, const std::size_t begin,
                                 const std::size_t width, Work &work) {
            const auto middle = std::min(begin + width, values.size());
            const auto end = std::min(begin + 2 * width, values.size());
            auto left = begin;
            auto right = middle;
            for (auto destination = begin; destination < end; ++destination) {
                if (!work.Spend())
                    return false;
                const bool takeLeft = right == end || (left < middle && Address(values[left]) <= Address(values[right]));
                scratch[destination] = values[takeLeft ? left++ : right++];
            }
            return true;
        }

        /** @brief Merges progressively doubled runs, preserving the same exact work ledger and canonical ordering. */
        [[nodiscard]] bool Sort(std::vector<Reference> &values, Work &work) {
            std::vector<Reference> scratch(values.size());
            std::size_t width = 1;
            while (width < values.size()) {
                for (std::size_t begin = 0; begin < values.size(); begin += 2 * width) {
                    if (!Merge(values, scratch, begin, width, work))
                        return false;
                }
                values.swap(scratch);
                width *= 2;
            }
            return true;
        }

        /** @brief Creates immutable source provenance, without an aggregate revision that would invalidate unrelated clusters. */
        [[nodiscard]] CookedFoliageCluster Cluster(const Reference &reference, const Sha256Digest &profile) {
            const auto &source = *reference.source;
            const auto &placement = *source.placement;
            CookedFoliageCluster cluster;
            cluster.tile = placement.Tile();
            cluster.type = placement.Type();
            cluster.id = reference.instance->cluster;
            cluster.sourceRevision = placement.SourceRevision();
            cluster.definitionRevision = placement.DefinitionRevision();
            cluster.capability = placement.CapabilityRevision();
            cluster.placementRevision = placement.ContentRevision();
            cluster.placementFingerprint = placement.Fingerprint();
            cluster.placementDigest = placement.ResultDigest();
            cluster.geometryDigest = source.geometryDigest;
            cluster.geometryRadiusMillimeters = source.geometryRadiusMillimeters;
            cluster.profileFingerprint = profile;
            return cluster;
        }

        /** @brief Builds one pre-admitted cluster, checking bounds and cancellation before emitting each whole record. */
        [[nodiscard]] Result<CookedFoliageCluster> BuildCluster(const std::span<const Reference> references, const Sha256Digest &profile,
                                                                Work &work) {
            auto cluster = Cluster(references.front(), profile);
            cluster.instances.reserve(references.size());
            for (const auto &reference : references) {
                if (!work.Spend())
                    return Result<CookedFoliageCluster>::Failure(MakeError(*work.error));
                if (!IncludeBounds(cluster.bounds, *reference.instance, cluster.geometryRadiusMillimeters, cluster.instances.empty()))
                    return Result<CookedFoliageCluster>::Failure(MakeError(FoliageClusterCookErrors::LimitExceeded));
                cluster.instances.push_back(*reference.instance);
            }
            cluster.payload.reserve(static_cast<std::size_t>(HeaderBytes + InstanceBytes * references.size()));
            Writer writer{&cluster.payload};
            EncodeHeader(writer, cluster);
            for (const auto &instance : cluster.instances) {
                if (!work.Spend())
                    return Result<CookedFoliageCluster>::Failure(MakeError(*work.error));
                EncodeInstance(writer, instance);
            }
            cluster.digest = writer.Finish();
            return Result<CookedFoliageCluster>::Success(std::move(cluster));
        }

        /** @brief Canonical source order, including empty placements whose provenance still participates in the cook key. */
        [[nodiscard]] auto SourceAddress(const FoliageClusterCookSource *source) {
            return std::tuple{source->placement->Tile(), source->placement->Type()};
        }

        /** @brief Binds all source evidence, geometry and target policy, independent of caller ordering. */
        [[nodiscard]] Sha256Digest Fingerprint(const FoliageClusterCookRequest &request,
                                               const std::span<const FoliageClusterCookSource *const> sources,
                                               const Sha256Digest &profile) {
            Writer writer;
            writer.Domain("horo.foliage.cluster.inputs.v1");
            writer.Bytes(request.dataset.Bytes());
            writer.Integer(request.content.Value());
            writer.Bytes(profile.bytes);
            writer.Integer(sources.size());
            for (const auto *source : sources) {
                const auto &placement = *source->placement;
                writer.Bytes(SerializeTerrainTileId(placement.Tile()));
                writer.Bytes(placement.Type().Bytes());
                writer.Integer(placement.SourceRevision().Value());
                writer.Integer(placement.DefinitionRevision().Value());
                writer.Integer(placement.CapabilityRevision().Value());
                writer.Integer(placement.ContentRevision().Value());
                writer.Bytes(placement.Fingerprint().bytes);
                writer.Bytes(placement.ResultDigest().bytes);
                writer.Bytes(source->geometryDigest.bytes);
                writer.Integer(source->geometryRadiusMillimeters);
            }
            return writer.Finish();
        }

        /** @brief Checks stable source identities independently from geometry and buffer shape. */
        [[nodiscard]] bool ValidClusterIdentity(const CookedFoliageCluster &cluster, const TerrainDatasetId dataset,
                                                const TerrainCapabilityRevision capability) {
            return cluster.tile.IsValid() && cluster.tile.dataset == dataset && cluster.id.IsValid() && cluster.type.IsValid() &&
                   cluster.sourceRevision.IsValid() && cluster.definitionRevision.IsValid() && cluster.placementRevision.IsValid() &&
                   cluster.capability == capability;
        }

        /** @brief Requires verified finite geometry evidence and exact profile identity. */
        [[nodiscard]] bool ValidClusterGeometry(const CookedFoliageCluster &cluster, const Sha256Digest &profile) {
            return cluster.profileFingerprint == profile && cluster.geometryRadiusMillimeters != 0 &&
                   cluster.geometryRadiusMillimeters <= MaximumGeometryRadius && HasDigest(cluster.geometryDigest);
        }

        /** @brief Validates the exact independently addressed cluster envelope before visiting record storage. */
        [[nodiscard]] bool ValidClusterMetadata(const CookedFoliageCluster &cluster, const TerrainDatasetId dataset,
                                                const TerrainCapabilityRevision capability, const Sha256Digest &profile) {
            return ValidClusterIdentity(cluster, dataset, capability) && ValidClusterGeometry(cluster, profile) &&
                   !cluster.instances.empty() && cluster.payload.size() == HeaderBytes + InstanceBytes * cluster.instances.size();
        }

        /** @brief Checks exact typed membership and strictly increasing stable instance IDs. */
        [[nodiscard]] bool ValidInstance(const CookedFoliageCluster &cluster, const std::size_t index) {
            const auto &instance = cluster.instances[index];
            return instance.id.IsValid() && instance.cluster == cluster.id && instance.type == cluster.type &&
                   (index == 0 || cluster.instances[index - 1].id < instance.id);
        }

        /** @brief Verifies canonical typed records and exact geometry bounds without allocating or repairing evidence. */
        [[nodiscard]] bool ValidCluster(const CookedFoliageCluster &cluster, const TerrainDatasetId dataset,
                                        const TerrainCapabilityRevision capability, const Sha256Digest &profile,
                                        const CancellationToken &cancellation) {
            if (!ValidClusterMetadata(cluster, dataset, capability, profile))
                return false;
            FoliageClusterBounds bounds;
            for (std::size_t index = 0; index < cluster.instances.size(); ++index) {
                const auto &instance = cluster.instances[index];
                if (cancellation.IsCancellationRequested() || !ValidInstance(cluster, index) ||
                    !IncludeBounds(bounds, instance, cluster.geometryRadiusMillimeters, index == 0))
                    return false;
            }
            Writer writer;
            EncodeHeader(writer, cluster);
            for (const auto &instance : cluster.instances) {
                if (cancellation.IsCancellationRequested())
                    return false;
                EncodeInstance(writer, instance);
            }
            return bounds == cluster.bounds && writer.Finish() == cluster.digest &&
                   VerifyFoliageClusterPayload(cluster, cluster.payload, cancellation).HasValue();
        }

        /** @brief Rejects absent dataset, revision and target identity without selecting a fallback. */
        [[nodiscard]] bool ValidRequestIdentity(const FoliageClusterCookRequest &request) {
            return request.dataset.IsValid() && request.content.IsValid() && HasDigest(request.profile.targetDigest) &&
                   HasDigest(request.profile.toolchainDigest);
        }

        /** @brief Admits the exact target and predecessor before allocating source or record workspaces. */
        [[nodiscard]] Result<void> ValidateRequest(const FoliageClusterCookRequest &request) {
            const auto &limits = request.profile.configuration.Data().limits;
            if (!ValidRequestIdentity(request))
                return Result<void>::Failure(MakeError(FoliageClusterCookErrors::InvalidInput));
            if (request.previous && (request.previous->Dataset() != request.dataset ||
                                     request.previous->ContentRevision().Value() == std::numeric_limits<std::uint64_t>::max() ||
                                     request.content.Value() != request.previous->ContentRevision().Value() + 1))
                return Result<void>::Failure(MakeError(FoliageClusterCookErrors::Stale));
            if (request.sources.size() > limits.maximumActiveFoliageClusters ||
                request.sources.size() * sizeof(const FoliageClusterCookSource *) > limits.maximumStagingBytes)
                return Result<void>::Failure(MakeError(FoliageClusterCookErrors::LimitExceeded));
            return Result<void>::Success();
        }

        /** @brief Bounded canonical source workspace and pre-admitted total record count. */
        struct SourceCollection final {
            std::vector<const FoliageClusterCookSource *> sources{};
            std::uint64_t instanceCount{};
        };

        /** @brief Validates a source's exact geometry, dataset, capability and target contract without dereferencing null evidence. */
        [[nodiscard]] bool SourceMatches(const FoliageClusterCookSource &source, const FoliageClusterCookRequest &request) {
            const auto &configuration = request.profile.configuration.Data();
            return source.placement && HasDigest(source.geometryDigest) && source.geometryRadiusMillimeters != 0 &&
                   source.geometryRadiusMillimeters <= MaximumGeometryRadius && source.placement->Tile().dataset == request.dataset &&
                   source.placement->CapabilityRevision() == configuration.capability &&
                   source.placement->TargetDigest() == request.profile.targetDigest &&
                   source.placement->ToolchainDigest() == request.profile.toolchainDigest && source.placement->Tier() == configuration.tier;
        }

        /** @brief Validates source membership and finite geometry/counts before any record-sized allocation. */
        [[nodiscard]] Result<SourceCollection> CollectSources(const FoliageClusterCookRequest &request, Work &work) {
            SourceCollection collection;
            collection.sources.reserve(request.sources.size());
            const auto &configuration = request.profile.configuration.Data();
            for (const auto &source : request.sources) {
                if (!work.Spend())
                    return Result<SourceCollection>::Failure(MakeError(*work.error));
                if (!SourceMatches(source, request))
                    return Result<SourceCollection>::Failure(MakeError(FoliageClusterCookErrors::InvalidInput));
                const auto count = source.placement->Instances().size();
                if (count > configuration.limits.maximumActiveFoliageInstances - collection.instanceCount)
                    return Result<SourceCollection>::Failure(MakeError(FoliageClusterCookErrors::LimitExceeded));
                collection.instanceCount += count;
                collection.sources.push_back(&source);
            }
            std::ranges::sort(collection.sources, {}, SourceAddress);
            for (std::size_t index = 1; index < collection.sources.size(); ++index)
                if (SourceAddress(collection.sources[index - 1]) == SourceAddress(collection.sources[index]))
                    return Result<SourceCollection>::Failure(MakeError(FoliageClusterCookErrors::InvalidInput));
            return Result<SourceCollection>::Success(std::move(collection));
        }

        /** @brief Sort workspace and admitted peak footprint for a complete detached generation. */
        struct PreparedReferences final {
            std::vector<Reference> references{};
            TerrainDescriptorFootprint footprint{};
        };

        /** @brief Counts complete clusters and duplicate evidence after sorting, never truncating required records. */
        [[nodiscard]] Result<std::uint32_t> CountClusters(const std::span<const Reference> references, Work &work) {
            std::uint32_t count{};
            for (std::size_t index = 0; index < references.size(); ++index) {
                if (!work.Spend())
                    return Result<std::uint32_t>::Failure(MakeError(*work.error));
                if (index == 0 || references[index].instance->cluster != references[index - 1].instance->cluster)
                    ++count;
                else if (references[index].source != references[index - 1].source ||
                         references[index].instance->id == references[index - 1].instance->id)
                    return Result<std::uint32_t>::Failure(MakeError(FoliageClusterCookErrors::InvalidInput));
            }
            return Result<std::uint32_t>::Success(count);
        }

        /** @brief Admits the complete peak output/scratch footprint and the actual predecessor's retirement cost. */
        [[nodiscard]] bool FitsFootprint(const TerrainDescriptorLimits &limits, const std::uint32_t clusters,
                                         const std::uint64_t residentBytes, const std::uint64_t scratchBytes,
                                         const std::uint64_t retiringBytes) {
            return clusters <= limits.maximumActiveFoliageClusters && residentBytes <= limits.maximumResidentFoliageBytes &&
                   residentBytes <= limits.maximumStagingBytes && scratchBytes <= limits.maximumStagingBytes - residentBytes &&
                   retiringBytes <= limits.maximumRetiringBytes;
        }

        /** @brief Reserves sorting and output accounting before creating instance or encoded output buffers. */
        [[nodiscard]] Result<PreparedReferences> PrepareReferences(const FoliageClusterCookRequest &request,
                                                                   const SourceCollection &sources, Work &work) {
            const auto &limits = request.profile.configuration.Data().limits;
            const auto scratchBytes =
                sources.instanceCount * sizeof(Reference) * 2 + sources.sources.size() * sizeof(const FoliageClusterCookSource *);
            if (scratchBytes > limits.maximumStagingBytes)
                return Result<PreparedReferences>::Failure(MakeError(FoliageClusterCookErrors::LimitExceeded));
            PreparedReferences prepared;
            prepared.references.reserve(static_cast<std::size_t>(sources.instanceCount));
            for (const auto *source : sources.sources)
                for (const auto &instance : source->placement->Instances()) {
                    if (!work.Spend())
                        return Result<PreparedReferences>::Failure(MakeError(*work.error));
                    prepared.references.push_back({source, &instance});
                }
            if (!Sort(prepared.references, work))
                return Result<PreparedReferences>::Failure(MakeError(*work.error));
            const auto clusters = CountClusters(prepared.references, work);
            if (clusters.HasError())
                return Result<PreparedReferences>::Failure(clusters.ErrorValue());
            const auto residentBytes = sizeof(CookedFoliageClusterSet) +
                                       std::uint64_t{clusters.Value()} * (sizeof(CookedFoliageCluster) + HeaderBytes) +
                                       sources.instanceCount * (sizeof(CookedFoliageInstance) + InstanceBytes);
            const auto retiringBytes = request.previous ? request.previous->Footprint().residentFoliageBytes : 0;
            if (!FitsFootprint(limits, clusters.Value(), residentBytes, scratchBytes, retiringBytes))
                return Result<PreparedReferences>::Failure(MakeError(FoliageClusterCookErrors::LimitExceeded));
            prepared.footprint = {.activeFoliageClusters = clusters.Value(),
                                  .activeFoliageInstances = sources.instanceCount,
                                  .residentFoliageBytes = residentBytes,
                                  .stagingBytes = residentBytes + scratchBytes,
                                  .retiringBytes = retiringBytes};
            return Result<PreparedReferences>::Success(std::move(prepared));
        }

        /** @brief Materializes every admitted cluster into detached owned storage; failures discard the whole candidate. */
        [[nodiscard]] Result<std::vector<CookedFoliageCluster>> BuildClusters(const PreparedReferences &prepared,
                                                                              const Sha256Digest &profile, Work &work) {
            std::vector<CookedFoliageCluster> clusters;
            clusters.reserve(prepared.footprint.activeFoliageClusters);
            const auto &references = prepared.references;
            for (std::size_t begin = 0; begin < references.size();) {
                auto end = begin + 1;
                while (end < references.size() && references[end].instance->cluster == references[begin].instance->cluster) {
                    if (!work.Spend())
                        return Result<std::vector<CookedFoliageCluster>>::Failure(MakeError(*work.error));
                    ++end;
                }
                if (!work.Spend())
                    return Result<std::vector<CookedFoliageCluster>>::Failure(MakeError(*work.error));
                auto cluster = BuildCluster(std::span{references}.subspan(begin, end - begin), profile, work);
                if (cluster.HasError())
                    return Result<std::vector<CookedFoliageCluster>>::Failure(cluster.ErrorValue());
                clusters.push_back(std::move(cluster).Value());
                begin = end;
            }
            return Result<std::vector<CookedFoliageCluster>>::Success(std::move(clusters));
        }
    }  // namespace

    /** @brief Target-private synchronous worker; borrowed sources and cancellation outlive this bounded call. */
    struct FoliageClusterCookWorker final {
        const FoliageClusterCookRequest &request;
        const CancellationToken &cancellation;
        Work work;

        FoliageClusterCookWorker(const FoliageClusterCookRequest &input, const CancellationToken &observer)
            : request(input), cancellation(observer), work{observer, input.profile.configuration.Data().limits.maximumWorkItems} {}

        /** @brief Charges and revalidates the exact predecessor before admitting new storage. */
        [[nodiscard]] Result<void> ValidatePrevious() {
            if (!request.previous)
                return Result<void>::Success();
            if (!work.Spend(request.previous->Footprint().activeFoliageInstances * 3 + request.previous->Clusters().size()))
                return Result<void>::Failure(MakeError(*work.error));
            if (!request.previous->IsWellFormed(cancellation))
                return Result<void>::Failure(MakeError(cancellation.IsCancellationRequested() ? FoliageClusterCookErrors::Cancelled
                                                                                              : FoliageClusterCookErrors::InvalidInput));
            return Result<void>::Success();
        }

        /** @brief Rechecks every cook-issued placement digest with cooperative cancellation and exact visit accounting. */
        [[nodiscard]] Result<void> ValidatePlacements(const SourceCollection &sources) {
            for (const auto *source : sources.sources) {
                if (!work.Spend(source->placement->Instances().size()))
                    return Result<void>::Failure(MakeError(*work.error));
                if (!source->placement->IsWellFormed(cancellation))
                    return Result<void>::Failure(MakeError(cancellation.IsCancellationRequested()
                                                               ? FoliageClusterCookErrors::Cancelled
                                                               : FoliageClusterCookErrors::InvalidInput));
            }
            return Result<void>::Success();
        }

        /** @brief Prepares one complete detached generation; each validation failure discards all candidate storage. */
        [[nodiscard]] Result<CookedFoliageClusterSet> Run() {
            if (!work.Spend())
                return Result<CookedFoliageClusterSet>::Failure(MakeError(*work.error));
            if (const auto valid = ValidateRequest(request); valid.HasError())
                return Result<CookedFoliageClusterSet>::Failure(valid.ErrorValue());
            if (const auto valid = ValidatePrevious(); valid.HasError())
                return Result<CookedFoliageClusterSet>::Failure(valid.ErrorValue());
            auto sources = CollectSources(request, work);
            if (sources.HasError())
                return Result<CookedFoliageClusterSet>::Failure(sources.ErrorValue());
            if (const auto valid = ValidatePlacements(sources.Value()); valid.HasError())
                return Result<CookedFoliageClusterSet>::Failure(valid.ErrorValue());
            auto prepared = PrepareReferences(request, sources.Value(), work);
            if (prepared.HasError())
                return Result<CookedFoliageClusterSet>::Failure(prepared.ErrorValue());
            CookedFoliageClusterSet output{request.profile};
            output.dataset_ = request.dataset;
            output.content_ = request.content;
            const auto profile = ProfileDigest(request.profile);
            output.fingerprint_ = Fingerprint(request, sources.Value().sources, profile);
            auto clusters = BuildClusters(prepared.Value(), profile, work);
            if (clusters.HasError())
                return Result<CookedFoliageClusterSet>::Failure(clusters.ErrorValue());
            output.clusters_ = std::move(clusters).Value();
            if (!work.Spend(output.clusters_.size()))
                return Result<CookedFoliageClusterSet>::Failure(MakeError(*work.error));
            output.manifestDigest_ = ComputeManifestDigest(output.dataset_, output.content_, output.fingerprint_, output.clusters_);
            if (!work.Spend())
                return Result<CookedFoliageClusterSet>::Failure(MakeError(*work.error));
            output.footprint_ = prepared.Value().footprint;
            output.footprint_.workItems = work.used;
            return Result<CookedFoliageClusterSet>::Success(std::move(output));
        }
    };

    /** @copydoc CookFoliageClusters */
    Result<CookedFoliageClusterSet> CookFoliageClusters(const FoliageClusterCookRequest &request, const CancellationToken &cancellation) {
        return FoliageClusterCookWorker{request, cancellation}.Run();
    }

    namespace FoliageClusterCookInternal {
        /** @copydoc ProfileFingerprint */
        Sha256Digest ProfileFingerprint(const FoliageClusterCookProfile &profile) {
            return ProfileDigest(profile);
        }

        /** @copydoc ManifestFingerprint */
        Sha256Digest ManifestFingerprint(const TerrainDatasetId dataset, const TerrainContentRevision content,
                                         const Sha256Digest &fingerprint, const std::span<const CookedFoliageCluster> clusters) {
            return ComputeManifestDigest(dataset, content, fingerprint, clusters);
        }

        /** @copydoc ValidateCluster */
        bool ValidateCluster(const CookedFoliageCluster &cluster, const TerrainDatasetId dataset,
                             const TerrainCapabilityRevision capability, const Sha256Digest &profile,
                             const CancellationToken &cancellation) {
            return ValidCluster(cluster, dataset, capability, profile, cancellation);
        }
    }  // namespace FoliageClusterCookInternal
}  // namespace Horo::Terrain
