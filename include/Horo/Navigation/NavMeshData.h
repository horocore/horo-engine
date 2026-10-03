#pragma once

/**
 * @file NavMeshData.h
 * @brief Versioned, bounded and provider-neutral cooked grounded NavMesh data.
 */

#include "Horo/Foundation/Result.h"
#include "Horo/Foundation/Sha256.h"
#include "Horo/Math/SceneMath.h"
#include "Horo/Math/WorldCoordinate64.h"
#include "Horo/Navigation/NavigationAgentProfiles.h"
#include "Horo/Navigation/NavigationAreas.h"
#include "Horo/Navigation/NavigationSourceGeometry.h"

#include <compare>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

namespace Horo::Navigation {
    /** @brief Independently versioned neutral NavMesh payload format. */
    struct NavMeshFormatVersion final {
        std::uint16_t major{1}; /**< Breaking neutral schema version. */
        std::uint16_t minor{};  /**< Backward-compatible neutral schema revision. */

        [[nodiscard]] constexpr auto operator<=>(const NavMeshFormatVersion &) const noexcept = default;
    };

    /** @brief Exact neutral NavMesh schema understood by this runtime. */
    inline constexpr NavMeshFormatVersion CurrentNavMeshFormatVersion{1, 0};

    /** @brief Byte order declared by a neutral or provider-private encoded payload. */
    enum class NavMeshByteOrder : std::uint8_t {
        LittleEndian,
        BigEndian,
        Count,
    };

    /** @brief Compression applied to one encoded neutral or provider-private payload. */
    enum class NavMeshCompression : std::uint8_t {
        None,
        Lz4,
        Zstandard,
        Count,
    };

    /** @brief Closed coordinate representation admitted by the grounded navigation runtime. */
    enum class NavMeshCoordinateFrame : std::uint8_t {
        RightHandedYUpMeters,
        Count,
    };

    /** @brief Exact profile provenance semantic evidence retained by one cooked output. */
    struct NavMeshProfileDescriptor final {
        NavigationAgentProfileId id;                  /**< Stable authored profile identity. */
        NavigationAgentBuildGeometry buildGeometry{}; /**< Exact grounded dimensions and bake resolution. */
        Sha256Digest contentDigest{};                 /**< Digest of exact profile and traversal settings. */

        /** @brief Compares exact profile identity, build geometry and digest. @param other Profile to compare. @return True on equality. */
        [[nodiscard]] bool operator==(const NavMeshProfileDescriptor &other) const noexcept;
    };

    /** @brief Canonical global origin and metric tile grid used by every artifact coordinate. */
    struct NavMeshCoordinateFrameDescriptor final {
        NavMeshCoordinateFrame representation{NavMeshCoordinateFrame::RightHandedYUpMeters};
        Math::WorldCoordinate64 origin{}; /**< Exact global origin for local float coordinates. */
        float tileSizeMeters{};           /**< Positive finite horizontal tile edge length. */

        [[nodiscard]] auto operator<=>(const NavMeshCoordinateFrameDescriptor &) const noexcept = default;
    };

    /** @brief Half-open range into one decoded artifact table. */
    struct NavMeshTableRange final {
        std::uint32_t first{}; /**< First table row. */
        std::uint32_t count{}; /**< Number of rows in the range. */

        [[nodiscard]] constexpr auto operator<=>(const NavMeshTableRange &) const noexcept = default;
    };

    /** @brief Sentinel stored for a polygon edge that has no neighboring polygon. */
    inline constexpr std::uint32_t NavMeshBoundaryAdjacency = std::numeric_limits<std::uint32_t>::max();

    /** @brief Stable address for one independently validated horizontal tile layer. */
    struct NavMeshTileKey final {
        std::int32_t x{};      /**< Horizontal X tile coordinate. */
        std::int32_t z{};      /**< Horizontal Z tile coordinate. */
        std::uint16_t layer{}; /**< Vertical layer at the X/Z coordinate. */

        [[nodiscard]] constexpr auto operator<=>(const NavMeshTileKey &) const noexcept = default;
    };

