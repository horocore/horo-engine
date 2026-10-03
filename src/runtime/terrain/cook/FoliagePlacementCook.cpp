#include "Horo/Terrain/FoliagePlacementCook.h"

#include <algorithm>
#include <array>
#include <limits>
#include <ranges>
#include <string_view>
#include <utility>

namespace Horo::Terrain {
    namespace FoliagePlacementCookErrors {
        namespace {
            const ErrorDomainId Domain{"horo.terrain.foliage.placement"};
        }

        const ErrorCodeDescriptor InvalidInput{Domain, ErrorCode{"terrain.foliage.placement_invalid"}, ErrorSeverity::Error,
                                               "Foliage placement input is invalid.", "Check source, definition, revisions and geometry."};
        const ErrorCodeDescriptor LimitExceeded{Domain, ErrorCode{"terrain.foliage.placement_limit_exceeded"}, ErrorSeverity::Error,
                                                "Foliage placement exceeds its finite limit.",
                                                "Reduce density or raise the declared cook limit."};
        const ErrorCodeDescriptor Cancelled{Domain, ErrorCode{"terrain.foliage.placement_cancelled"}, ErrorSeverity::Warning,
                                            "Foliage placement was cancelled.", "Retry against the current source revision."};
        const ErrorCodeDescriptor Stale{Domain, ErrorCode{"terrain.foliage.placement_stale"}, ErrorSeverity::Error,
                                        "Foliage placement publication is stale.", "Recook against the current generation."};
        const ErrorCodeDescriptor Closed{Domain, ErrorCode{"terrain.foliage.placement_closed"}, ErrorSeverity::Error,
                                         "Foliage placement owner is closed.", "Create a new owner for the next session."};
    }  // namespace FoliagePlacementCookErrors

    namespace {
        constexpr std::uint64_t SquareKilometerInSquareMillimeters = 1'000'000'000'000ULL;
        constexpr std::uint32_t FullDensity = 65'535U;
        constexpr std::int64_t MaximumCoordinateMillimeters = 1'000'000'000'000LL;
        constexpr std::uint16_t PlacementSchemaVersion = 1U;
        constexpr std::uint16_t PlacementPrngVersion = 1U;
        constexpr std::uint16_t PlacementQuantizationVersion = 1U;

        /** @brief Checks exact definition grants; a missing consumer capability is never substituted. */
        [[nodiscard]] bool CapabilitiesCoverDefinition(const FoliageTypeDefinitionData &data,
                                                       const FoliageDefinitionCapabilitySet capabilities) noexcept {
            using enum FoliageDefinitionCapability;
            const auto culling = data.culling.recipe == FoliageCullingRecipe::CpuDirect ? CpuCulling : GpuIndirectCulling;
            return capabilities.Contains(culling) && (!data.assets.impostor || capabilities.Contains(Impostors)) &&
                   (data.wind.model == FoliageWindModel::None || capabilities.Contains(VertexWind)) &&
                   (data.collision.shape == FoliageCollisionShape::None || capabilities.Contains(Collision)) &&
                   (!data.collision.blocksNavigation || capabilities.Contains(NavigationBlocking));
        }

        /** @brief Adds a fixed-width integer in network byte order to a canonical digest. */
        void HashInteger(Sha256Builder &hash, const std::uint64_t value) noexcept {
            std::array<std::byte, 8> bytes{};
            for (std::size_t index = 0; index < bytes.size(); ++index)
                bytes[index] = static_cast<std::byte>(value >> ((7U - index) * 8U));
            static_cast<void>(hash.Update(bytes));
        }

        /** @brief Reads the first 64 canonical bits of a digest. */
        [[nodiscard]] std::uint64_t SeedFrom(const Sha256Digest &digest) noexcept {
            std::uint64_t value{};
            for (std::size_t index = 0; index < 8U; ++index)
                value = (value << 8U) | digest.bytes[index];
            return value;
        }

        /** @brief SplitMix64 v1: fixed unsigned arithmetic with no process RNG or worker state. */
        [[nodiscard]] std::uint64_t Next(std::uint64_t &state) noexcept {
            state += 0x9e3779b97f4a7c15ULL;
            std::uint64_t value = state;
            value = (value ^ (value >> 30U)) * 0xbf58476d1ce4e5b9ULL;
            value = (value ^ (value >> 27U)) * 0x94d049bb133111ebULL;
            return value ^ (value >> 31U);
        }

