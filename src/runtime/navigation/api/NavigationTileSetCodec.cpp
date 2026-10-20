#include "NavigationTileArtifactInternal.h"

namespace Horo::Navigation {
    namespace {
        constexpr std::uint32_t LegacySetMagic = 0x31534e48;      // HNS1
        constexpr std::uint32_t ProvenanceSetMagic = 0x32534e48;  // HNS2
        constexpr std::uint32_t EmptySetMagic = 0x33534e48;       // HNS3: complete empty closure with producer authority

        constexpr std::uint32_t ScopedSetMagic = 0x34534e48;  // HNS4: exact Scene-qualified partitions

        /** @brief Compares the complete coordinate-space and authored partition identity. */
        bool SamePartition(const NavigationBakeTileKey &left, const NavigationBakeTileKey &right) {
            return left.scope == right.scope && left.profile == right.profile && left.surface == right.surface;
        }

        /** @brief Counts exact canonical partitions while rejecting invalid or reordered tile owners. */
        [[nodiscard]] std::size_t PartitionCount(const NavigationCookedTileSet &set) {
            const NavigationCookedTile *previous{};
            std::size_t count{};
            for (const auto &tile : set.tiles) {
                if (!tile || (previous && tile->Key() <= previous->Key()))
                    throw std::invalid_argument("Invalid navigation tile closure");
                if (!previous || !SamePartition(previous->Key(), tile->Key()))
                    ++count;
                previous = tile.get();
            }
            return count;
        }

        /** @brief Writes producer compatibility, optional owned policy and closure derived from validated tiles. */
        void WriteProvenance(TileArtifactInternal::Writer &writer, const NavigationCookedTileSet &set) {
            const auto &provenance = *set.provenance;
            writer.Digest(provenance.compatibility.provider);
            writer.Digest(provenance.compatibility.schemas);
            writer.Digest(provenance.compatibility.settings);
            writer.Integer(provenance.projectProfile.has_value() ? 1 : 0, 1);
            if (provenance.projectProfile)
                TileArtifactInternal::WriteContentProfile(writer, *provenance.projectProfile);
            writer.Integer(PartitionCount(set));
            const NavigationCookedTile *previous{};
            for (const auto &tile : set.tiles) {
                if (!previous || !SamePartition(previous->Key(), tile->Key())) {
                    if (tile->Key().scope.IsSceneScoped())
                        writer.Raw(tile->Key().scope.SceneBytes());
                    writer.Integer(tile->Key().profile.Value(), 8);
                    writer.Integer(tile->Key().surface.Value(), 8);
                }
                previous = tile.get();
            }
        }

        /** @brief Reads fixed producer metadata before bounded closure/tile allocations. */
        [[nodiscard]] std::span<const std::uint8_t> ReadProvenance(TileArtifactInternal::Reader &reader, NavigationCookedTileSet &set,
                                                                   bool emptyClosure, bool scoped) {
            using namespace TileArtifactInternal;
            NavigationCookedContentProvenance provenance;
            provenance.compatibility = {reader.Digest(), reader.Digest(), reader.Digest()};
            if (!Present(provenance.compatibility.provider) || !Present(provenance.compatibility.schemas) ||
                !Present(provenance.compatibility.settings))
                throw std::invalid_argument("Incomplete navigation content compatibility");
            const auto policy = reader.Integer(1);
            if (policy > 1)
                throw std::invalid_argument("Unknown navigation content policy marker");
            if (policy == 1)
                provenance.projectProfile.emplace(ReadContentProfile(reader));
            set.provenance.emplace(std::move(provenance));
            const auto partitions = reader.Count(NavMeshArtifactLimits::MaximumTiles, scoped ? 32 : 16);
            if ((partitions == 0) != emptyClosure)
                throw std::invalid_argument("Navigation content partition closure disagrees with its format");
            return reader.Raw(partitions * (scoped ? 32 : 16));
        }

