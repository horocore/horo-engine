#pragma once
/** @file TerrainAuthoringSourceCodec.h @brief Portable canonical terrain source and semantic layer-set persistence. */

#include "Horo/Foundation/Sha256.h"
#include "Horo/Terrain/FoliageAuthoringSource.h"
#include "Horo/Terrain/TerrainMaterial.h"
#include "Horo/Terrain/TerrainSourceImport.h"

namespace Horo::Terrain {
    /** @brief Explicit bounded source I/O policy, independent of runtime residency or backend capabilities. */
    struct TerrainAuthoringCodecLimits final {
        FoliageAuthoringSourceLimits foliage;
        std::size_t maximumBytes{64 * 1024 * 1024};
        std::uint64_t maximumSamples{1'048'576};
        std::uint64_t maximumWorkItems{TerrainDescriptorHardLimits::WorkItems}; /**< Sample/channel visits plus 4 KiB hash chunks. */
    };

    /** @brief Stable semantic layer binding; order covers every stored channel, never only the active brush targets. */
    struct TerrainAuthoringLayerBinding final {
        Assets::AssetId asset;
        TerrainLayerSetId id;
        TerrainContentRevision revision;
        Sha256Digest digest;
        std::array<TerrainLayerId, TerrainDescriptorHardLimits::LayersPerTile> order{};
        auto operator<=>(const TerrainAuthoringLayerBinding &) const = default;
    };

    /** @brief Detached source construction value; source capability is supplied by the admitting host and is not persisted. */
    struct TerrainAuthoringSource final {
        TerrainCanonicalSource source;
        std::optional<TerrainAuthoringLayerBinding> layers;
        FoliageAuthoringSource foliage; /**< Complete authored placement source and only its used definition bindings. */
    };

    /** @brief One actual tracked layer-set asset using the existing material-domain semantic schema. */
    struct TerrainAuthoringLayerSet final {
        Assets::AssetId asset;
        TerrainMaterialLayerSetData layers;
    };

    /** @brief Recognizes the disjoint canonical-container prefix; its first float is a reserved NaN, never valid RAW-F32 height.
     * @param bytes Bounded source bytes. @return Whether the canonical source marker is present.
     */
    [[nodiscard]] bool IsTerrainAuthoringSource(std::span<const std::byte> bytes) noexcept;

    /** @brief Encodes all canonical channels and their complete semantic binding deterministically.
     * @param source Exact source and layer binding. @param limits Positive bounded source policy.
     * @param cancellation Checked during validation and every source row/byte chunk.
     * @return One self-contained versioned source payload or typed failure; no document, registry or file mutation.
     * @details Persistent identities and semantic revisions are encoded. Host capability, workspace session and cook grants are not.
     */
    [[nodiscard]] Result<std::vector<std::byte>> EncodeTerrainAuthoringSource(const TerrainAuthoringSource &source,
                                                                              const TerrainAuthoringCodecLimits &limits = {},
                                                                              const CancellationToken &cancellation = {});

    /** @brief Decodes a complete source and rejects malformed/oversized/unknown versions before bulk allocation.
     * @param bytes Exact immutable source. @param expectedAsset Actual Assets identity that owns these bytes.
     * @param capability Fresh immutable host admission; never recovered from file bytes.
     * @param limits Explicit source limits. @param cancellation Cooperative stop.
     * @return Detached validated source or typed zero-publication failure.
     */
    [[nodiscard]] Result<TerrainAuthoringSource> DecodeTerrainAuthoringSource(std::span<const std::byte> bytes,
                                                                              Assets::AssetId expectedAsset,
                                                                              TerrainCapabilityRevision capability,
                                                                              const TerrainAuthoringCodecLimits &limits = {},
                                                                              const CancellationToken &cancellation = {});

    /** @brief Encodes one actual layer-set asset after canonical material/tier validation.
     * @param source Actual asset identity and full ordered layer meanings. @param configuration Explicit admitted tier limits.
     * @return Small deterministic versioned payload or typed failure; no implicit material lookup or creation.
     */
    [[nodiscard]] Result<std::vector<std::byte>> EncodeTerrainAuthoringLayerSet(const TerrainAuthoringLayerSet &source,
                                                                                const TerrainConfigurationSnapshot &configuration);

    /** @brief Decodes one bounded complete layer-set asset and validates stable identities, uniqueness and tier policy.
     * @param bytes Exact captured source, at most 4 KiB. @param expectedAsset Actual tracked identity.
     * @param configuration Explicit admitted tier limits. @return Full ordered meaning or typed failure.
     */
    [[nodiscard]] Result<TerrainAuthoringLayerSet> DecodeTerrainAuthoringLayerSet(std::span<const std::byte> bytes,
                                                                                  Assets::AssetId expectedAsset,
                                                                                  const TerrainConfigurationSnapshot &configuration);

    /** @brief Matches a source binding to a separately captured authoritative layer-set asset.
     * @param source Source with complete stored weight order. @param layers Captured decoded layer-set source.
     * @param capturedDigest Exact digest from its Assets source capture, not a registry epoch or recomputed substitute.
     * @return Success or typed stale/malformed binding; material existence remains the explicit application capture's responsibility.
     */
    [[nodiscard]] Result<void> ValidateTerrainAuthoringLayerBinding(const TerrainAuthoringSource &source,
                                                                    const TerrainAuthoringLayerSet &layers,
                                                                    const Sha256Digest &capturedDigest);
}  // namespace Horo::Terrain
