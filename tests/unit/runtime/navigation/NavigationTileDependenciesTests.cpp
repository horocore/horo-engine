#include "Horo/Navigation/NavigationTileArtifact.h"
#include "navigation/IncrementalBakeFixture.h"

namespace Horo::Navigation {
    using namespace TestSupport;

    TEST_CASE("Tile dependencies invalidate border neighbors and preserve remote tiles across scene revisions") {
        IncrementalBakeFixture fixture;
        const auto original = fixture.Input();
        const auto tiles = fixture.Tiles();
        fixture.ExcludeBorder();
        const auto edited = fixture.Input();
        REQUIRE(original->Fingerprint() != edited->Fingerprint());
        for (std::size_t i = 0; i < tiles.size(); ++i) {
            const auto before = PrepareNavigationBakeTile(*original, tiles[i], fixture.compatibility).Value();
            const auto after = PrepareNavigationBakeTile(*edited, tiles[i], fixture.compatibility).Value();
            CHECK((before.dependencyKey != after.dependencyKey) == (i < 2));
            CHECK(before.borderSizeCells == 4);
        }
        const auto border = PrepareNavigationBakeTile(*edited, tiles[0], fixture.compatibility).Value();
        fixture.ExcludeBorder(16);
        const auto moved = PrepareNavigationBakeTile(*fixture.Input(), tiles[0], fixture.compatibility).Value();
        CHECK(border.dependencyKey != moved.dependencyKey);
        CHECK(moved.modifiers.empty());
        CHECK(moved.dependencyKey == PrepareNavigationBakeTile(*original, tiles[0], fixture.compatibility).Value().dependencyKey);
    }

    TEST_CASE("Tile keys reject false hits for changed actual values provider schemas settings and coordinate policy") {
        IncrementalBakeFixture fixture;
        const auto tile = fixture.Tiles().front();
        const auto initial = PrepareNavigationBakeTile(*fixture.Input(), tile, fixture.compatibility).Value().dependencyKey;
        fixture.vertices.front().y = 0.2F;  // Producer revision and digest deliberately remain unchanged.
        CHECK(PrepareNavigationBakeTile(*fixture.Input(), tile, fixture.compatibility).Value().dependencyKey != initial);
        fixture.vertices.front().y = 0;
        const std::array compatibilityCases{NavigationTileBakeCompatibility{Digest(90), fixture.compatibility.schemas,
                                                                            fixture.compatibility.settings},
                                            NavigationTileBakeCompatibility{fixture.compatibility.provider, Digest(90),
                                                                            fixture.compatibility.settings},
                                            NavigationTileBakeCompatibility{fixture.compatibility.provider, fixture.compatibility.schemas,
                                                                            Digest(90)}};
        for (const auto &compatibility : compatibilityCases)
            CHECK(PrepareNavigationBakeTile(*fixture.Input(), tile, compatibility).Value().dependencyKey != initial);
        fixture.profile.buildGeometry.radiusMeters = 0.6F;
        CHECK(PrepareNavigationBakeTile(*fixture.Input(), tile, fixture.compatibility).Value().dependencyKey != initial);
        fixture.profile.buildGeometry.radiusMeters = 0.5F;
        fixture.revisions.coordinates = Id<NavigationCoordinatePolicyRevision>(2);
        CHECK(PrepareNavigationBakeTile(*fixture.Input(), tile, fixture.compatibility).Value().dependencyKey != initial);
    }