        /** @brief Decode canonical ordered tile ownership while charging every retained allocation to the byte ceiling. */
        [[nodiscard]] Result<void> ReadTiles(TileArtifactInternal::Reader &reader, NavigationCookedTileSet &set, const std::size_t count,
                                             const std::size_t maximumBytes) {
            using namespace TileArtifactInternal;
            set.tiles.reserve(count);
            std::size_t storage = sizeof(set) + set.tiles.capacity() * sizeof(std::shared_ptr<const NavigationCookedTile>);
            if (storage > maximumBytes)
                return Failure<void>(NavigationErrors::CapacityExceeded);
            for (std::size_t i = 0; i < count; ++i) {
                const auto content = reader.Digest();
                const auto size = reader.Integer(8);
                if (size > maximumBytes)
                    return Failure<void>(NavigationErrors::CapacityExceeded);
                const auto tileBytes = reader.Raw(static_cast<std::size_t>(size));
                if (ComputeSha256(std::as_bytes(tileBytes)) != content)
                    return Failure<void>(NavigationErrors::NavMeshArtifactCorrupt);
                auto tile = NavigationCookedTile::Decode(tileBytes);
                if (tile.HasError())
                    return Result<void>::Failure(tile.ErrorValue());
                if (!set.tiles.empty() && set.tiles.back()->Key() >= tile.Value()->Key())
                    return Failure<void>(NavigationErrors::NavMeshArtifactCorrupt);
                if (tile.Value()->StorageBytes() > maximumBytes - storage)
                    return Failure<void>(NavigationErrors::CapacityExceeded);
                storage += tile.Value()->StorageBytes();
                set.tiles.push_back(std::move(tile).Value());
            }
            return Result<void>::Success();
        }

        /** @brief Proves serialized release closure equals the actual decoded canonical tile partitions. */
        void VerifyPartitionClosure(const std::span<const std::uint8_t> bytes, const NavigationCookedTileSet &set, bool scoped) {
            TileArtifactInternal::Reader reader(bytes);
            const NavigationCookedTile *previous{};
            for (const auto &tile : set.tiles) {
                if (!previous || !SamePartition(previous->Key(), tile->Key())) {
                    const auto scope = scoped ? TileArtifactInternal::ReadSceneScope(reader) : NavigationBakeScopeId{};
                    const auto profile = reader.Integer(8);
                    const auto surface = reader.Integer(8);
                    if (scope != tile->Key().scope || profile != tile->Key().profile.Value() || surface != tile->Key().surface.Value())
                        throw std::invalid_argument("Navigation content partition closure mismatch");
                }
                previous = tile.get();
            }
            if (!reader.Empty())
                throw std::invalid_argument("Extra navigation content partition closure");
        }
    }  // namespace

    /** @copydoc EncodeNavigationCookedTileSet */
    Result<std::vector<std::uint8_t>> EncodeNavigationCookedTileSet(const NavigationCookedTileSet &set, const std::size_t maximumBytes) {
        using namespace TileArtifactInternal;
        if (!Present(set.inputFingerprint) || (set.tiles.empty() && !set.provenance) ||
            set.tiles.size() > NavMeshArtifactLimits::MaximumTiles || maximumBytes == 0 ||
            maximumBytes > NavMeshArtifactLimits::MaximumOwnedBytes)
            return Failure<std::vector<std::uint8_t>>(NavigationErrors::BakeInputInvalid);
        if (set.provenance && (!Present(set.provenance->compatibility.provider) || !Present(set.provenance->compatibility.schemas) ||
                               !Present(set.provenance->compatibility.settings)))
            return Failure<std::vector<std::uint8_t>>(NavigationErrors::BakeInputInvalid);
        const bool scoped = !set.tiles.empty() && set.tiles.front() && set.tiles.front()->Key().scope.IsSceneScoped();
        if (scoped && !set.provenance)
            return Failure<std::vector<std::uint8_t>>(NavigationErrors::BakeInputInvalid);
        try {
            Writer writer(maximumBytes);
            auto provenanceMagic = set.tiles.empty() ? EmptySetMagic : ProvenanceSetMagic;
            if (scoped)
                provenanceMagic = ScopedSetMagic;
            writer.Integer(set.provenance ? provenanceMagic : LegacySetMagic);
            writer.Digest(set.inputFingerprint);
            if (set.provenance)
                WriteProvenance(writer, set);
            writer.Integer(set.tiles.size());
            const NavigationCookedTile *previous{};
            for (const auto &tile : set.tiles) {
                if (!tile || tile->Key().scope.IsSceneScoped() != scoped || (previous != nullptr && tile->Key() <= previous->Key()))
                    return Failure<std::vector<std::uint8_t>>(NavigationErrors::BakeInputInvalid);
                writer.Digest(tile->ContentIdentity());
                writer.Integer(tile->Bytes().size(), 8);
                writer.Raw(tile->Bytes());
                previous = tile.get();
            }
            return Result<std::vector<std::uint8_t>>::Success(std::move(writer).TakeBytes());
        } catch (const std::invalid_argument &) {
            return Failure<std::vector<std::uint8_t>>(NavigationErrors::BakeInputInvalid);
        } catch (const std::length_error &) {
            return Failure<std::vector<std::uint8_t>>(NavigationErrors::CapacityExceeded);
        } catch (const std::bad_alloc &) {
            return Failure<std::vector<std::uint8_t>>(NavigationErrors::CapacityExceeded);
        }
    }

