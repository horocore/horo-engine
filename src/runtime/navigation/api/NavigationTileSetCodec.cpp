#include "NavigationTileArtifactInternal.h"

namespace Horo::Navigation {
    namespace {
        constexpr std::uint32_t LegacySetMagic = 0x31534e48;      // HNS1
        constexpr std::uint32_t ProvenanceSetMagic = 0x32534e48;  // HNS2

        /** @brief Counts exact canonical partitions while rejecting invalid or reordered tile owners. */
        [[nodiscard]] std::size_t PartitionCount(const NavigationCookedTileSet &set) {
            const NavigationCookedTile *previous{};
            std::size_t count{};
            for (const auto &tile : set.tiles) {
                if (!tile || (previous && tile->Key() <= previous->Key()))
                    throw std::invalid_argument("Invalid navigation tile closure");
                if (!previous || previous->Key().profile != tile->Key().profile || previous->Key().surface != tile->Key().surface)
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
                if (!previous || previous->Key().profile != tile->Key().profile || previous->Key().surface != tile->Key().surface) {
                    writer.Integer(tile->Key().profile.Value(), 8);
                    writer.Integer(tile->Key().surface.Value(), 8);
                }
                previous = tile.get();
            }
        }

        /** @brief Reads fixed producer metadata before bounded closure/tile allocations. */
        [[nodiscard]] std::span<const std::uint8_t> ReadProvenance(TileArtifactInternal::Reader &reader, NavigationCookedTileSet &set) {
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
            const auto partitions = reader.Count(NavMeshArtifactLimits::MaximumTiles, 16);
            if (partitions == 0)
                throw std::invalid_argument("Empty navigation content partition closure");
            return reader.Raw(partitions * 16);
        }

        /** @brief Proves serialized release closure equals the actual decoded canonical tile partitions. */
        void VerifyPartitionClosure(const std::span<const std::uint8_t> bytes, const NavigationCookedTileSet &set) {
            TileArtifactInternal::Reader reader(bytes);
            const NavigationCookedTile *previous{};
            for (const auto &tile : set.tiles) {
                if (!previous || previous->Key().profile != tile->Key().profile || previous->Key().surface != tile->Key().surface) {
                    const auto profile = reader.Integer(8);
                    const auto surface = reader.Integer(8);
                    if (profile != tile->Key().profile.Value() || surface != tile->Key().surface.Value())
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
        if (!Present(set.inputFingerprint) || set.tiles.empty() || set.tiles.size() > NavMeshArtifactLimits::MaximumTiles ||
            maximumBytes == 0 || maximumBytes > NavMeshArtifactLimits::MaximumOwnedBytes)
            return Failure<std::vector<std::uint8_t>>(NavigationErrors::BakeInputInvalid);
        if (set.provenance && (!Present(set.provenance->compatibility.provider) || !Present(set.provenance->compatibility.schemas) ||
                               !Present(set.provenance->compatibility.settings)))
            return Failure<std::vector<std::uint8_t>>(NavigationErrors::BakeInputInvalid);
        try {
            Writer writer(maximumBytes);
            writer.Integer(set.provenance ? ProvenanceSetMagic : LegacySetMagic);
            writer.Digest(set.inputFingerprint);
            if (set.provenance)
                WriteProvenance(writer, set);
            writer.Integer(set.tiles.size());
            const NavigationCookedTile *previous{};
            for (const auto &tile : set.tiles) {
                if (!tile || (previous != nullptr && tile->Key() <= previous->Key()))
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
            if (magic != LegacySetMagic && magic != ProvenanceSetMagic) {
                const auto &error = (magic & 0x00ffffffU) == (LegacySetMagic & 0x00ffffffU) ? NavigationErrors::UnsupportedCookedVersion
                                                                                            : NavigationErrors::NavMeshArtifactCorrupt;
                return Failure<NavigationCookedTileSet>(error);
            }
            NavigationCookedTileSet set{.inputFingerprint = reader.Digest()};
            const auto closure = magic == ProvenanceSetMagic ? ReadProvenance(reader, set) : std::span<const std::uint8_t>{};
            const auto count = reader.Count(NavMeshArtifactLimits::MaximumTiles, 40);
            if (count == 0 || !Present(set.inputFingerprint))
                return Failure<NavigationCookedTileSet>(NavigationErrors::NavMeshArtifactCorrupt);
            set.tiles.reserve(count);
            std::size_t storage = sizeof(set) + set.tiles.capacity() * sizeof(std::shared_ptr<const NavigationCookedTile>);
            if (storage > maximumBytes)
                return Failure<NavigationCookedTileSet>(NavigationErrors::CapacityExceeded);
            for (std::size_t i = 0; i < count; ++i) {
                const auto content = reader.Digest();
                const auto size = reader.Integer(8);
                if (size > maximumBytes)
                    return Failure<NavigationCookedTileSet>(NavigationErrors::CapacityExceeded);
                const auto tileBytes = reader.Raw(static_cast<std::size_t>(size));
                if (ComputeSha256(std::as_bytes(tileBytes)) != content)
                    return Failure<NavigationCookedTileSet>(NavigationErrors::NavMeshArtifactCorrupt);
                auto tile = NavigationCookedTile::Decode(tileBytes);
                if (tile.HasError())
                    return Result<NavigationCookedTileSet>::Failure(tile.ErrorValue());
                if (!set.tiles.empty() && set.tiles.back()->Key() >= tile.Value()->Key())
                    return Failure<NavigationCookedTileSet>(NavigationErrors::NavMeshArtifactCorrupt);
                if (tile.Value()->StorageBytes() > maximumBytes - storage)
                    return Failure<NavigationCookedTileSet>(NavigationErrors::CapacityExceeded);
                storage += tile.Value()->StorageBytes();
                set.tiles.push_back(std::move(tile).Value());
            }
            if (!reader.Empty())
                return Failure<NavigationCookedTileSet>(NavigationErrors::NavMeshArtifactCorrupt);
            if (set.provenance)
                VerifyPartitionClosure(closure, set);
            return Result<NavigationCookedTileSet>::Success(std::move(set));
        } catch (const std::invalid_argument &) {
            return Failure<NavigationCookedTileSet>(NavigationErrors::NavMeshArtifactCorrupt);
        } catch (const std::bad_alloc &) {
            return Failure<NavigationCookedTileSet>(NavigationErrors::CapacityExceeded);
        }
    }
}  // namespace Horo::Navigation
