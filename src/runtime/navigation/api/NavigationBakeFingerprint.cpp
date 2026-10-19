#include "NavigationBakeFingerprintInternal.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>

namespace Horo::Navigation::Internal {
    namespace {
        class FingerprintBuilder final {
        public:
            explicit FingerprintBuilder(bool scoped) noexcept {
                static constexpr std::array<std::byte, 28> Domain{
                    std::byte{0x68}, std::byte{0x6f}, std::byte{0x72}, std::byte{0x6f}, std::byte{0x2e}, std::byte{0x6e}, std::byte{0x61},
                    std::byte{0x76}, std::byte{0x2e}, std::byte{0x62}, std::byte{0x61}, std::byte{0x6b}, std::byte{0x65}, std::byte{0x2e},
                    std::byte{0x69}, std::byte{0x6e}, std::byte{0x70}, std::byte{0x75}, std::byte{0x74}, std::byte{0x2e}, std::byte{0x76},
                    std::byte{0x31}, std::byte{0},    std::byte{0},    std::byte{0},    std::byte{0},    std::byte{0},    std::byte{1},
                };
                auto domain = Domain;
                if (scoped) {
                    domain[21] = std::byte{0x32};
                    domain[27] = std::byte{2};
                }
                digest_ = ComputeSha256(std::span<const std::byte>{domain});
            }

            void AddU64(const std::uint64_t value) noexcept {
                std::array<std::byte, 8> bytes{};
                for (std::size_t index = 0; index < bytes.size(); ++index)
                    bytes[index] = static_cast<std::byte>((value >> ((bytes.size() - 1 - index) * 8)) & 0xFFU);
                Add(bytes);
            }

            void AddU32(const std::uint32_t value) noexcept {
                AddU64(value);
            }

            void AddFloat(const float value) noexcept {
                AddU32(std::bit_cast<std::uint32_t>(value == 0.0F ? 0.0F : value));
            }

            void AddDigest(const Sha256Digest &value) noexcept {
                std::array<std::byte, 32> bytes{};
                for (std::size_t index = 0; index < bytes.size(); ++index)
                    bytes[index] = static_cast<std::byte>(value.bytes[index]);
                Add(bytes);
            }

            [[nodiscard]] Sha256Digest Finish() const noexcept {
                return digest_;
            }

        private:
            template <std::size_t Size> void Add(const std::array<std::byte, Size> &value) noexcept {
                static_assert(Size <= 32);
                std::array<std::byte, 64> input{};
                for (std::size_t index = 0; index < digest_.bytes.size(); ++index)
                    input[index] = static_cast<std::byte>(digest_.bytes[index]);
                std::ranges::copy(value, input.begin() + static_cast<std::ptrdiff_t>(digest_.bytes.size()));
                digest_ = ComputeSha256(std::span{input}.first(digest_.bytes.size() + value.size()));
            }

            Sha256Digest digest_{};
        };

        void Add(FingerprintBuilder &builder, const NavigationBakeInputRevisions &revisions) noexcept {
            builder.AddU64(revisions.definition.Value());
            builder.AddU64(revisions.scene.Value());
            builder.AddU64(revisions.areaRegistry.Value());
            builder.AddU64(revisions.projectProfile.Value());
            builder.AddU64(revisions.coordinates.Value());
            builder.AddU64(revisions.geometry.Value());
        }

        void Add(FingerprintBuilder &builder, const Math::Vec3 value) noexcept {
            builder.AddFloat(value.x);
            builder.AddFloat(value.y);
            builder.AddFloat(value.z);
        }

        void Add(FingerprintBuilder &builder, const std::span<const NavigationResolvedBakeProfile> profiles) noexcept {
            builder.AddU64(profiles.size());
            for (const auto &profile : profiles) {
                builder.AddU64(profile.id.Value());
                builder.AddFloat(profile.buildGeometry.radiusMeters);
                builder.AddFloat(profile.buildGeometry.heightMeters);
                builder.AddFloat(profile.buildGeometry.maxSlopeDegrees);
                builder.AddFloat(profile.buildGeometry.stepHeightMeters);
                builder.AddFloat(profile.buildGeometry.cellSizeMeters);
                builder.AddFloat(profile.buildGeometry.cellHeightMeters);
                builder.AddFloat(profile.buildGeometry.minimumRegionSizeMeters);
            }
        }

