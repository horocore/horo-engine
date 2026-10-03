#include "Horo/Navigation/NavigationTileDescriptor.h"

#include "NavigationTileArtifactInternal.h"

namespace Horo::Navigation {
    /** @copydoc ProjectNavigationCookedTileDescriptor */
    Result<NavigationCookedTileDescriptor> ProjectNavigationCookedTileDescriptor(const NavigationCookedTile &tile) {
        try {
            TileArtifactInternal::Reader reader(tile.Bytes());
            // Version, identities, grid address and bounds have already passed the canonical tile factory.
            static_cast<void>(reader.Raw(sizeof(std::uint32_t) + 2 * sizeof(std::uint64_t) + 2 * sizeof(std::int32_t) +
                                         sizeof(std::uint16_t) + 6 * sizeof(float)));
            NavigationCookedTileDescriptor descriptor;
            descriptor.tileSizeMeters = reader.Float();
            descriptor.geometry = {reader.Float(), reader.Float(), reader.Float(), reader.Float(),
                                   reader.Float(), reader.Float(), reader.Float()};
            descriptor.borderSizeCells = static_cast<std::uint32_t>(reader.Integer());
            return Result<NavigationCookedTileDescriptor>::Success(descriptor);
        } catch (const std::invalid_argument &) {
            return TileArtifactInternal::Failure<NavigationCookedTileDescriptor>(NavigationErrors::NavMeshArtifactCorrupt);
        }
    }
}  // namespace Horo::Navigation