        /** @brief Encodes two 32-bit provenance coordinates for typed identity derivation. */
        [[nodiscard]] std::array<std::byte, 8> Key(const std::uint32_t x, const std::uint32_t z) noexcept {
            std::array<std::byte, 8> bytes{};
            for (std::size_t index = 0; index < 4U; ++index) {
                bytes[index] = static_cast<std::byte>(x >> ((3U - index) * 8U));
                bytes[index + 4U] = static_cast<std::byte>(z >> ((3U - index) * 8U));
            }
            return bytes;
        }

        /** @brief Names a candidate by cell and local attempt, bound to its exact semantic source. */
        [[nodiscard]] std::array<std::byte, 28> InstanceKey(const Sha256Digest &fingerprint, const std::uint32_t x, const std::uint32_t z,
                                                            const std::uint32_t local) noexcept {
            std::array<std::byte, 28> bytes{};
            const auto cell = Key(x, z);
            std::ranges::copy(cell, bytes.begin());
            for (std::size_t index = 0; index < 4U; ++index)
                bytes[index + 8U] = static_cast<std::byte>(local >> ((3U - index) * 8U));
            for (std::size_t index = 0; index < 16U; ++index)
                bytes[index + 12U] = static_cast<std::byte>(fingerprint.bytes[index]);
            return bytes;
        }

        /** @brief Keeps cluster identities separate across foliage types in the same tile. */
        [[nodiscard]] std::array<std::byte, 24> ClusterKey(const FoliageTypeId type, const std::uint32_t x,
                                                           const std::uint32_t z) noexcept {
            std::array<std::byte, 24> bytes{};
            const auto cell = Key(x, z);
            for (std::size_t index = 0; index < type.Bytes().size(); ++index)
                bytes[index] = static_cast<std::byte>(type.Bytes()[index]);
            std::ranges::copy(cell, bytes.begin() + 16U);
            return bytes;
        }

        /** @brief Checks typed identities and exact rule/capability admission. */
        [[nodiscard]] bool ValidIdentity(const FoliagePlacementCookRequest &request) noexcept {
            const auto &grid = request.grid;
            const auto &definition = request.definition.Data();
            return grid.tile.IsValid() && grid.sourceRevision.IsValid() && grid.capability.IsValid() && request.contentRevision.IsValid() &&
                   definition.type.IsValid() && definition.revision.IsValid() && request.capabilities.IsValid() &&
                   request.tier < TerrainFeatureTier::Count;
        }

        /** @brief Checks versioned rule and grid shape without indexing either. */
        [[nodiscard]] bool ValidRuleShape(const FoliagePlacementCookRequest &request) noexcept {
            const auto &definition = request.definition.Data();
            const auto &grid = request.grid;
            return definition.contractVersion == CurrentFoliageDefinitionContractVersion &&
                   definition.placement.algorithmVersion == CurrentFoliagePlacementAlgorithmVersion &&
                   definition.placement.algorithm == FoliagePlacementAlgorithm::StratifiedJitterV1 &&
                   CapabilitiesCoverDefinition(definition, request.capabilities) && grid.width >= 2U && grid.height >= 2U &&
                   grid.spacingXMillimeters != 0U && grid.spacingZMillimeters != 0U;
        }

        /** @brief Checks finite caller policy and cluster topology. */
        [[nodiscard]] bool ValidPolicyShape(const FoliagePlacementCookRequest &request) noexcept {
            const auto &limits = request.limits;
            return request.clustering.version == 1U && request.clustering.cellsPerCluster != 0U && limits.maximumCells != 0U &&
                   limits.maximumCandidates != 0U && limits.maximumInstances != 0U && limits.maximumClusters != 0U &&
                   limits.maximumExclusions != 0U && limits.maximumPredicateChecks != 0U;
        }

        /** @brief Checks hard implementation ceilings before any allocation. */
        [[nodiscard]] bool WithinHardLimits(const FoliagePlacementCookLimits &limits) noexcept {
            return limits.maximumCells <= 65'536U && limits.maximumCandidates <= 1'048'576U && limits.maximumInstances <= 262'144U &&
                   limits.maximumClusters <= 4'096U && limits.maximumExclusions <= 4'096U && limits.maximumPredicateChecks <= 16'777'216U;
        }