    /** @copydoc DecodeNavigationCookedTileSet */
    Result<NavigationCookedTileSet> DecodeNavigationCookedTileSet(const std::span<const std::uint8_t> bytes,
                                                                  const std::size_t maximumBytes) {
        using namespace TileArtifactInternal;
        if (bytes.size() > maximumBytes || maximumBytes == 0 || maximumBytes > NavMeshArtifactLimits::MaximumOwnedBytes)
            return Failure<NavigationCookedTileSet>(NavigationErrors::CapacityExceeded);
        try {
            Reader reader(bytes);
            const auto magic = reader.Integer();
            if (magic != LegacySetMagic && magic != ProvenanceSetMagic && magic != EmptySetMagic && magic != ScopedSetMagic) {
                const auto &error = (magic & 0x00ffffffU) == (LegacySetMagic & 0x00ffffffU) ? NavigationErrors::UnsupportedCookedVersion
                                                                                            : NavigationErrors::NavMeshArtifactCorrupt;
                return Failure<NavigationCookedTileSet>(error);
            }
            NavigationCookedTileSet set{.inputFingerprint = reader.Digest()};
            const auto closure = magic != LegacySetMagic ? ReadProvenance(reader, set, magic == EmptySetMagic, magic == ScopedSetMagic)
                                                         : std::span<const std::uint8_t>{};
            const auto count = reader.Count(NavMeshArtifactLimits::MaximumTiles, 40);
            if (((count == 0) != (magic == EmptySetMagic)) || !Present(set.inputFingerprint))
                return Failure<NavigationCookedTileSet>(NavigationErrors::NavMeshArtifactCorrupt);
            if (const auto tiles = ReadTiles(reader, set, count, maximumBytes); tiles.HasError())
                return Result<NavigationCookedTileSet>::Failure(tiles.ErrorValue());
            if (!reader.Empty() || std::ranges::any_of(set.tiles, [magic](const auto &tile) {
                return tile->Key().scope.IsSceneScoped() != (magic == ScopedSetMagic);
            }))
                return Failure<NavigationCookedTileSet>(NavigationErrors::NavMeshArtifactCorrupt);
            if (set.provenance)
                VerifyPartitionClosure(closure, set, magic == ScopedSetMagic);
            return Result<NavigationCookedTileSet>::Success(std::move(set));
        } catch (const std::invalid_argument &) {
            return Failure<NavigationCookedTileSet>(NavigationErrors::NavMeshArtifactCorrupt);
        } catch (const std::bad_alloc &) {
            return Failure<NavigationCookedTileSet>(NavigationErrors::CapacityExceeded);
        }
    }
}  // namespace Horo::Navigation