        void Add(FingerprintBuilder &builder, const std::span<const NavigationResolvedBakeArea> areas) noexcept {
            builder.AddU64(areas.size());
            for (const auto &area : areas) {
                builder.AddU64(area.id.Value());
                builder.AddU32(static_cast<std::uint32_t>(area.source.kind));
                builder.AddU64(area.source.id.Value());
                builder.AddFloat(area.traversalCost);
                builder.AddU64(area.flags.bits);
            }
        }

        void Add(FingerprintBuilder &builder, const std::span<const NavigationTileBuildPartition> partitions) noexcept {
            builder.AddU64(partitions.size());
            for (const auto &partition : partitions) {
                if (partition.scope.IsSceneScoped())
                    for (const auto byte : partition.scope.SceneBytes())
                        builder.AddU32(byte);
                builder.AddU64(partition.profile.Value());
                builder.AddU64(partition.surface.Value());
                builder.AddU64(partition.filter.Value());
                builder.AddU32(partition.firstTriangle);
                builder.AddU32(partition.triangleCount);
                builder.AddU32(partition.firstModifier);
                builder.AddU32(partition.modifierCount);
            }
        }

        void Add(FingerprintBuilder &builder, const std::span<const NavigationTileBuildTriangle> triangles) noexcept {
            builder.AddU64(triangles.size());
            for (const auto &triangle : triangles) {
                for (const auto vertex : triangle.vertices)
                    Add(builder, vertex);
                builder.AddU64(triangle.area.Value());
                builder.AddFloat(triangle.traversalCost);
                builder.AddU32(triangle.materialSlot.value);
                builder.AddU32(static_cast<std::uint32_t>(triangle.provenance.kind));
                builder.AddU64(triangle.provenance.producer.Value());
                builder.AddU64(triangle.provenance.contribution.Value());
                builder.AddU64(triangle.provenance.revision.Value());
                builder.AddDigest(triangle.provenance.contentDigest);
                builder.AddU32(triangle.provenance.sourceTriangleIndex);
            }
        }

        void Add(FingerprintBuilder &builder, const std::span<const NavigationTileBuildModifier> modifiers) noexcept {
            builder.AddU64(modifiers.size());
            for (const auto &modifier : modifiers) {
                if (modifier.scope.IsSceneScoped())
                    for (const auto byte : modifier.scope.SceneBytes())
                        builder.AddU32(byte);
                builder.AddU64(modifier.profile.Value());
                builder.AddU64(modifier.surface.Value());
                builder.AddU64(modifier.id.Value());
                builder.AddU32(static_cast<std::uint32_t>(modifier.mode));
                builder.AddU64(modifier.area.Value());
                Add(builder, modifier.canonicalBounds.minimum);
                Add(builder, modifier.canonicalBounds.maximum);
            }
        }
    }  // namespace

    /** @copydoc ComputeBakeInputFingerprint */
    Sha256Digest ComputeBakeInputFingerprint(const NavigationBakeInputRevisions &revisions,
                                             const std::span<const NavigationResolvedBakeProfile> profiles,
                                             const std::span<const NavigationResolvedBakeArea> areas,
                                             const std::span<const NavigationTileBuildPartition> partitions,
                                             const std::span<const NavigationTileBuildTriangle> triangles,
                                             const std::span<const NavigationTileBuildModifier> modifiers) noexcept {
        const bool scoped = std::ranges::any_of(partitions, [](const auto &partition) {
            return partition.scope.IsSceneScoped();
        });
        FingerprintBuilder builder{scoped};
        Add(builder, revisions);
        Add(builder, profiles);
        Add(builder, areas);
        Add(builder, partitions);
        Add(builder, triangles);
        Add(builder, modifiers);
        return builder.Finish();
    }
}  // namespace Horo::Navigation::Internal
