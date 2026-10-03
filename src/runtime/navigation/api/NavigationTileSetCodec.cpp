#include "NavigationTileArtifactInternal.h"

namespace Horo::Navigation {
    namespace {
        constexpr std::uint32_t SetMagic = 0x31534e48;  // HNS1
    }

    /** @copydoc EncodeNavigationCookedTileSet */
    Result<std::vector<std::uint8_t>> EncodeNavigationCookedTileSet(const NavigationCookedTileSet &set, const std::size_t maximumBytes) {
        using namespace TileArtifactInternal;
        if (!Present(set.inputFingerprint) || set.tiles.empty() || set.tiles.size() > NavMeshArtifactLimits::MaximumTiles ||
            maximumBytes == 0 || maximumBytes > NavMeshArtifactLimits::MaximumOwnedBytes)
            return Failure<std::vector<std::uint8_t>>(NavigationErrors::BakeInputInvalid);
        try {
            Writer writer(maximumBytes);
            writer.Integer(SetMagic);
            writer.Digest(set.inputFingerprint);
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
            if (reader.Integer() != SetMagic)
                return Failure<NavigationCookedTileSet>(NavigationErrors::NavMeshArtifactCorrupt);
            NavigationCookedTileSet set{.inputFingerprint = reader.Digest()};
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
            return Result<NavigationCookedTileSet>::Success(std::move(set));
        } catch (const std::invalid_argument &) {
            return Failure<NavigationCookedTileSet>(NavigationErrors::NavMeshArtifactCorrupt);
        } catch (const std::bad_alloc &) {
            return Failure<NavigationCookedTileSet>(NavigationErrors::CapacityExceeded);
        }
    }
}  // namespace Horo::Navigation
