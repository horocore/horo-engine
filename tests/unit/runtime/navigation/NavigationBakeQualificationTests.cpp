#include "navigation/NavigationBakeServiceFixture.h"

namespace Horo::Application {
    using namespace Horo::Navigation;
    using namespace Horo::Navigation::TestSupport;
    using namespace BakeTestSupport;

    namespace {
        /** @brief Proves that caller permutations cannot bypass canonical closure admission. */
        void RejectNoncanonicalClosure(BakeHarness &harness, const std::size_t repetition, std::vector<NavigationBakeTile> &tiles) {
            if (repetition == 0)
                return;
            if (repetition == 1)
                std::ranges::reverse(tiles);
            else
                std::ranges::rotate(tiles, tiles.begin() + 1);
            auto rejected = harness.service->Submit({.input = harness.fixture.Input(),
                                                     .compatibility = harness.fixture.compatibility,
                                                     .tiles = tiles,
                                                     .sources = harness.fixture.Observations()});
            REQUIRE(rejected.HasError());
            CHECK(rejected.ErrorValue().code == NavigationErrors::BakeInputInvalid.code);
            CHECK(harness.builder->builds.load() == 0);
            CHECK_FALSE(harness.service->Published());
            std::ranges::sort(tiles, {}, &NavigationBakeTile::key);
        }
    }  // namespace

    TEST_CASE("Native bake qualification reproduces cold artifact bytes and rejects noncanonical closure order",
              "[navigation][bake][qualification][native]") {
        std::optional<Assets::AssetCookGenerationContents> expected;
        std::optional<Sha256Digest> expectedManifest;
        std::vector<Sha256Digest> expectedTiles;
        for (std::size_t repetition = 0; repetition < 3; ++repetition) {
            BakeHarness harness;
            auto tiles = harness.fixture.Tiles();
            REQUIRE(harness.config.sourceAuthority->UpdateCurrent(harness.fixture.revisions, harness.fixture.Observations()).HasValue());
            RejectNoncanonicalClosure(harness, repetition, tiles);
            auto submitted = harness.service->Submit({.input = harness.fixture.Input(),
                                                      .compatibility = harness.fixture.compatibility,
                                                      .tiles = std::move(tiles),
                                                      .sources = harness.fixture.Observations()});
            REQUIRE(submitted.HasValue());
            REQUIRE(Terminal(*harness.service, harness.operations, submitted.Value()).state == OperationState::Succeeded);
            const auto publication = harness.service->Published();
            REQUIRE(publication);
            REQUIRE(publication->rebuiltTiles == 4);
            REQUIRE(publication->reusedTiles == 0);
            REQUIRE(harness.builder->builds.load() == 4);
            auto read = Assets::ReadCookGenerationContents(publication->generation, harness.config.maximumCandidateBytes);
            REQUIRE(read.HasValue());
            const auto &contents = read.Value();
            REQUIRE(contents.artifacts.size() == 1);
            REQUIRE(contents.entries.size() == 1);
            REQUIRE(Query(publication->tiles).Value().status == NavigationPathStatus::Reachable);
            if (!expected) {
                expected = contents;
                expectedManifest = publication->generation.manifestDigest;
                for (const auto &tile : publication->tiles.tiles)
                    expectedTiles.push_back(tile->ContentIdentity());
            } else {
                CHECK(publication->generation.manifestDigest == *expectedManifest);
                CHECK(contents.artifacts == expected->artifacts);
                CHECK(contents.entries.front().artifactHash == expected->entries.front().artifactHash);
                REQUIRE(publication->tiles.tiles.size() == expectedTiles.size());
                for (std::size_t index = 0; index < expectedTiles.size(); ++index)
                    CHECK(publication->tiles.tiles[index]->ContentIdentity() == expectedTiles[index]);
            }
        }
    }

    TEST_CASE("Native bake qualification preserves verified bytes through output write failure and recovers",
              "[navigation][bake][qualification][native]") {
        auto files = std::make_shared<ControlledFiles>();
        BakeHarness harness(files);
        REQUIRE(Terminal(*harness.service, harness.operations, Submit(harness)).state == OperationState::Succeeded);
        const auto before = harness.service->Published();
        REQUIRE(before);
        const auto priorContents = Assets::ReadCookGenerationContents(before->generation, harness.config.maximumCandidateBytes);
        REQUIRE(priorContents.HasValue());
        harness.fixture.ExcludeBorder();
        files->failWrites.store(true);
        CHECK(Terminal(*harness.service, harness.operations, Submit(harness)).state == OperationState::Failed);
        CHECK(harness.service->Published() == before);
        const auto current = Assets::ResolveCurrentCookGeneration(harness.config.targetRoot);
        REQUIRE(current.HasValue());
        CHECK(current.Value().manifestDigest == before->generation.manifestDigest);
        const auto read = Assets::ReadCookGenerationContents(current.Value(), harness.config.maximumCandidateBytes);
        REQUIRE(read.HasValue());
        CHECK(read.Value().artifacts == priorContents.Value().artifacts);
        CHECK(Query(before->tiles).Value().status == NavigationPathStatus::Reachable);
        files->failWrites.store(false);
        REQUIRE(Terminal(*harness.service, harness.operations, Submit(harness)).state == OperationState::Succeeded);
        const auto after = harness.service->Published();
        REQUIRE(after);
        CHECK(after->generation.manifestDigest != before->generation.manifestDigest);
        CHECK(Query(after->tiles).Value().status != NavigationPathStatus::Reachable);
        CHECK(Assets::ReadCookGenerationContents(before->generation, harness.config.maximumCandidateBytes).Value().artifacts ==
              priorContents.Value().artifacts);
    }

    TEST_CASE("Native bake qualification refuses an undersized output profile without replacing the verified generation",
              "[navigation][bake][qualification][native]") {
        BakeHarness harness;
        REQUIRE(Terminal(*harness.service, harness.operations, Submit(harness)).state == OperationState::Succeeded);
        const auto before = harness.service->Published();
        REQUIRE(before);
        const auto priorContents = Assets::ReadCookGenerationContents(before->generation, harness.config.maximumCandidateBytes);
        REQUIRE(priorContents.HasValue());
        harness.service->Close();
        auto restricted = harness.config;
        restricted.maximumCandidateBytes = 1;
        harness.service = NavigationBakeService::Create(restricted, harness.operations, harness.jobs).Value();
        harness.fixture.ExcludeBorder();
        CHECK(Terminal(*harness.service, harness.operations, Submit(harness)).state == OperationState::Failed);
        CHECK_FALSE(harness.service->Published());
        const auto current = Assets::ResolveCurrentCookGeneration(harness.config.targetRoot);
        REQUIRE(current.HasValue());
        CHECK(current.Value().manifestDigest == before->generation.manifestDigest);
        const auto read = Assets::ReadCookGenerationContents(current.Value(), harness.config.maximumCandidateBytes);
        REQUIRE(read.HasValue());
        CHECK(read.Value().artifacts == priorContents.Value().artifacts);
        CHECK(Query(before->tiles).Value().status == NavigationPathStatus::Reachable);
        harness.service->Close();
        harness.service = NavigationBakeService::Create(harness.config, harness.operations, harness.jobs).Value();
        REQUIRE(Terminal(*harness.service, harness.operations, Submit(harness)).state == OperationState::Succeeded);
        CHECK(Query(harness.service->Published()->tiles).Value().status != NavigationPathStatus::Reachable);
    }

}  // namespace Horo::Application