        /** @brief Checks sample count, coordinate extent and density multiplication. */
        [[nodiscard]] bool WithinGridLimits(const FoliagePlacementCookRequest &request) noexcept {
            const auto &grid = request.grid;
            const auto &limits = request.limits;
            const auto density = request.definition.Data().placement.densityPerSquareKilometer;
            return std::uint64_t{grid.width} * grid.height == grid.samples.size() &&
                   std::uint64_t{grid.width - 1U} * (grid.height - 1U) <= limits.maximumCells &&
                   grid.exclusions.size() <= limits.maximumExclusions && grid.originXMillimeters >= -MaximumCoordinateMillimeters &&
                   grid.originXMillimeters <= MaximumCoordinateMillimeters && grid.originZMillimeters >= -MaximumCoordinateMillimeters &&
                   grid.originZMillimeters <= MaximumCoordinateMillimeters &&
                   std::uint64_t{grid.width - 1U} * grid.spacingXMillimeters <= MaximumCoordinateMillimeters &&
                   std::uint64_t{grid.height - 1U} * grid.spacingZMillimeters <= MaximumCoordinateMillimeters &&
                   std::uint64_t{grid.spacingXMillimeters} * grid.spacingZMillimeters <=
                       std::numeric_limits<std::uint64_t>::max() / density;
        }

        /** @brief Rejects malformed quantized surface values. */
        [[nodiscard]] bool ValidSamples(const std::span<const FoliagePlacementSample> samples) noexcept {
            return std::ranges::all_of(samples, [](const auto &sample) {
                const std::int64_t lengthSquared = std::int64_t{sample.normalXPermille} * sample.normalXPermille +
                                                   std::int64_t{sample.normalYPermille} * sample.normalYPermille +
                                                   std::int64_t{sample.normalZPermille} * sample.normalZPermille;
                return sample.slopeMilliDegrees <= 90'000U && sample.normalYPermille > 0 && lengthSquared >= 900'000 &&
                       lengthSquared <= 1'100'000;
            });
        }

        /** @brief Rejects inverted inclusive rectangles. */
        [[nodiscard]] bool ValidExclusions(const std::span<const FoliageExclusionRectangle> exclusions) noexcept {
            return std::ranges::all_of(exclusions, [](const auto &rectangle) {
                return rectangle.minimumXMillimeters <= rectangle.maximumXMillimeters &&
                       rectangle.minimumZMillimeters <= rectangle.maximumZMillimeters;
            });
        }

        /** @brief Rejects unbounded or contradictory grid and rule input before allocating output. */
        [[nodiscard]] Result<void> Validate(const FoliagePlacementCookRequest &request) {
            if (!ValidIdentity(request) || !ValidRuleShape(request) || !ValidPolicyShape(request))
                return Result<void>::Failure(MakeError(FoliagePlacementCookErrors::InvalidInput));
            const auto &grid = request.grid;
            if (request.clustering.cellsPerCluster > std::max(grid.width - 1U, grid.height - 1U))
                return Result<void>::Failure(MakeError(FoliagePlacementCookErrors::InvalidInput));
            if (!WithinHardLimits(request.limits) || !WithinGridLimits(request))
                return Result<void>::Failure(MakeError(FoliagePlacementCookErrors::LimitExceeded));
            if (!ValidSamples(grid.samples) || !ValidExclusions(grid.exclusions))
                return Result<void>::Failure(MakeError(FoliagePlacementCookErrors::InvalidInput));
            return Result<void>::Success();
        }

        /** @brief Hashes the exact spatial values and canonical exclusion sequence. */
        void HashGrid(Sha256Builder &hash, const FoliagePlacementGrid &grid,
                      const std::span<const FoliageExclusionRectangle> exclusions) noexcept {
            HashInteger(hash, static_cast<std::uint64_t>(grid.originXMillimeters));
            HashInteger(hash, static_cast<std::uint64_t>(grid.originZMillimeters));
            HashInteger(hash, grid.spacingXMillimeters);
            HashInteger(hash, grid.spacingZMillimeters);
            HashInteger(hash, grid.width);
            HashInteger(hash, grid.height);
            for (const auto &sample : grid.samples) {
                HashInteger(hash, static_cast<std::uint64_t>(sample.altitudeMillimeters));
                HashInteger(hash, sample.slopeMilliDegrees);
                HashInteger(hash, sample.density);
                HashInteger(hash, sample.hole);
                HashInteger(hash, static_cast<std::uint16_t>(sample.normalXPermille));
                HashInteger(hash, static_cast<std::uint16_t>(sample.normalYPermille));
                HashInteger(hash, static_cast<std::uint16_t>(sample.normalZPermille));
            }
            HashInteger(hash, exclusions.size());
            for (const auto &rectangle : exclusions) {
                HashInteger(hash, static_cast<std::uint64_t>(rectangle.minimumXMillimeters));
                HashInteger(hash, static_cast<std::uint64_t>(rectangle.minimumZMillimeters));
                HashInteger(hash, static_cast<std::uint64_t>(rectangle.maximumXMillimeters));
                HashInteger(hash, static_cast<std::uint64_t>(rectangle.maximumZMillimeters));
            }
        }