    /** @brief Hard qualified ceilings and caller-selected lower decode limits. */
    struct NavMeshArtifactLimits final {
        static constexpr std::uint32_t MaximumTiles = 65'536;
        static constexpr std::uint32_t MaximumVertices = 4'000'000;
        static constexpr std::uint32_t MaximumPolygons = 2'000'000;
        static constexpr std::uint32_t MaximumPolygonVertexIndices = 12'000'000;
        static constexpr std::uint32_t MaximumPolygonAdjacencies = 12'000'000;
        static constexpr std::uint32_t MaximumOffMeshLinks = 1'000'000;
        static constexpr std::uint32_t MaximumProvenanceRows = 2'000'000;
        static constexpr std::uint32_t MaximumProviderPayloads = 65'536;
        static constexpr std::uint32_t MaximumVerticesPerPolygon = 64;
        static constexpr std::uint64_t MaximumPortableEncodedBytes = 1024ULL * 1024ULL * 1024ULL;
        static constexpr std::uint64_t MaximumPortableDecodedBytes = 1024ULL * 1024ULL * 1024ULL;
        static constexpr std::uint64_t MaximumProviderEncodedBytes = 1024ULL * 1024ULL * 1024ULL;
        static constexpr std::uint64_t MaximumProviderDecodedBytes = 2ULL * 1024ULL * 1024ULL * 1024ULL;
        static constexpr std::uint64_t MaximumOwnedBytes = 2ULL * 1024ULL * 1024ULL * 1024ULL;

        std::uint32_t maxTiles{MaximumTiles};
        std::uint32_t maxVertices{MaximumVertices};
        std::uint32_t maxPolygons{MaximumPolygons};
        std::uint32_t maxPolygonVertexIndices{MaximumPolygonVertexIndices};
        std::uint32_t maxPolygonAdjacencies{MaximumPolygonAdjacencies};
        std::uint32_t maxOffMeshLinks{MaximumOffMeshLinks};
        std::uint32_t maxProvenanceRows{MaximumProvenanceRows};
        std::uint32_t maxProviderPayloads{MaximumProviderPayloads};
        std::uint32_t maxVerticesPerPolygon{MaximumVerticesPerPolygon};
        std::uint64_t maxPortableEncodedBytes{MaximumPortableEncodedBytes};
        std::uint64_t maxPortableDecodedBytes{MaximumPortableDecodedBytes};
        std::uint64_t maxProviderEncodedBytes{MaximumProviderEncodedBytes};
        std::uint64_t maxProviderDecodedBytes{MaximumProviderDecodedBytes};
        std::uint64_t maxOwnedBytes{MaximumOwnedBytes};

        [[nodiscard]] constexpr auto operator<=>(const NavMeshArtifactLimits &) const noexcept = default;
    };

    /** @brief Scalar facts parsed from the fixed artifact header before any table allocation. */
    struct NavMeshArtifactHeader final {
        NavMeshFormatVersion formatVersion{CurrentNavMeshFormatVersion}; /**< Independent cooked schema version. */
        NavMeshByteOrder byteOrder{NavMeshByteOrder::LittleEndian};      /**< Neutral table byte order. */
        NavMeshCompression compression{NavMeshCompression::None};        /**< Neutral table compression. */
        NavMeshCoordinateFrameDescriptor coordinateFrame{};              /**< Exact canonical origin and metric grid. */
        NavMeshProfileDescriptor profile{};                              /**< Exact grounded bake-profile evidence. */
        std::uint32_t tileCount{};
        std::uint32_t vertexCount{};
        std::uint32_t polygonCount{};
        std::uint32_t polygonVertexIndexCount{};
        std::uint32_t polygonAdjacencyCount{};
        std::uint32_t offMeshLinkCount{};
        std::uint32_t provenanceCount{};
        std::uint32_t providerPayloadCount{};
        std::uint64_t portableEncodedBytes{}; /**< Encoded neutral payload bytes in the asset envelope. */
        std::uint64_t portableDecodedBytes{}; /**< Decoded neutral payload bytes admitted before table allocation. */
        std::uint64_t providerEncodedBytes{}; /**< Complete opaque provider byte table size. */
        std::uint64_t providerDecodedBytes{}; /**< Aggregate provider-declared decoded size. */
        Sha256Digest payloadDigest{};         /**< Digest of the exact encoded artifact payload. */

        [[nodiscard]] bool operator==(const NavMeshArtifactHeader &) const noexcept = default;
    };

    /** @brief One grounded convex polygon addressing canonical vertices and neighboring polygons. */
    struct NavMeshPolygon final {
        NavMeshTableRange vertexIndices; /**< Range into the polygon-vertex-index table. */
        NavMeshTableRange adjacencies;   /**< Range into the polygon-adjacency table. */
        NavigationAreaId area;           /**< Stable Horo area identity, never a provider flag. */

        [[nodiscard]] constexpr auto operator<=>(const NavMeshPolygon &) const noexcept = default;
    };

