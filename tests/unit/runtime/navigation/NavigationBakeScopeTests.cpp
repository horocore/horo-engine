#include "Horo/Navigation/NavigationTileArtifact.h"
#include "Horo/Navigation/NavigationTileDescriptor.h"
#include "navigation/IncrementalBakeFixture.h"

namespace Horo::Navigation {
    namespace {
        using namespace TestSupport;

        /** @brief Construct a complete nonzero Scene UUID for scope isolation checks. */
        NavigationBakeScopeId SceneScope(std::uint8_t value) {
            std::array<std::uint8_t, 16> bytes{};
            bytes.front() = value;
            bytes.back() = 9;
            return NavigationBakeScopeId::ForScene(bytes).Value();
        }
    }  // namespace

    TEST_CASE("Scene bake scopes preserve full UUIDs and never infer a Scene from an unscoped domain", "[unit][navigation][scope]") {
        CHECK(NavigationBakeScopeId::ForScene({}).HasError());
        CHECK_FALSE(NavigationBakeScopeId{}.IsSceneScoped());
        const auto a = SceneScope(1);
        const auto b = SceneScope(2);
        CHECK(a != b);
        CHECK(a.SceneBytes().front() == 1);
        CHECK(a.SceneBytes().back() == 9);
    }

    TEST_CASE("Identical authored surface profile grid and source values stay isolated by Scene scope", "[unit][navigation][scope]") {
        IncrementalBakeFixture fixture;
        fixture.scopes = {SceneScope(1), SceneScope(2)};
        const auto input = fixture.Input();
        REQUIRE(input->Partitions().size() == 2);
        CHECK(input->Partitions()[0].surface == input->Partitions()[1].surface);
        CHECK(input->Partitions()[0].scope != input->Partitions()[1].scope);
        const auto tiles = fixture.Tiles();
        REQUIRE(tiles.size() == 8);
        auto first = PrepareNavigationBakeTile(*input, tiles[0], fixture.compatibility);
        auto second = PrepareNavigationBakeTile(*input, tiles[4], fixture.compatibility);
        REQUIRE(first.HasValue());
        REQUIRE(second.HasValue());
        CHECK(first.Value().dependencyKey != second.Value().dependencyKey);
        CHECK(tiles[0].key != tiles[4].key);
        CHECK(tiles[0].key < tiles[4].key);
        auto wrong = tiles[0];
        wrong.key.scope = SceneScope(3);
        CHECK(PrepareNavigationBakeTile(*input, wrong, fixture.compatibility).HasError());
        fixture.scopes = {SceneScope(1), NavigationBakeScopeId{}};
        RequireError(fixture.TryInput(), NavigationErrors::BakeInputInvalid);
    }

    TEST_CASE("Scoped modifier identity and ranges cannot change a different Scene's matching authored surface",
              "[unit][navigation][scope]") {
        IncrementalBakeFixture fixture;
        fixture.scopes = {SceneScope(1), SceneScope(2)};
        fixture.ExcludeBorder();
        const auto input = fixture.Input();
        REQUIRE(input->Partitions().size() == 2);
        CHECK(input->Partitions()[0].modifierCount == 1);
        CHECK(input->Partitions()[1].modifierCount == 0);
        fixture.modifiers.front().scope = SceneScope(3);
        RequireError(fixture.TryInput(), NavigationErrors::BakeInputReferenceMissing);
    }

    TEST_CASE("HNT2 and HNS4 roundtrip complete Scene scope without rewriting legacy tile formats", "[unit][navigation][scope][content]") {
        IncrementalBakeFixture fixture;
        fixture.scopes = {SceneScope(1), SceneScope(2)};
        const auto input = fixture.Input();
        NavigationCookedTileSet set{input->Fingerprint(), {}, NavigationCookedContentProvenance{fixture.compatibility, std::nullopt}};
        for (const auto index : {0, 4}) {
            const auto prepared = PrepareNavigationBakeTile(*input, fixture.Tiles()[index], fixture.compatibility);
            REQUIRE(prepared.HasValue());
            auto tile = NavigationCookedTile::Create(prepared.Value(),
                                                     {.key = prepared.Value().tile.key.tile, .bounds = prepared.Value().tile.bounds});
            REQUIRE(tile.HasValue());
            CHECK(tile.Value()->Bytes()[3] == '2');
            const auto descriptor = ProjectNavigationCookedTileDescriptor(*tile.Value());
            REQUIRE(descriptor.HasValue());
            CHECK(descriptor.Value().tileSizeMeters == 8);
            set.tiles.push_back(tile.Value());
        }
        const auto encoded = EncodeNavigationCookedTileSet(set, 8192);
        REQUIRE(encoded.HasValue());
        REQUIRE(encoded.Value()[3] == '4');
        const auto decoded = DecodeNavigationCookedTileSet(encoded.Value(), 8192);
        REQUIRE(decoded.HasValue());
        REQUIRE(decoded.Value().tiles.size() == 2);
        CHECK(decoded.Value().tiles[0]->Key().scope == SceneScope(1));
        CHECK(decoded.Value().tiles[1]->Key().scope == SceneScope(2));
        CHECK(EncodeNavigationCookedTileSet(decoded.Value(), 8192).Value() == encoded.Value());
        auto masked = encoded.Value();
        masked[3] = '2';
        CHECK(DecodeNavigationCookedTileSet(masked, 8192).HasError());
        auto bytes = std::vector<std::uint8_t>{set.tiles.front()->Bytes().begin(), set.tiles.front()->Bytes().end()};
        std::fill(bytes.begin() + 4, bytes.begin() + 20, 0);
        CHECK(NavigationCookedTile::Decode(bytes).HasError());
        set.provenance.reset();
        CHECK(EncodeNavigationCookedTileSet(set, 8192).HasError());
    }
}  // namespace Horo::Navigation