        /** @brief Hashes every placement-affecting input without structure padding or native endian dependence. */
        [[nodiscard]] Sha256Digest Fingerprint(const FoliagePlacementCookRequest &request,
                                               const std::span<const FoliageExclusionRectangle> exclusions,
                                               const bool publicationProvenance) noexcept {
            Sha256Builder hash;
            constexpr std::string_view domain{"horo.foliage.placement.v1"};
            static_cast<void>(hash.Update(std::as_bytes(std::span{domain.data(), domain.size()})));
            const auto tileBytes = SerializeTerrainTileId(request.grid.tile);
            static_cast<void>(hash.Update(std::as_bytes(std::span{tileBytes})));
            static_cast<void>(hash.Update(std::as_bytes(std::span{request.definition.Data().type.Bytes()})));
            if (publicationProvenance) {
                HashInteger(hash, request.grid.sourceRevision.Value());
                HashInteger(hash, request.grid.capability.Value());
                HashInteger(hash, request.definition.Data().revision.Value());
                HashInteger(hash, request.contentRevision.Value());
                HashInteger(hash, request.capabilities.bits);
                static_cast<void>(hash.Update(std::as_bytes(std::span{request.targetDigest.bytes})));
                static_cast<void>(hash.Update(std::as_bytes(std::span{request.toolchainDigest.bytes})));
                static_cast<void>(hash.Update(std::as_bytes(std::span{request.layerDependencyDigest.bytes})));
                static_cast<void>(hash.Update(std::as_bytes(std::span{request.splineDependencyDigest.bytes})));
                HashInteger(hash, static_cast<std::uint8_t>(request.tier));
            }
            const auto &rule = request.definition.Data().placement;
            for (const std::uint64_t value :
                 {std::uint64_t{PlacementSchemaVersion}, std::uint64_t{PlacementPrngVersion}, std::uint64_t{PlacementQuantizationVersion},
                  std::uint64_t{request.definition.Data().contractVersion}, std::uint64_t{rule.algorithmVersion}, std::uint64_t{rule.seed},
                  std::uint64_t{rule.densityPerSquareKilometer}, static_cast<std::uint64_t>(rule.minimumAltitudeMillimeters),
                  static_cast<std::uint64_t>(rule.maximumAltitudeMillimeters), std::uint64_t{rule.minimumSlopeMilliDegrees},
                  std::uint64_t{rule.maximumSlopeMilliDegrees}, std::uint64_t{rule.minimumSeparationMillimeters},
                  std::uint64_t{rule.coordinateQuantumMillimeters}, std::uint64_t{request.definition.Data().minimumScalePermille},
                  std::uint64_t{request.definition.Data().maximumScalePermille}, std::uint64_t{request.definition.Data().maximumInstances},
                  std::uint64_t{request.clustering.version}, std::uint64_t{request.clustering.cellsPerCluster},
                  std::uint64_t{request.clustering.radiusMillimeters}})
                HashInteger(hash, value);
            if (publicationProvenance) {
                for (const std::uint64_t value :
                     {std::uint64_t{request.limits.maximumCells}, std::uint64_t{request.limits.maximumCandidates},
                      std::uint64_t{request.limits.maximumInstances}, std::uint64_t{request.limits.maximumClusters},
                      std::uint64_t{request.limits.maximumExclusions}, request.limits.maximumPredicateChecks})
                    HashInteger(hash, value);
            }
            HashInteger(hash, static_cast<std::uint8_t>(rule.algorithm));
            HashInteger(hash, static_cast<std::uint8_t>(rule.alignment));
            HashGrid(hash, request.grid, exclusions);
            return hash.Finalize();
        }

        /** @brief Derives a stable per-cell stream from the complete cook fingerprint. */
        [[nodiscard]] std::uint64_t CellSeed(const Sha256Digest &fingerprint, const std::uint32_t x, const std::uint32_t z) noexcept {
            return SeedFrom(fingerprint) ^ (std::uint64_t{x} << 32U) ^ z;
        }

        /** @brief Quantizes non-negative coordinates to the authored quantum. */
        [[nodiscard]] std::int64_t Quantize(const std::int64_t value, const std::uint32_t quantum) noexcept {
            const std::int64_t divisor = quantum;
            const std::int64_t quotient = value >= 0 ? value / divisor : -((-value + divisor - 1) / divisor);
            return quotient * divisor;
        }