    /** @brief Provider-neutral generated grounded transition between two exact polygons. */
    struct NavMeshOffMeshLink final {
        Math::Vec3 start{};           /**< Canonical-metre start point. */
        Math::Vec3 end{};             /**< Canonical-metre destination point. */
        float radiusMeters{};         /**< Positive finite endpoint acceptance radius. */
        std::uint32_t startPolygon{}; /**< Global polygon index owned by the declaring tile. */
        std::uint32_t endPolygon{};   /**< Global destination polygon index. */
        NavigationAreaId area;        /**< Stable traversal-area semantics. */
        bool bidirectional{};         /**< Whether traversal is admitted in both directions. */

        [[nodiscard]] constexpr auto operator<=>(const NavMeshOffMeshLink &) const noexcept = default;
    };

    /** @brief Stable source evidence for one contiguous generated polygon range. */
    struct NavMeshSourceProvenance final {
        NavigationSourceProducerKind kind{NavigationSourceProducerKind::StaticCollider};
        NavigationSourceProducerId producer;
        NavigationSourceContributionId contribution;
        NavigationSourceRevision revision;
        Sha256Digest sourceDigest{};
        NavMeshTableRange polygons;

        [[nodiscard]] auto operator<=>(const NavMeshSourceProvenance &) const noexcept = default;
    };

    /** @brief Opaque provider section metadata; bytes never expose native structs through this API. */
    struct NavMeshProviderPayloadDescriptor final {
        Sha256Digest providerFingerprint{}; /**< Exact provider/build compatibility fingerprint. */
        std::uint32_t formatVersion{};      /**< Non-zero provider-owned payload version. */
        NavMeshByteOrder byteOrder{NavMeshByteOrder::LittleEndian};
        NavMeshCompression compression{NavMeshCompression::None};
        std::uint64_t byteOffset{};   /**< Offset into the artifact's opaque provider byte table. */
        std::uint64_t encodedBytes{}; /**< Encoded byte count. */
        std::uint64_t decodedBytes{}; /**< Bounded decoded byte count. */
        Sha256Digest payloadDigest{}; /**< Digest of this exact encoded provider section. */

        [[nodiscard]] auto operator<=>(const NavMeshProviderPayloadDescriptor &) const noexcept = default;
    };

    /** @brief Canonical table ownership and integrity facts for one independently addressable tile. */
    struct NavMeshTileDescriptor final {
        NavMeshTileKey key;
        Math::Aabb bounds{};
        NavMeshTableRange vertices;
        NavMeshTableRange polygons;
        NavMeshTableRange polygonVertexIndices;
        NavMeshTableRange polygonAdjacencies;
        NavMeshTableRange offMeshLinks;
        NavMeshTableRange provenance;
        NavMeshTableRange providerPayloads;
        Sha256Digest payloadDigest{}; /**< Digest of this exact encoded neutral tile payload. */
    };

    /** @brief Borrowed provider-neutral decoded tables shared by artifact and exact-tile views. */
    struct NavMeshDecodedTablesView final {
        std::span<const Math::Vec3> vertices;
        std::span<const NavMeshPolygon> polygons;
        std::span<const std::uint32_t> polygonVertexIndices;
        std::span<const std::uint32_t> polygonAdjacencies;
        std::span<const NavMeshOffMeshLink> offMeshLinks;
        std::span<const NavMeshSourceProvenance> provenance;
        std::span<const NavMeshProviderPayloadDescriptor> providerPayloads;
    };

    /** @brief Borrowed decoded tables and parser-observed digests submitted for bounded validation and ownership. */
    struct NavMeshArtifactView final {
        NavMeshArtifactHeader header;
        Sha256Digest observedPayloadDigest{}; /**< Digest computed from exact encoded bytes before decode. */
        std::span<const NavMeshTileDescriptor> tiles;
        std::span<const Sha256Digest> observedTilePayloadDigests; /**< Actual encoded digest for every tile row. */
        NavMeshDecodedTablesView tables;
        std::span<const std::byte> providerPayloadBytes;
    };

    /** @brief Exact provider-private format requested by a composed provider. */
    struct NavMeshProviderPayloadCompatibility final {
        Sha256Digest providerFingerprint{};
        std::uint32_t formatVersion{};
        NavMeshByteOrder byteOrder{NavMeshByteOrder::LittleEndian};
        NavMeshCompression compression{NavMeshCompression::None};

        [[nodiscard]] auto operator<=>(const NavMeshProviderPayloadCompatibility &) const noexcept = default;
    };

    /** @brief Borrowed independently addressable neutral tile tables. */
    struct NavMeshTileView final {
        const NavMeshTileDescriptor *descriptor{};
        NavMeshDecodedTablesView tables;
    };

