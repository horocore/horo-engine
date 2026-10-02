#include "Horo/Navigation/NavigationTileDependencies.h"

#include "Horo/Navigation/NavigationErrors.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <limits>

namespace Horo::Navigation {
    namespace {
        /** @brief Fixed-width canonical SHA-256 chain; no native padding, paths or operation identities enter keys. */
        class TileKeyWriter final {
        public:
            void Integer(const std::uint64_t value) noexcept {
                std::array<std::byte, 8> bytes{};
                for (std::size_t index = 0; index < bytes.size(); ++index)
                    bytes[index] = static_cast<std::byte>((value >> (index * 8U)) & 0xffU);
                Add(bytes);
            }

            void Float(const float value) noexcept {
                Integer(std::bit_cast<std::uint32_t>(value == 0.0F ? 0.0F : value));
            }

            void Vector(const Math::Vec3 value) noexcept {
                Float(value.x);
                Float(value.y);
                Float(value.z);
            }

            void Digest(const Sha256Digest &value) noexcept {
                Add(std::as_bytes(std::span{value.bytes}));
            }

            [[nodiscard]] Sha256Digest Finish() const noexcept {
                return digest_;
            }

        private:
            void Add(const std::span<const std::byte> value) noexcept {
                std::array<std::byte, 64> bytes{};
                std::ranges::copy(std::as_bytes(std::span{digest_.bytes}), bytes.begin());
                std::ranges::copy(value, bytes.begin() + 32);
                digest_ = ComputeSha256(std::span{bytes}.first(32 + value.size()));
            }

            static constexpr std::string_view Domain = "horo.navigation.tile-dependencies.v1";
            Sha256Digest digest_ = ComputeSha256(std::as_bytes(std::span{Domain.data(), Domain.size()}));
        };

        /** @brief Rejects absent compatibility evidence rather than treating it as a valid cache namespace. */
        [[nodiscard]] bool Present(const Sha256Digest &digest) noexcept {
            return std::ranges::any_of(digest.bytes, [](const auto byte) {
                return byte != 0;
            });
        }

        /** @brief Conservative closed intersection includes exact border contacts and vertical rasterization inputs. */
        [[nodiscard]] bool Intersects(const Math::Aabb &left, const Math::Aabb &right) noexcept {
            return left.minimum.x <= right.maximum.x && left.maximum.x >= right.minimum.x && left.minimum.y <= right.maximum.y &&
                   left.maximum.y >= right.minimum.y && left.minimum.z <= right.maximum.z && left.maximum.z >= right.minimum.z;
        }

        /** @brief Computes the conservative triangle footprint without relying on producer-supplied bounds. */
        [[nodiscard]] Math::Aabb Bounds(const NavigationTileBuildTriangle &triangle) noexcept {
            Math::Aabb result{triangle.vertices.front(), triangle.vertices.front()};
            for (const auto vertex : triangle.vertices) {
                result.minimum.x = std::min(result.minimum.x, vertex.x);
                result.minimum.y = std::min(result.minimum.y, vertex.y);
                result.minimum.z = std::min(result.minimum.z, vertex.z);
                result.maximum.x = std::max(result.maximum.x, vertex.x);
                result.maximum.y = std::max(result.maximum.y, vertex.y);
                result.maximum.z = std::max(result.maximum.z, vertex.z);
            }
            return result;
        }

        /** @brief Hashes exact profile geometry; a host settings digest cannot hide a changed resolved profile. */
        void Geometry(TileKeyWriter &writer, const NavigationAgentBuildGeometry &geometry) noexcept {
            writer.Float(geometry.radiusMeters);
            writer.Float(geometry.heightMeters);
            writer.Float(geometry.maxSlopeDegrees);
            writer.Float(geometry.stepHeightMeters);
            writer.Float(geometry.cellSizeMeters);
            writer.Float(geometry.cellHeightMeters);
            writer.Float(geometry.minimumRegionSizeMeters);
        }

        /** @brief Hashes source evidence AND actual rasterization values, preventing dishonest or stale producer digests from aliasing. */
        void Triangle(TileKeyWriter &writer, const NavigationTileBuildTriangle &triangle) noexcept {
            writer.Integer(static_cast<std::uint8_t>(triangle.provenance.kind));
            writer.Integer(triangle.provenance.producer.Value());
            writer.Integer(triangle.provenance.contribution.Value());
            writer.Integer(triangle.provenance.revision.Value());
            writer.Digest(triangle.provenance.contentDigest);
            writer.Integer(triangle.provenance.sourceTriangleIndex);
            writer.Integer(triangle.area.Value());
            writer.Float(triangle.traversalCost);
            writer.Integer(triangle.materialSlot.value);
            for (const auto vertex : triangle.vertices)
                writer.Vector(vertex);
        }