        /** @brief Hashes canonical output fields for byte-independent result equality. */
        [[nodiscard]] Sha256Digest DigestInstances(const std::span<const CookedFoliageInstance> instances,
                                                   const CancellationToken *cancellation = nullptr) noexcept {
            Sha256Builder hash;
            constexpr std::string_view domain{"horo.foliage.placement.result.v1"};
            static_cast<void>(hash.Update(std::as_bytes(std::span{domain.data(), domain.size()})));
            HashInteger(hash, instances.size());
            for (const auto &instance : instances) {
                if (cancellation && cancellation->IsCancellationRequested())
                    return {};
                static_cast<void>(hash.Update(std::as_bytes(std::span{instance.id.Bytes()})));
                static_cast<void>(hash.Update(std::as_bytes(std::span{instance.cluster.Bytes()})));
                static_cast<void>(hash.Update(std::as_bytes(std::span{instance.type.Bytes()})));
                HashInteger(hash, static_cast<std::uint64_t>(instance.xMillimeters));
                HashInteger(hash, static_cast<std::uint64_t>(instance.altitudeMillimeters));
                HashInteger(hash, static_cast<std::uint64_t>(instance.zMillimeters));
                HashInteger(hash, instance.scalePermille);
                HashInteger(hash, instance.yawMilliDegrees);
                HashInteger(hash, instance.slopeMilliDegrees);
                HashInteger(hash, instance.candidateOrdinal);
                HashInteger(hash, static_cast<std::uint16_t>(instance.normalXPermille));
                HashInteger(hash, static_cast<std::uint16_t>(instance.normalYPermille));
                HashInteger(hash, static_cast<std::uint16_t>(instance.normalZPermille));
            }
            return hash.Finalize();
        }

        /** @brief Checks an integer disk without signed squares or unsigned subtraction underflow. */
        [[nodiscard]] bool InsideDisk(const std::int64_t offsetX, const std::int64_t offsetZ, const std::uint32_t radius) noexcept {
            const auto distanceX = static_cast<std::uint64_t>(offsetX < 0 ? -offsetX : offsetX);
            const auto distanceZ = static_cast<std::uint64_t>(offsetZ < 0 ? -offsetZ : offsetZ);
            const auto radiusSquared = std::uint64_t{radius} * radius;
            return distanceZ <= radius && distanceX <= radius && distanceX * distanceX <= radiusSquared - distanceZ * distanceZ;
        }

        /** @brief Applies the exact inclusive authored local surface constraints. */
        [[nodiscard]] bool SurfaceAllowed(const FoliagePlacementSample &sample, const FoliagePlacementDefinition &rule) noexcept {
            return !sample.hole && sample.density != 0U && sample.altitudeMillimeters >= rule.minimumAltitudeMillimeters &&
                   sample.altitudeMillimeters <= rule.maximumAltitudeMillimeters &&
                   sample.slopeMilliDegrees >= rule.minimumSlopeMilliDegrees && sample.slopeMilliDegrees <= rule.maximumSlopeMilliDegrees;
        }

        /** @brief Quantized position and source sample selected for one candidate. */
        struct PlacementPosition final {
            std::int64_t x{};
            std::int64_t z{};
            std::uint32_t clusterX{};
            std::uint32_t clusterZ{};
            const FoliagePlacementSample *sample{};
        };

        /** @brief One detached, bounded cook's mutable scratch state. */
        struct PlacementWork final {
            const FoliagePlacementCookRequest &request;
            const std::span<const FoliageExclusionRectangle> exclusions;
            const Sha256Digest &semanticDigest;
            const CancellationToken &cancellation;
            std::vector<CookedFoliageInstance> instances{};
            std::vector<FoliageClusterId> clusters{};
            std::uint32_t ordinal{};
            std::uint64_t predicateChecks{};