    /** @brief Borrowed opaque provider bytes paired with their validated portable descriptor. */
    struct NavMeshProviderPayloadView final {
        const NavMeshProviderPayloadDescriptor *descriptor{};
        std::span<const std::byte> bytes;
    };

    /**
     * @brief Preflights fixed-header counts and sizes without allocating decoded tables.
     * @param header Scalar header facts parsed from bounded fixed storage.
     * @param limits Positive qualified ceilings, no greater than hard limits.
     * @return Success or a typed invalid, unsupported-version, or capacity failure.
     */
    [[nodiscard]] Result<void> ValidateNavMeshArtifactHeader(const NavMeshArtifactHeader &header, const NavMeshArtifactLimits &limits = {});

    /**
     * @brief Validates one tile and every table range it owns without depending on other tile validity.
     * @param artifact Complete borrowed table view whose selected tile ranges are inspected.
     * @param tileIndex Exact tile descriptor index.
     * @param limits Qualified per-polygon and provider decode ceilings.
     * @return Success or a typed corrupt, unsupported-version, or capacity failure.
     */
    [[nodiscard]] Result<void> ValidateNavMeshTile(const NavMeshArtifactView &artifact, std::size_t tileIndex,
                                                   const NavMeshArtifactLimits &limits = {});

    /** @brief Validate complete decoded tables without allocating an owning copy.
     * @param artifact Borrowed tables and parser-observed digest evidence.
     * @param limits Qualified schema and ownership ceilings.
     * @return The same typed validation result used by NavMeshData::Create. */
    [[nodiscard]] Result<void> ValidateNavMeshArtifact(const NavMeshArtifactView &artifact, const NavMeshArtifactLimits &limits = {});

    /** @brief Immutable, move-only provider-neutral cooked NavMesh artifact. */
    class NavMeshData final {
    public:
        NavMeshData(const NavMeshData &) = delete;
        NavMeshData &operator=(const NavMeshData &) = delete;
        NavMeshData(NavMeshData &&) noexcept = default;
        NavMeshData &operator=(NavMeshData &&) = delete;

        /**
         * @brief Preflights, validates and owns one complete decoded artifact transactionally.
         * @param artifact Borrowed decoded tables plus actual-byte digest evidence.
         * @param limits Positive qualified storage and decode ceilings.
         * @return Owned immutable data or a typed invalid, corrupt, unsupported, or capacity failure.
         * @throws std::bad_alloc if storage fails after every count, size and table range has been validated.
         */
        [[nodiscard]] static Result<NavMeshData> Create(const NavMeshArtifactView &artifact, const NavMeshArtifactLimits &limits = {});

        /** @brief Returns immutable fixed-header facts. @return Artifact header retained by value. */
        [[nodiscard]] const NavMeshArtifactHeader &Header() const noexcept;
        /** @brief Returns canonical tile descriptors. @return Key-sorted immutable tile rows. */
        [[nodiscard]] std::span<const NavMeshTileDescriptor> Tiles() const noexcept;

        /**
         * @brief Resolves one exact tile without coordinate fallback.
         * @param key Stable tile coordinate and layer.
         * @return Borrowed tile-local tables or NavigationErrors::NavMeshTileUnknown.
         */
        [[nodiscard]] Result<NavMeshTileView> ResolveTile(NavMeshTileKey key) const;

        /**
         * @brief Resolves opaque bytes only for one exact provider-private format.
         * @param tile Exact tile owning the provider section.
         * @param compatibility Provider fingerprint, version, byte order and codec supported by the caller.
         * @return Borrowed bytes, NavMeshProviderPayloadIncompatible for a format mismatch, or a typed missing error.
         */
        [[nodiscard]] Result<NavMeshProviderPayloadView> ResolveProviderPayload(
            NavMeshTileKey tile, const NavMeshProviderPayloadCompatibility &compatibility) const;

    private:
        explicit NavMeshData(const NavMeshArtifactView &artifact);

        NavMeshArtifactHeader header_;
        std::vector<NavMeshTileDescriptor> tiles_;
        std::vector<Math::Vec3> vertices_;
        std::vector<NavMeshPolygon> polygons_;
        std::vector<std::uint32_t> polygonVertexIndices_;
        std::vector<std::uint32_t> polygonAdjacencies_;
        std::vector<NavMeshOffMeshLink> offMeshLinks_;
        std::vector<NavMeshSourceProvenance> provenance_;
        std::vector<NavMeshProviderPayloadDescriptor> providerPayloads_;
        std::vector<std::byte> providerPayloadBytes_;
    };
}  // namespace Horo::Navigation