    TEST_CASE("Portable empty tile identities reject truncation trailing data invalid counts and digest tampering") {
        IncrementalBakeFixture fixture;
        const auto prepared = PrepareNavigationBakeTile(*fixture.Input(), fixture.Tiles().front(), fixture.compatibility).Value();
        auto created = NavigationCookedTile::Create(prepared, {.key = prepared.tile.key.tile, .bounds = prepared.tile.bounds});
        REQUIRE(created.HasValue());
        const auto tile = created.Value();
        auto roundtrip = NavigationCookedTile::Decode(tile->Bytes());
        REQUIRE(roundtrip.HasValue());
        CHECK(NavigationCookedTile::Decode(tile->Bytes(), 0).HasError());
        CHECK(NavigationCookedTile::Decode(tile->Bytes(), NavigationTileBuildLimits::MaximumOwnedBytes + 1).HasError());
        CHECK(tile->StorageBytes() >= tile->Bytes().size());
        CHECK(tile->ContentIdentity() == roundtrip.Value()->ContentIdentity());
        NavigationCookedTileSet set{.inputFingerprint = fixture.Input()->Fingerprint(), .tiles = {tile}};
        const auto encoded = EncodeNavigationCookedTileSet(set, 4096).Value();
        REQUIRE(DecodeNavigationCookedTileSet(encoded, 4096).HasValue());
        CHECK(DecodeNavigationCookedTileSet(encoded, encoded.size()).HasError());
        for (const auto size : {std::size_t{0}, std::size_t{4}, encoded.size() - 1})
            CHECK(DecodeNavigationCookedTileSet(std::span{encoded}.first(size), 4096).HasError());
        auto corrupt = encoded;
        corrupt.back() = static_cast<std::uint8_t>(std::byte{corrupt.back()} ^ std::byte{1});
        CHECK(DecodeNavigationCookedTileSet(corrupt, 4096).HasError());
        corrupt = encoded;
        corrupt.push_back(0);
        CHECK(DecodeNavigationCookedTileSet(corrupt, 4096).HasError());
        corrupt = encoded;
        for (std::size_t i = 36; i < 40; ++i)
            corrupt[i] = 255;
        CHECK(DecodeNavigationCookedTileSet(corrupt, 4096).HasError());
        CHECK(EncodeNavigationCookedTileSet(set, 10).HasError());
        CancellationSource cancellation;
        cancellation.RequestCancellation();
        CHECK(PrepareNavigationBakeTile(*fixture.Input(), prepared.tile, fixture.compatibility, cancellation.Token()).HasError());
    }

    TEST_CASE("Navigation tile limits admit exact hard ceilings and minimum native budgets") {
        constexpr NavigationTileBuildLimits maximum;
        STATIC_REQUIRE(maximum.IsValid());
        constexpr NavigationTileBuildLimits minimum{.maximumVertices = 1,
                                                    .maximumPolygons = 1,
                                                    .maximumOffMeshLinks = 1,
                                                    .maximumVerticesPerPolygon = 3,
                                                    .maximumOwnedBytes = 1,
                                                    .maximumWorkUnits = 1};
        STATIC_REQUIRE(minimum.IsValid());
    }