            /** @brief Samples an integer cluster disk and floor-quantizes within the half-open tile. */
            [[nodiscard]] std::optional<PlacementPosition> Position(const std::uint32_t cellX, const std::uint32_t cellZ,
                                                                    std::uint64_t &random) const noexcept {
                const auto &grid = request.grid;
                const auto &clusterRule = request.clustering;
                std::int64_t x = grid.originXMillimeters + std::int64_t{cellX} * grid.spacingXMillimeters +
                                 static_cast<std::int64_t>(Next(random) % grid.spacingXMillimeters);
                std::int64_t z = grid.originZMillimeters + std::int64_t{cellZ} * grid.spacingZMillimeters +
                                 static_cast<std::int64_t>(Next(random) % grid.spacingZMillimeters);
                const std::uint32_t clusterX = cellX / clusterRule.cellsPerCluster;
                const std::uint32_t clusterZ = cellZ / clusterRule.cellsPerCluster;
                if (clusterRule.radiusMillimeters != 0U) {
                    const std::uint64_t spanX = std::uint64_t{clusterRule.cellsPerCluster} * grid.spacingXMillimeters;
                    const std::uint64_t spanZ = std::uint64_t{clusterRule.cellsPerCluster} * grid.spacingZMillimeters;
                    const std::int64_t centerX = grid.originXMillimeters + static_cast<std::int64_t>(clusterX * spanX + spanX / 2U);
                    const std::int64_t centerZ = grid.originZMillimeters + static_cast<std::int64_t>(clusterZ * spanZ + spanZ / 2U);
                    const std::uint64_t diameter = std::uint64_t{clusterRule.radiusMillimeters} * 2U + 1U;
                    const std::int64_t offsetX = static_cast<std::int64_t>(Next(random) % diameter) - clusterRule.radiusMillimeters;
                    const std::int64_t offsetZ = static_cast<std::int64_t>(Next(random) % diameter) - clusterRule.radiusMillimeters;
                    if (!InsideDisk(offsetX, offsetZ, clusterRule.radiusMillimeters))
                        return std::nullopt;
                    x = centerX + offsetX;
                    z = centerZ + offsetZ;
                }
                x = Quantize(x, request.definition.Data().placement.coordinateQuantumMillimeters);
                z = Quantize(z, request.definition.Data().placement.coordinateQuantumMillimeters);
                const std::int64_t localX = x - grid.originXMillimeters;
                const std::int64_t localZ = z - grid.originZMillimeters;
                if (localX < 0 || localZ < 0 ||
                    static_cast<std::uint64_t>(localX) >= std::uint64_t{grid.width - 1U} * grid.spacingXMillimeters ||
                    static_cast<std::uint64_t>(localZ) >= std::uint64_t{grid.height - 1U} * grid.spacingZMillimeters)
                    return std::nullopt;
                const auto sampleX = static_cast<std::size_t>(localX / grid.spacingXMillimeters);
                const auto sampleZ = static_cast<std::size_t>(localZ / grid.spacingZMillimeters);
                return PlacementPosition{x, z, clusterX, clusterZ, &grid.samples[sampleZ * grid.width + sampleX]};
            }

            /** @brief Tests sorted inclusive authored exclusions with cooperative cancellation. */
            [[nodiscard]] Result<bool> IsExcluded(const PlacementPosition &position) const {
                for (std::size_t index = 0; index < exclusions.size(); ++index) {
                    if ((index & 255U) == 0U && cancellation.IsCancellationRequested())
                        return Result<bool>::Failure(MakeError(FoliagePlacementCookErrors::Cancelled));
                    const auto &rectangle = exclusions[index];
                    if (position.x >= rectangle.minimumXMillimeters && position.x <= rectangle.maximumXMillimeters &&
                        position.z >= rectangle.minimumZMillimeters && position.z <= rectangle.maximumZMillimeters)
                        return Result<bool>::Success(true);
                }
                return Result<bool>::Success(false);
            }

            /** @brief Tests accepted-instance separation in fixed-point squared distance. */
            [[nodiscard]] Result<bool> IsSeparated(const PlacementPosition &position) const {
                const auto separation = request.definition.Data().placement.minimumSeparationMillimeters;
                const std::uint64_t separationSquared = std::uint64_t{separation} * separation;
                for (std::size_t index = 0; index < instances.size(); ++index) {
                    if ((index & 255U) == 0U && cancellation.IsCancellationRequested())
                        return Result<bool>::Failure(MakeError(FoliagePlacementCookErrors::Cancelled));
                    const auto &prior = instances[index];
                    const auto dx =
                        static_cast<std::uint64_t>(std::max(position.x, prior.xMillimeters) - std::min(position.x, prior.xMillimeters));
                    const auto dz =
                        static_cast<std::uint64_t>(std::max(position.z, prior.zMillimeters) - std::min(position.z, prior.zMillimeters));
                    if (dx < separation && dz < separation && dx * dx < separationSquared - dz * dz)
                        return Result<bool>::Success(false);
                }
                return Result<bool>::Success(true);
            }

            /** @brief Enforces predicate budget and authored local surface constraints. */
            [[nodiscard]] Result<bool> Eligible(const PlacementPosition &position) {
                const std::uint64_t needed = exclusions.size() + instances.size() + clusters.size();
                if (needed > request.limits.maximumPredicateChecks - predicateChecks)
                    return Result<bool>::Failure(MakeError(FoliagePlacementCookErrors::LimitExceeded));
                predicateChecks += needed;
                auto excluded = IsExcluded(position);
                if (excluded.HasError())
                    return excluded;
                if (excluded.Value())
                    return Result<bool>::Success(false);
                if (!SurfaceAllowed(*position.sample, request.definition.Data().placement))
                    return Result<bool>::Success(false);
                return IsSeparated(position);
            }