        /** @brief Hashes canonical modifier identity, operation, area and full bounds. */
        void Modifier(TileKeyWriter &writer, const NavigationTileBuildModifier &modifier) noexcept {
            writer.Integer(modifier.id.Value());
            writer.Integer(static_cast<std::uint8_t>(modifier.mode));
            writer.Integer(modifier.area.Value());
            writer.Vector(modifier.canonicalBounds.minimum);
            writer.Vector(modifier.canonicalBounds.maximum);
        }

        /** @brief Validated rasterization halo for one integral grid tile. */
        struct TileSampling final {
            Math::Aabb bounds;
            std::uint32_t border{};
        };

        /** @brief Validates grid placement and expands only the sampling footprint. */
        [[nodiscard]] Result<TileSampling> Sampling(const NavigationBakeTile &tile, const NavigationAgentBuildGeometry &geometry) {
            const auto invalid = [] {
                return Result<TileSampling>::Failure(MakeError(NavigationErrors::BakeInputInvalid));
            };
            if (!tile.bounds.IsValid() || !std::isfinite(tile.tileSizeMeters) || tile.tileSizeMeters <= 0.0F)
                return invalid();
            const double cells = static_cast<double>(tile.tileSizeMeters) / geometry.cellSizeMeters;
            const double border = std::ceil(static_cast<double>(geometry.radiusMeters) / geometry.cellSizeMeters) + 3.0;
            if (!std::isfinite(cells) || cells < 1 || cells > 65'000 || std::abs(cells - std::round(cells)) > 1.0e-4 || border >= 255)
                return invalid();
            const float x = static_cast<float>(tile.key.tile.x) * tile.tileSizeMeters;
            const float z = static_cast<float>(tile.key.tile.z) * tile.tileSizeMeters;
            if (!std::isfinite(x) || !std::isfinite(z) || tile.bounds.minimum.x != x || tile.bounds.minimum.z != z ||
                tile.bounds.maximum.x != x + tile.tileSizeMeters || tile.bounds.maximum.z != z + tile.tileSizeMeters ||
                tile.bounds.maximum.y <= tile.bounds.minimum.y)
                return invalid();
            const float halo = static_cast<float>(border) * geometry.cellSizeMeters;
            const Math::Aabb bounds{{x - halo, tile.bounds.minimum.y, z - halo},
                                    {tile.bounds.maximum.x + halo, tile.bounds.maximum.y, tile.bounds.maximum.z + halo}};
            if (!bounds.IsValid())
                return invalid();
            return Result<TileSampling>::Success({bounds, static_cast<std::uint32_t>(border)});
        }

        /** @brief Owns intersecting rows within the conservative vector-growth budget. */
        [[nodiscard]] Result<void> SelectDependencies(NavigationPreparedTile &result, const NavigationBakeInputSnapshot &input,
                                                      const NavigationTileBuildPartition &partition, const Math::Aabb &sampling,
                                                      const CancellationToken &cancellation, const std::size_t maximumBytes) {
            std::size_t selectedBytes{};
            for (const auto &triangle : input.Triangles().subspan(partition.firstTriangle, partition.triangleCount)) {
                if (cancellation.IsCancellationRequested())
                    return Result<void>::Failure(MakeError(NavigationErrors::BakeInputCancelled));
                if (Intersects(Bounds(triangle), sampling)) {
                    if (sizeof(triangle) > maximumBytes / 2 - selectedBytes)
                        return Result<void>::Failure(MakeError(NavigationErrors::BakeInputCapacityExceeded));
                    selectedBytes += sizeof(triangle);
                    result.triangles.push_back(triangle);
                }
            }
            for (const auto &modifier : input.Modifiers().subspan(partition.firstModifier, partition.modifierCount)) {
                if (cancellation.IsCancellationRequested())
                    return Result<void>::Failure(MakeError(NavigationErrors::BakeInputCancelled));
                if (Intersects(modifier.canonicalBounds, sampling)) {
                    if (sizeof(modifier) > maximumBytes / 2 - selectedBytes)
                        return Result<void>::Failure(MakeError(NavigationErrors::BakeInputCapacityExceeded));
                    selectedBytes += sizeof(modifier);
                    result.modifiers.push_back(modifier);
                }
            }
            return Result<void>::Success();
        }

        /** @brief Includes registry semantics only for areas actually referenced by selected inputs. */
        void ReferencedAreas(TileKeyWriter &writer, const NavigationPreparedTile &result,
                             const std::span<const NavigationResolvedBakeArea> areas) noexcept {
            for (const auto &area : areas) {
                const bool used = std::ranges::any_of(result.triangles, [&area](const auto &v) {
                    return v.area == area.id;
                }) || std::ranges::any_of(result.modifiers, [&area](const auto &v) {
                    return v.area == area.id;
                });
                if (used) {
                    writer.Integer(area.id.Value());
                    writer.Integer(static_cast<std::uint8_t>(area.source.kind));
                    writer.Integer(area.source.id.Value());
                    writer.Float(area.traversalCost);
                    writer.Integer(area.flags.bits);
                }
            }
        }

        /** @brief Canonical identity covers grid, compatibility and the complete selected rasterization closure. */
        [[nodiscard]] Sha256Digest DependencyKey(const NavigationPreparedTile &result, const NavigationBakeInputSnapshot &input,
                                                 const NavigationTileBuildPartition &partition,
                                                 const NavigationTileBakeCompatibility &compatibility) noexcept {
            TileKeyWriter writer;
            writer.Digest(compatibility.provider);
            writer.Digest(compatibility.schemas);
            writer.Digest(compatibility.settings);
            const auto &tile = result.tile;
            writer.Integer(tile.key.profile.Value());
            writer.Integer(tile.key.surface.Value());
            writer.Integer(partition.filter.Value());
            writer.Integer(static_cast<std::uint32_t>(tile.key.tile.x));
            writer.Integer(static_cast<std::uint32_t>(tile.key.tile.z));
            writer.Integer(tile.key.tile.layer);
            writer.Vector(tile.bounds.minimum);
            writer.Vector(tile.bounds.maximum);
            writer.Float(tile.tileSizeMeters);
            writer.Integer(result.borderSizeCells);
            writer.Integer(input.Revisions().coordinates.Value());
            Geometry(writer, result.geometry);
            writer.Integer(result.triangles.size());
            for (const auto &triangle : result.triangles)
                Triangle(writer, triangle);
            writer.Integer(result.modifiers.size());
            for (const auto &modifier : result.modifiers)
                Modifier(writer, modifier);
            ReferencedAreas(writer, result, input.Areas());
            return writer.Finish();
        }
    }  // namespace

