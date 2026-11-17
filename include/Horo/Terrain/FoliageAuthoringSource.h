#pragma once
/** @file FoliageAuthoringSource.h @brief Portable authored placements and immutable foliage type asset bindings. */
#include "Horo/Assets/AssetId.h"
#include "Horo/Foundation/Sha256.h"
#include "Horo/Terrain/FoliageDefinition.h"
#include "Horo/Terrain/TerrainSourceImport.h"

namespace Horo::Terrain {
    /** @brief Authored dataset-local placement; cooked cluster and runtime ownership are absent. */
    struct TerrainAuthoredPlacement final {
        FoliageInstanceId id;
        FoliageTypeId type;
        std::uint32_t sampleX{}, sampleZ{};
        float offsetMeters{}, scale{1.0F}, yawRadians{};
        [[nodiscard]] bool operator==(const TerrainAuthoredPlacement &) const noexcept = default;
    };

    /** @brief Exact used type binding captured from an actual tracked foliage definition asset. */
    struct FoliageAuthoringDefinitionReference final {
        Assets::AssetId asset;
        FoliageTypeId type;
        FoliageDefinitionRevision revision;
        Sha256Digest sourceDigest;
        [[nodiscard]] auto operator<=>(const FoliageAuthoringDefinitionReference &) const noexcept = default;
    };

    /** @brief Owned canonical authoring source; vectors encode in stable identity order. */
    struct FoliageAuthoringSource final {
        std::vector<TerrainAuthoredPlacement> placements;
        std::vector<FoliageAuthoringDefinitionReference> definitions;
        [[nodiscard]] bool operator==(const FoliageAuthoringSource &) const noexcept = default;
    };

    /** @brief Lowerable finite source admission and conservative sorting/validation work ceilings. */
    struct FoliageAuthoringSourceLimits final {
        std::size_t maximumBytes{8U * 1024U * 1024U};
        std::size_t maximumPlacements{65'536}, maximumDefinitions{256};
        std::uint64_t maximumWorkItems{4'194'304};
    };

    /** @brief Conservative complete admitted source footprint, reusable by containing source formats. */
    struct FoliageAuthoringSourceFootprint final {
        std::uint64_t encodedBytes{}, workItems{};
    };

    /** @brief Measure/admit collection storage and sort/lookup work before any allocation.
     * @param placementCount Declared complete placement count. @param definitionCount Declared used type count.
     * @param limits Lowerable finite source limits. @return Exact encoded bytes and conservative work, or typed failure. */
    [[nodiscard]] Result<FoliageAuthoringSourceFootprint> MeasureFoliageAuthoringSource(std::size_t placementCount,
                                                                                        std::size_t definitionCount,
                                                                                        const FoliageAuthoringSourceLimits &limits = {});
    /** @brief Measure an encoded subsection through the same count/header authority, before containing-format allocation.
     * @param bytes Exact bounded subsection. @param limits Lowerable finite source policy.
     * @return Admitted complete byte/work footprint or typed malformed/limit failure; semantic records remain for Decode. */
    [[nodiscard]] Result<FoliageAuthoringSourceFootprint> MeasureEncodedFoliageAuthoringSource(
        std::span<const std::byte> bytes, const FoliageAuthoringSourceLimits &limits = {});

    /** @brief Actual separately tracked foliage type source, without host grants or generation tokens. */
    struct FoliageTypeSource final {
        Assets::AssetId asset;
        FoliageTypeDefinitionData definition;
        [[nodiscard]] bool operator==(const FoliageTypeSource &) const noexcept = default;
    };

    /** @brief Validate finite shape, identities, transforms and complete used type membership without mutation.
     * @param source Owned placement/reference values. @param width Canonical source width. @param height Canonical source height.
     * @param limits Explicit finite policy. @param cancellation Cooperative stop.
     * @return Success or existing typed Terrain source error, with no publication. */
    [[nodiscard]] Result<void> ValidateFoliageAuthoringSource(const FoliageAuthoringSource &source, std::uint32_t width,
                                                              std::uint32_t height, const FoliageAuthoringSourceLimits &limits = {},
                                                              const CancellationToken &cancellation = {});
    /** @brief Encode a detached source canonically without changing identities or caller order.
     * @param source Actual placements and used references. @param width Canonical width. @param height Canonical height.
     * @param limits Finite policy. @param cancellation Cooperative stop. @return Padding-free versioned bytes or typed failure. */
    [[nodiscard]] Result<std::vector<std::byte>> EncodeFoliageAuthoringSource(const FoliageAuthoringSource &source, std::uint32_t width,
                                                                              std::uint32_t height,
                                                                              const FoliageAuthoringSourceLimits &limits = {},
                                                                              const CancellationToken &cancellation = {});
    /** @brief Decode exact canonical bytes after size/count admission and reject missing type bindings.
     * @param bytes Immutable source. @param width Canonical width. @param height Canonical height.
     * @param limits Finite policy. @param cancellation Cooperative stop. @return Detached values or typed zero-publication failure. */
    [[nodiscard]] Result<FoliageAuthoringSource> DecodeFoliageAuthoringSource(std::span<const std::byte> bytes, std::uint32_t width,
                                                                              std::uint32_t height,
                                                                              const FoliageAuthoringSourceLimits &limits = {},
                                                                              const CancellationToken &cancellation = {});
    /** @brief Encode an actual type asset through the existing definition validator.
     * @param source Actual source asset. @param configuration Exact project tier. @param capabilities Explicit admitted grants.
     * @param cancellation Cooperative stop. @return Canonical bounded bytes or original typed failure. */
    [[nodiscard]] Result<std::vector<std::byte>> EncodeFoliageTypeSource(const FoliageTypeSource &source,
                                                                         const TerrainConfigurationSnapshot &configuration,
                                                                         FoliageDefinitionCapabilitySet capabilities,
                                                                         const CancellationToken &cancellation = {});
    /** @brief Decode a bounded type source without granting capabilities or selecting asset dependencies.
     * @param bytes Exact captured bytes. @param expectedAsset Actual tracked Assets identity. @param configuration Project tier.
     * @param capabilities Explicit admitted grants. @param cancellation Cooperative stop.
     * @return Validated typed source or original typed failure; host still verifies referenced mesh/material assets. */
    [[nodiscard]] Result<FoliageTypeSource> DecodeFoliageTypeSource(std::span<const std::byte> bytes, Assets::AssetId expectedAsset,
                                                                    const TerrainConfigurationSnapshot &configuration,
                                                                    FoliageDefinitionCapabilitySet capabilities,
                                                                    const CancellationToken &cancellation = {});
}  // namespace Horo::Terrain