            /** @brief Appends one accepted stable identity and neutral transform or fails before publication. */
            [[nodiscard]] Result<void> Append(const PlacementPosition &position, const std::uint32_t cellX, const std::uint32_t cellZ,
                                              const std::uint32_t local, std::uint64_t &random) {
                const auto &data = request.definition.Data();
                if (instances.size() >= request.limits.maximumInstances || instances.size() >= data.maximumInstances)
                    return Result<void>::Failure(MakeError(FoliagePlacementCookErrors::LimitExceeded));
                auto cluster = DeriveFoliageClusterId(request.grid.tile, ClusterKey(data.type, position.clusterX, position.clusterZ));
                auto instance = DeriveFoliageInstanceId(request.grid.tile, data.type, InstanceKey(semanticDigest, cellX, cellZ, local));
                if (cluster.HasError() || instance.HasError())
                    return Result<void>::Failure(MakeError(FoliagePlacementCookErrors::InvalidInput));
                if (std::ranges::find(clusters, cluster.Value()) == clusters.end()) {
                    if (clusters.size() >= request.limits.maximumClusters)
                        return Result<void>::Failure(MakeError(FoliagePlacementCookErrors::LimitExceeded));
                    clusters.push_back(cluster.Value());
                }
                const std::uint64_t scaleRange = std::uint64_t{data.maximumScalePermille} - data.minimumScalePermille + 1U;
                const auto &sample = *position.sample;
                const auto normalAligned = data.placement.alignment == FoliageSurfaceAlignment::SurfaceNormal;
                instances.push_back({.id = instance.Value(),
                                     .cluster = cluster.Value(),
                                     .type = data.type,
                                     .xMillimeters = position.x,
                                     .altitudeMillimeters = sample.altitudeMillimeters,
                                     .zMillimeters = position.z,
                                     .scalePermille = data.minimumScalePermille + static_cast<std::uint32_t>(Next(random) % scaleRange),
                                     .yawMilliDegrees = static_cast<std::uint32_t>(Next(random) % 360'000U),
                                     .slopeMilliDegrees = sample.slopeMilliDegrees,
                                     .candidateOrdinal = ordinal,
                                     .normalXPermille = normalAligned ? sample.normalXPermille : std::int16_t{0},
                                     .normalYPermille = normalAligned ? sample.normalYPermille : std::int16_t{1'000},
                                     .normalZPermille = normalAligned ? sample.normalZPermille : std::int16_t{0}});
                return Result<void>::Success();
            }

            /** @brief Evaluates exact candidate count in one authored cell. */
            [[nodiscard]] Result<void> Cell(const std::uint32_t x, const std::uint32_t z, const std::uint64_t densityArea) {
                if (cancellation.IsCancellationRequested())
                    return Result<void>::Failure(MakeError(FoliagePlacementCookErrors::Cancelled));
                const auto &sample = request.grid.samples[std::size_t{z} * request.grid.width + x];
                if (sample.hole || sample.density == 0U)
                    return Result<void>::Success();
                const std::uint64_t scaled =
                    (densityArea / FullDensity) * sample.density + ((densityArea % FullDensity) * sample.density) / FullDensity;
                std::uint64_t random = CellSeed(semanticDigest, x, z);
                const std::uint64_t count =
                    scaled / SquareKilometerInSquareMillimeters +
                    std::uint64_t{(Next(random) % SquareKilometerInSquareMillimeters) < (scaled % SquareKilometerInSquareMillimeters)};
                for (std::uint64_t local = 0; local < count; ++local) {
                    if (cancellation.IsCancellationRequested())
                        return Result<void>::Failure(MakeError(FoliagePlacementCookErrors::Cancelled));
                    ++ordinal;
                    auto position = Position(x, z, random);
                    if (!position)
                        continue;
                    auto eligible = Eligible(*position);
                    if (eligible.HasError())
                        return Result<void>::Failure(eligible.ErrorValue());
                    if (!eligible.Value())
                        continue;
                    if (auto appended = Append(*position, x, z, static_cast<std::uint32_t>(local), random); appended.HasError())
                        return appended;
                }
                return Result<void>::Success();
            }

            /** @brief Traverses cells in canonical Z then X order. */
            [[nodiscard]] Result<void> Run(const std::uint64_t densityArea) {
                for (std::uint32_t z = 0; z < request.grid.height - 1U; ++z) {
                    for (std::uint32_t x = 0; x < request.grid.width - 1U; ++x) {
                        if (auto result = Cell(x, z, densityArea); result.HasError())
                            return result;
                    }
                }
                if (cancellation.IsCancellationRequested())
                    return Result<void>::Failure(MakeError(FoliagePlacementCookErrors::Cancelled));
                return Result<void>::Success();
            }
        };
    }  // namespace

