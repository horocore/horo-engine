#pragma once

/** @file WorldIndexArtifact.h
 * @brief Canonical ADR-023 world.index encoding and bounded inert decoding.
 */
#include "Horo/Foundation/CancellationToken.h"
#include "Horo/WorldStreaming/CookedWorldIndexManifest.h"

#include <array>

namespace Horo::WorldStreaming {
    /** @brief Coordinate convention encoded by ADR-023 without selecting a runtime backend. */
    enum class WorldIndexCoordinateSystem : std::uint8_t {
        RightHandedYUp,
        RightHandedZUp,
        LeftHandedYUp,
        LeftHandedZUp
    };
    /** @brief Static relevance-volume category; runtime activation remains outside this artifact. */
    enum class WorldIndexVolumeKind : std::uint8_t {
        Camera,
        Gameplay,
        NetworkRelevance,
        Preload
    };
    /** @brief Version-one inert volume envelope representation. */
    enum class WorldIndexVolumeShape : std::uint8_t {
        Sphere,
        Box,
        ConvexHull
    };

    /** @brief Complete ADR-023 static volume row in logical world-meter units. */
    struct WorldIndexVolume final {
        std::uint32_t id{};
        WorldIndexVolumeKind kind{};
        WorldIndexVolumeShape shape{};
        bool unloadOutside{};
        std::array<float, 3> origin{};
        std::array<float, 3> extents{};
        float priority{1.0F};
        std::uint32_t layerMask{}; /**< Bits address canonical layer ordinals, never layer IDs. */
        bool operator==(const WorldIndexVolume &) const noexcept = default;
    };

    /** @brief Mandatory artifact/table ceilings applied before copying external records. */
    struct WorldIndexArtifactLimits final {
        std::size_t maximumArtifactBytes{};
        std::uint32_t maximumStringBytes{};
        std::uint32_t maximumVolumes{};
        WorldPartitionDescriptorLimits partition;
        CookedWorldIndexManifestLimits cooked;
    };

    /** @brief Complete canonical index bytes and two explicitly distinct integrity identities. */
    struct EncodedWorldIndex final {
        std::vector<std::byte> bytes;
        Sha256Digest manifestHash; /**< ADR-023 digest with the embedded hash field zeroed. */
        Sha256Digest artifactHash; /**< SHA-256 of the final complete artifact, binding sibling dependency data. */
    };

    /** @brief Owned validated index projection; hard-edge reconstruction requires its matching dependency bundle. */
    struct DecodedWorldIndex final {
        WorldPartitionDescriptor descriptor;
        std::string name;
        WorldIndexCoordinateSystem coordinates{};
        std::vector<WorldIndexVolume> volumes;
        std::vector<CookedWorldCellManifestEntry> cells;
        Sha256Digest manifestHash;
        Sha256Digest artifactHash;
    };

    /**
     * @brief Encodes an immutable cooked manifest without changing ADR-023 record widths.
     * @param manifest Sole validated cell/topology authority; hard edges are encoded by the matching dependency bundle.
     * @param name World name normalized to NFC before canonical hashing.
     * @param coordinates Explicit world convention retained in the artifact.
     * @param volumes Complete static-volume metadata, canonically sorted by ID.
     * @param limits Mandatory count and byte bounds.
     * @param cancellation Borrowed observer checked while copying and hashing.
     * @return Complete canonical artifact or typed malformed/capacity/unsupported/cancelled failure; no external publication.
     * @details Millimeter coordinates must round trip exactly through the version-one float-meter representation.
     * Unrepresentable coordinates are rejected rather than rounded into a different world.
     * @throws std::bad_alloc on bounded ownership allocation.
     */
    [[nodiscard]] Result<EncodedWorldIndex> EncodeWorldIndexArtifact(const CookedWorldIndexManifest &manifest, std::string_view name,
                                                                     WorldIndexCoordinateSystem coordinates,
                                                                     std::span<const WorldIndexVolume> volumes,
                                                                     const WorldIndexArtifactLimits &limits,
                                                                     const CancellationToken &cancellation = {});
    /**
     * @brief Decodes and validates complete canonical bytes before exposing an inert owned projection.
     * @param bytes Exact world.index bytes; no borrowed source storage survives success.
     * @param limits Mandatory bounds checked before allocations.
     * @param cancellation Borrowed observer checked throughout decoding and hashing.
     * @return Owned immutable topology and matching integrity facts, or typed failure without partial publication.
     * @details Validates versions, exact packed offsets, reserved bytes, canonical NFC names/order, positive zero,
     * aggregate sizes, cell/package identities and both hashes. This does not activate cells or admit package coverage.
     * @throws std::bad_alloc on bounded ownership allocation.
     */
    [[nodiscard]] Result<DecodedWorldIndex> DecodeWorldIndexArtifact(std::span<const std::byte> bytes,
                                                                     const WorldIndexArtifactLimits &limits,
                                                                     const CancellationToken &cancellation = {});
}  // namespace Horo::WorldStreaming