    /** @copydoc PrepareNavigationBakeTile */
    Result<NavigationPreparedTile> PrepareNavigationBakeTile(const NavigationBakeInputSnapshot &input, const NavigationBakeTile &tile,
                                                             const NavigationTileBakeCompatibility &compatibility,
                                                             const CancellationToken &cancellation, const std::size_t maximumOwnedBytes) {
        if (cancellation.IsCancellationRequested())
            return Result<NavigationPreparedTile>::Failure(MakeError(NavigationErrors::BakeInputCancelled));
        if (maximumOwnedBytes == 0 || maximumOwnedBytes > NavigationBakeInputLimits::MaximumOwnedBytes ||
            !Present(compatibility.provider) || !Present(compatibility.schemas) || !Present(compatibility.settings))
            return Result<NavigationPreparedTile>::Failure(MakeError(NavigationErrors::BakeInputInvalid));
        const auto partition = std::ranges::find_if(input.Partitions(), [&tile](const auto &value) {
            return value.surface == tile.key.surface && value.profile == tile.key.profile;
        });
        const auto profile = std::ranges::find(input.Profiles(), tile.key.profile, &NavigationResolvedBakeProfile::id);
        if (partition == input.Partitions().end() || profile == input.Profiles().end())
            return Result<NavigationPreparedTile>::Failure(MakeError(NavigationErrors::BakeInputInvalid));
        const auto sampling = Sampling(tile, profile->buildGeometry);
        if (sampling.HasError())
            return Result<NavigationPreparedTile>::Failure(sampling.ErrorValue());
        try {
            NavigationPreparedTile result{.tile = tile, .geometry = profile->buildGeometry, .borderSizeCells = sampling.Value().border};
            if (const auto selected =
                    SelectDependencies(result, input, *partition, sampling.Value().bounds, cancellation, maximumOwnedBytes);
                selected.HasError())
                return Result<NavigationPreparedTile>::Failure(selected.ErrorValue());
            result.dependencyKey = DependencyKey(result, input, *partition, compatibility);
            return Result<NavigationPreparedTile>::Success(std::move(result));
        } catch (const std::bad_alloc &) {
            return Result<NavigationPreparedTile>::Failure(MakeError(NavigationErrors::BakeInputCapacityExceeded));
        }
    }
}  // namespace Horo::Navigation