    /** @copydoc CookFoliagePlacement */
    Result<CookedFoliagePlacement> CookFoliagePlacement(const FoliagePlacementCookRequest &request, const CancellationToken &cancellation) {
        if (auto valid = Validate(request); valid.HasError())
            return Result<CookedFoliagePlacement>::Failure(valid.ErrorValue());
        if (cancellation.IsCancellationRequested())
            return Result<CookedFoliagePlacement>::Failure(MakeError(FoliagePlacementCookErrors::Cancelled));

        std::vector<FoliageExclusionRectangle> exclusions{request.grid.exclusions.begin(), request.grid.exclusions.end()};
        std::ranges::sort(exclusions);
        if (std::ranges::adjacent_find(exclusions) != exclusions.end())
            return Result<CookedFoliagePlacement>::Failure(MakeError(FoliagePlacementCookErrors::InvalidInput));
        const auto fingerprint = Fingerprint(request, exclusions, true);
        const auto semanticDigest = Fingerprint(request, exclusions, false);
        const auto &grid = request.grid;
        const auto &rule = request.definition.Data().placement;
        const std::uint64_t area = std::uint64_t{grid.spacingXMillimeters} * grid.spacingZMillimeters;
        const std::uint64_t densityArea = area * rule.densityPerSquareKilometer;
        const std::uint64_t maxPerCell =
            densityArea / SquareKilometerInSquareMillimeters + std::uint64_t{densityArea % SquareKilometerInSquareMillimeters != 0U};
        if (const auto cellCount = std::uint64_t{grid.width - 1U} * (grid.height - 1U);
            maxPerCell > request.limits.maximumCandidates || cellCount > request.limits.maximumCandidates / maxPerCell)
            return Result<CookedFoliagePlacement>::Failure(MakeError(FoliagePlacementCookErrors::LimitExceeded));

        PlacementWork work{request, exclusions, semanticDigest, cancellation};
        if (auto result = work.Run(densityArea); result.HasError())
            return Result<CookedFoliagePlacement>::Failure(result.ErrorValue());
        CookedFoliagePlacement output;
        output.tile_ = grid.tile;
        output.sourceRevision_ = grid.sourceRevision;
        output.capability_ = grid.capability;
        output.type_ = request.definition.Data().type;
        output.definitionRevision_ = request.definition.Data().revision;
        output.contentRevision_ = request.contentRevision;
        output.targetDigest_ = request.targetDigest;
        output.toolchainDigest_ = request.toolchainDigest;
        output.tier_ = request.tier;
        output.fingerprint_ = fingerprint;
        output.instances_ = std::move(work.instances);
        output.resultDigest_ = DigestInstances(output.instances_);
        return Result<CookedFoliagePlacement>::Success(std::move(output));
    }

    /** @brief Rechecks detached candidate integrity before owner publication. */
    bool CookedFoliagePlacement::IsWellFormed(const CancellationToken &cancellation) const noexcept {
        return tile_.IsValid() && type_.IsValid() && contentRevision_.IsValid() && sourceRevision_.IsValid() && capability_.IsValid() &&
               definitionRevision_.IsValid() && !cancellation.IsCancellationRequested() &&
               resultDigest_ == DigestInstances(instances_, &cancellation);
    }

    /** @copydoc FoliagePlacementCookOwner::Publish */
    Result<void> FoliagePlacementCookOwner::Publish(CookedFoliagePlacement candidate,
                                                    const std::optional<TerrainContentRevision> expectedCurrent) {
        if (closed_)
            return Result<void>::Failure(MakeError(FoliagePlacementCookErrors::Closed));
        if (current_.has_value() != expectedCurrent.has_value())
            return Result<void>::Failure(MakeError(FoliagePlacementCookErrors::Stale));
        if (current_ &&
            (current_->contentRevision_ != *expectedCurrent || candidate.tile_ != current_->tile_ || candidate.type_ != current_->type_ ||
             current_->contentRevision_.Value() == std::numeric_limits<std::uint64_t>::max() ||
             candidate.contentRevision_.Value() != current_->contentRevision_.Value() + 1U))
            return Result<void>::Failure(MakeError(FoliagePlacementCookErrors::Stale));
        if (!candidate.IsWellFormed())
            return Result<void>::Failure(MakeError(FoliagePlacementCookErrors::InvalidInput));
        current_ = std::move(candidate);
        return Result<void>::Success();
    }
}  // namespace Horo::Terrain
