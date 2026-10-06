#pragma once

#include "Horo/Assets/AssetCook.h"
#include "Horo/Assets/NavMeshAssetType.h"
#include "Horo/Navigation/NavigationTileArtifact.h"
#include "navigation/IncrementalBakeFixture.h"

namespace Horo::Navigation::TestSupport {
    /** @brief Validated finite project policy, independent of native providers and renderer selection. */
    [[nodiscard]] inline NavigationProjectProfile ContentProfile(const std::uint64_t identity = 17) {
        NavigationProjectProfileInput input{.id = Id<NavigationProjectProfileId>(identity),
                                            .revision = Id<NavigationProjectProfileRevision>(1),
                                            .capacities = {64, 4, 8, 2, 4096, 32768, 2048},
                                            .maximumQuery = {.query = NavigationQueryKind::Path,
                                                             .quality = NavigationQualityLevel::Balanced,
                                                             .limits = {1024, 128, 2000}}};
        input.capabilities.fill(NavigationCapabilityRequirement::Optional);
        input.capabilities[static_cast<std::size_t>(NavigationCapability::GroundedQueries)] = NavigationCapabilityRequirement::Required;
        auto result = NavigationProjectProfile::Create(input);
        REQUIRE(result.HasValue());
        return std::move(result).Value();
    }

    /** @brief Self-contained encoded empty tile remains declared navigation content with a real typed partition. */
    [[nodiscard]] inline NavigationCookedTileSet EmptyContent(const std::uint64_t identity = 17) {
        IncrementalBakeFixture fixture;
        const auto input = fixture.Input();
        const auto prepared = PrepareNavigationBakeTile(*input, fixture.Tiles().front(), fixture.compatibility);
        REQUIRE(prepared.HasValue());
        const auto tile =
            NavigationCookedTile::Create(prepared.Value(), {.key = prepared.Value().tile.key.tile, .bounds = prepared.Value().tile.bounds});
        REQUIRE(tile.HasValue());
        return {input->Fingerprint(), {tile.Value()}, NavigationCookedContentProvenance{fixture.compatibility, ContentProfile(identity)}};
    }

    /** @brief Canonical owning envelope preserves actual source fingerprint and exact aggregate payload identity. */
    [[nodiscard]] inline std::vector<std::uint8_t> ContentEnvelope(const NavigationCookedTileSet &set, const Assets::AssetId id,
                                                                   const AssetCookTargetId &target) {
        const auto payload = EncodeNavigationCookedTileSet(set, 16384);
        REQUIRE(payload.HasValue());
        const auto digest = ComputeSha256(std::as_bytes(std::span{payload.Value()}));
        const auto type = Assets::AssetTypeId::Parse(Assets::NavMeshAssetTypeName);
        REQUIRE(type.HasValue());
        const Assets::AssetCookArtifact envelope{.id = id,
                                                 .type = type.Value(),
                                                 .target = target,
                                                 .cacheKeyDigest = digest,
                                                 .sourceDigest = set.inputFingerprint,
                                                 .payloadDigest = digest,
                                                 .payload = payload.Value()};
        const auto encoded = Assets::EncodeCookedArtifact(envelope);
        REQUIRE(encoded.HasValue());
        return encoded.Value();
    }
}  // namespace Horo::Navigation::TestSupport