    TEST_CASE("HNS2 policy changes preserve HNT1 tiles and distinguish aggregate provenance", "[unit][navigation][content]") {
        IncrementalBakeFixture fixture;
        const auto input = fixture.Input();
        const auto prepared = PrepareNavigationBakeTile(*input, fixture.Tiles().front(), fixture.compatibility);
        REQUIRE(prepared.HasValue());
        const auto tile =
            NavigationCookedTile::Create(prepared.Value(), {.key = prepared.Value().tile.key.tile, .bounds = prepared.Value().tile.bounds});
        REQUIRE(tile.HasValue());
        NavigationProjectProfileInput policy{.id = Id<NavigationProjectProfileId>(17),
                                             .revision = fixture.revisions.projectProfile,
                                             .capacities = {64, 4, 8, 2, 4096, 32768, 2048},
                                             .maximumQuery = {.query = NavigationQueryKind::Path,
                                                              .quality = NavigationQualityLevel::Balanced,
                                                              .limits = {1024, 128, 2000}}};
        policy.capabilities.fill(NavigationCapabilityRequirement::Optional);
        const auto first = NavigationProjectProfile::Create(policy);
        REQUIRE(first.HasValue());
        NavigationCookedTileSet set{input->Fingerprint(),
                                    {tile.Value()},
                                    NavigationCookedContentProvenance{fixture.compatibility, first.Value()}};
        const auto encoded = EncodeNavigationCookedTileSet(set, 16384);
        REQUIRE(encoded.HasValue());
        REQUIRE(encoded.Value()[3] == '2');
        const auto decoded = DecodeNavigationCookedTileSet(encoded.Value(), 16384);
        REQUIRE(decoded.HasValue());
        REQUIRE(decoded.Value().provenance.has_value());
        REQUIRE(decoded.Value().provenance->projectProfile.has_value());
        CHECK(decoded.Value().provenance->projectProfile->MatchesAuthority(first.Value()));
        CHECK(decoded.Value().provenance->compatibility == fixture.compatibility);
        ++policy.capacities.maximumAgents;
        const auto second = NavigationProjectProfile::Create(policy);
        REQUIRE(second.HasValue());
        set.provenance->projectProfile = second.Value();
        const auto changed = EncodeNavigationCookedTileSet(set, 16384);
        REQUIRE(changed.HasValue());
        CHECK(changed.Value() != encoded.Value());
        CHECK(ComputeSha256(std::as_bytes(std::span{changed.Value()})) != ComputeSha256(std::as_bytes(std::span{encoded.Value()})));
        const auto changedDecoded = DecodeNavigationCookedTileSet(changed.Value(), 16384);
        REQUIRE(changedDecoded.HasValue());
        REQUIRE(!changedDecoded.Value().tiles.empty());
        CHECK(changedDecoded.Value().tiles.front()->ContentIdentity() == tile.Value()->ContentIdentity());
        CHECK(changedDecoded.Value().tiles.front()->Bytes().size() == tile.Value()->Bytes().size());
    }

    TEST_CASE("HNS2 rejects missing compatibility unknown versions truncated evidence and false partition closure",
              "[unit][navigation][content][malformed]") {
        IncrementalBakeFixture fixture;
        const auto input = fixture.Input();
        const auto prepared = PrepareNavigationBakeTile(*input, fixture.Tiles().front(), fixture.compatibility);
        REQUIRE(prepared.HasValue());
        const auto tile =
            NavigationCookedTile::Create(prepared.Value(), {.key = prepared.Value().tile.key.tile, .bounds = prepared.Value().tile.bounds});
        REQUIRE(tile.HasValue());
        NavigationCookedTileSet set{input->Fingerprint(),
                                    {tile.Value()},
                                    NavigationCookedContentProvenance{fixture.compatibility, std::nullopt}};
        const auto encoded = EncodeNavigationCookedTileSet(set, 16384);
        REQUIRE(encoded.HasValue());
        for (std::size_t size = 0; size < encoded.Value().size(); ++size)
            CHECK(DecodeNavigationCookedTileSet(std::span{encoded.Value()}.first(size), 16384).HasError());
        auto malformed = encoded.Value();
        malformed[3] = '3';
        RequireError(DecodeNavigationCookedTileSet(malformed, 16384), NavigationErrors::UnsupportedCookedVersion);
        malformed = encoded.Value();
        // Fixed HNS2 prefix: magic, source digest, three compatibility digests, no-policy marker, count, profile/surface pair.
        constexpr std::size_t ClosureProfileOffset = 4 + 32 + 96 + 1 + 4;
        REQUIRE(malformed.size() > ClosureProfileOffset + 16);
        malformed[ClosureProfileOffset] ^= 1;
        RequireError(DecodeNavigationCookedTileSet(malformed, 16384), NavigationErrors::NavMeshArtifactCorrupt);
        set.provenance->compatibility.schemas = {};
        RequireError(EncodeNavigationCookedTileSet(set, 16384), NavigationErrors::BakeInputInvalid);
        set.provenance.reset();
        const auto legacy = EncodeNavigationCookedTileSet(set, 16384);
        REQUIRE(legacy.HasValue());
        REQUIRE(legacy.Value()[3] == '1');
        const auto decodedLegacy = DecodeNavigationCookedTileSet(legacy.Value(), 16384);
        REQUIRE(decodedLegacy.HasValue());
        CHECK_FALSE(decodedLegacy.Value().provenance.has_value());
    }
}  // namespace Horo::Navigation
