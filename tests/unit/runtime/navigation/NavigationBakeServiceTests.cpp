#include "navigation/NavigationBakeServiceFixture.h"

namespace Horo::Application {
    using namespace Horo::Navigation;
    using namespace Horo::Navigation::TestSupport;

    using namespace BakeTestSupport;

    TEST_CASE("Incremental admission rejects invalid native ceilings before scheduling with either cold or warm cache") {
        BakeHarness harness;
        if (GENERATE(false, true))
            REQUIRE(Terminal(*harness.service, harness.operations, Submit(harness)).state == OperationState::Succeeded);
        const auto before = harness.service->Published();
        const auto reject = [&harness](auto field, const auto value) {
            auto config = harness.config;
            config.tileLimits.*field = value;
            const auto created = NavigationBakeService::Create(std::move(config), harness.operations, harness.jobs);
            REQUIRE(created.HasError());
            CHECK(created.ErrorValue().domain.Value() == NavigationErrors::BakeInputInvalid.domain.Value());
            CHECK(created.ErrorValue().code.Value() == NavigationErrors::BakeInputInvalid.code.Value());
        };
        using Limits = NavigationTileBuildLimits;
        reject(&Limits::maximumWorkUnits, std::numeric_limits<std::uint64_t>::max() / 4 + 2);
        reject(&Limits::maximumOwnedBytes, std::numeric_limits<std::uint64_t>::max());
        reject(&Limits::maximumVertices, 0U);
        reject(&Limits::maximumVertices, Limits::MaximumVertices + 1);
        reject(&Limits::maximumPolygons, 0U);
        reject(&Limits::maximumPolygons, Limits::MaximumPolygons + 1);
        reject(&Limits::maximumOffMeshLinks, 0U);
        reject(&Limits::maximumOffMeshLinks, Limits::MaximumOffMeshLinks + 1);
        reject(&Limits::maximumVerticesPerPolygon, 2U);
        reject(&Limits::maximumVerticesPerPolygon, Limits::MaximumVerticesPerPolygon + 1);
        reject(&Limits::maximumOwnedBytes, std::uint64_t{0});
        reject(&Limits::maximumOwnedBytes, Limits::MaximumOwnedBytes + 1);
        reject(&Limits::maximumWorkUnits, std::uint64_t{0});
        reject(&Limits::maximumWorkUnits, Limits::MaximumWorkUnits + 1);
        CHECK(harness.service->Published() == before);
    }

    TEST_CASE("Incremental production cook rebuilds both sides of an edited border and reuses remote content identities") {
        BakeHarness harness;
        REQUIRE(Terminal(*harness.service, harness.operations, Submit(harness)).state == OperationState::Succeeded);
        const auto before = harness.service->Published();
        REQUIRE(before);
        REQUIRE(before->rebuiltTiles == 4);
        REQUIRE(Query(before->tiles).Value().status == NavigationPathStatus::Reachable);
        harness.fixture.ExcludeBorder();
        REQUIRE(Terminal(*harness.service, harness.operations, Submit(harness)).state == OperationState::Succeeded);
        const auto after = harness.service->Published();
        REQUIRE(after);
        CHECK(after->rebuiltTiles == 2);
        CHECK(after->reusedTiles == 2);
        CHECK(harness.builder->builds.load() == 6);
        CHECK(before->tiles.tiles[2] == after->tiles.tiles[2]);
        CHECK(before->tiles.tiles[3]->ContentIdentity() == after->tiles.tiles[3]->ContentIdentity());
        CHECK(Query(after->tiles).Value().status != NavigationPathStatus::Reachable);
        CHECK(Query(before->tiles).Value().status == NavigationPathStatus::Reachable);  // Retained generation remains queryable.
        auto current = Assets::ResolveCurrentCookGeneration(harness.config.targetRoot);
        REQUIRE(current.HasValue());
        auto contents = Assets::ReadCookGenerationContents(current.Value(), harness.config.maximumCandidateBytes);
        REQUIRE(contents.HasValue());
        auto envelope = Assets::DecodeCookedArtifact(contents.Value().artifacts.front());
        REQUIRE(envelope.HasValue());
        auto restored = DecodeNavigationCookedTileSet(envelope.Value().payload, harness.config.maximumCandidateBytes);
        REQUIRE(restored.HasValue());
        CHECK(restored.Value().tiles[3]->ContentIdentity() == after->tiles.tiles[3]->ContentIdentity());
        CHECK(Query(restored.Value()).Value().status != NavigationPathStatus::Reachable);
        harness.service->Close();
        harness.service = NavigationBakeService::Create(harness.config, harness.operations, harness.jobs).Value();
        REQUIRE(Terminal(*harness.service, harness.operations, Submit(harness)).state == OperationState::Succeeded);
        CHECK(harness.service->Published()->reusedTiles == 4);
        CHECK(harness.builder->builds.load() == 6);
    }

    TEST_CASE("Owned project policy changes cannot join baking or alias promoted aggregate identity", "[navigation][content][policy]") {
        BakeHarness harness;
        const auto &fixture = harness.fixture;
        REQUIRE(harness.config.sourceAuthority->UpdateCurrent(fixture.revisions, fixture.Observations()).HasValue());
        const auto submit = [&](const std::uint64_t policy) {
            auto result = harness.service->Submit({.input = fixture.Input(),
                                                   .compatibility = fixture.compatibility,
                                                   .tiles = fixture.Tiles(),
                                                   .sources = fixture.Observations(),
                                                   .projectProfile = ContentProfile(policy)});
            REQUIRE(result.HasValue());
            return result.Value();
        };
        harness.builder->pause.store(true);
        const auto first = submit(17);
        for (std::size_t iteration = 0; iteration < 2000 && !harness.builder->entered.load(); ++iteration)
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        REQUIRE(harness.builder->entered.load());
        REQUIRE(submit(17) == first);
        const auto changed = submit(18);
        REQUIRE(changed != first);
        harness.builder->pause.store(false);
        REQUIRE(Terminal(*harness.service, harness.operations, first).state == OperationState::Cancelled);
        REQUIRE(Terminal(*harness.service, harness.operations, changed).state == OperationState::Succeeded);
        const auto before = harness.service->Published();
        REQUIRE(before);
        REQUIRE(before->tiles.provenance.has_value());
        REQUIRE(before->tiles.provenance->projectProfile.has_value());
        REQUIRE(before->tiles.provenance->projectProfile->MatchesAuthority(ContentProfile(18)));
        const auto builds = harness.builder->builds.load();
        REQUIRE(Terminal(*harness.service, harness.operations, submit(17)).state == OperationState::Succeeded);
        const auto after = harness.service->Published();
        REQUIRE(after);
        REQUIRE(after->tiles.provenance.has_value());
        REQUIRE(after->tiles.provenance->projectProfile.has_value());
        REQUIRE(after->tiles.provenance->projectProfile->MatchesAuthority(ContentProfile(17)));
        REQUIRE(harness.builder->builds.load() == builds);
        REQUIRE(after->reusedTiles == before->tiles.tiles.size());
        REQUIRE(after->generation.manifestDigest != before->generation.manifestDigest);
        REQUIRE(after->tiles.tiles.size() == before->tiles.tiles.size());
        for (std::size_t index = 0; index < after->tiles.tiles.size(); ++index)
            REQUIRE(after->tiles.tiles[index]->ContentIdentity() == before->tiles.tiles[index]->ContentIdentity());
    }

    TEST_CASE("Latest request replaces pending work and shutdown cancels unadopted native baking") {
        BakeHarness harness;
        harness.builder->pause.store(true);
        const auto first = Submit(harness);
        for (std::size_t i = 0; i < 2000 && !harness.builder->entered.load(); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        REQUIRE(harness.builder->entered.load());
        CHECK(Submit(harness) == first);
        harness.fixture.ExcludeBorder();
        const auto second = Submit(harness);
        harness.fixture.ExcludeBorder(16);
        const auto third = Submit(harness);
        harness.builder->pause.store(false);
        CHECK(Terminal(*harness.service, harness.operations, first).state == OperationState::Cancelled);
        CHECK(Terminal(*harness.service, harness.operations, second).state == OperationState::Cancelled);
        CHECK(Terminal(*harness.service, harness.operations, third).state == OperationState::Succeeded);
        const auto last = harness.service->Published();
        harness.builder->pause.store(true);
        harness.builder->entered.store(false);
        harness.fixture.compatibility.provider = Digest(80);
        const auto closing = Submit(harness);
        for (std::size_t i = 0; i < 2000 && !harness.builder->entered.load(); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        REQUIRE(harness.builder->entered.load());
        harness.service->Close();
        CHECK(Terminal(*harness.service, harness.operations, closing).state == OperationState::Cancelled);
        CHECK(harness.service->Published() == last);
        CHECK(harness.service
                  ->Submit(
                      {.input = harness.fixture.Input(), .compatibility = harness.fixture.compatibility, .tiles = harness.fixture.Tiles()})
                  .HasError());
    }

    TEST_CASE("Malformed current authority fails incremental publication and preserves the last valid lease") {
        BakeHarness harness;
        REQUIRE(Terminal(*harness.service, harness.operations, Submit(harness)).state == OperationState::Succeeded);
        const auto before = harness.service->Published();
        {
            std::ofstream current(harness.config.targetRoot / "current.json", std::ios::trunc);
            current << "invalid";
        }
        harness.fixture.ExcludeBorder();
        CHECK(Terminal(*harness.service, harness.operations, Submit(harness)).state == OperationState::Failed);
        CHECK(harness.service->Published() == before);
        CHECK(Assets::ResolveCurrentCookGeneration(harness.config.targetRoot).HasError());
    }

    TEST_CASE("Source invalidation and replacement failure preserve current while post-rename failure reports committed truth") {
        auto files = std::make_shared<ControlledFiles>();
        BakeHarness harness(files);
        REQUIRE(Terminal(*harness.service, harness.operations, Submit(harness)).state == OperationState::Succeeded);
        const auto before = harness.service->Published();
        harness.fixture.ExcludeBorder();
        files->holdCurrent.store(true);
        files->currentStaged.store(false);
        const auto stale = Submit(harness);
        for (std::size_t i = 0; i < 2000 && !files->currentStaged.load(); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        const bool staged = files->currentStaged.load();
        harness.service->Invalidate();
        files->holdCurrent.store(false);
        REQUIRE(staged);
        CHECK(Terminal(*harness.service, harness.operations, stale).state == OperationState::Cancelled);
        CHECK(harness.service->Published() == before);
        CHECK(Assets::ResolveCurrentCookGeneration(harness.config.targetRoot).Value().manifestDigest == before->generation.manifestDigest);
        files->failReplacement.store(true);
        CHECK(Terminal(*harness.service, harness.operations, Submit(harness)).state == OperationState::Failed);
        CHECK(harness.service->Published() == before);
        CHECK(Assets::ResolveCurrentCookGeneration(harness.config.targetRoot).Value().manifestDigest == before->generation.manifestDigest);
        files->failReplacement.store(false);
        files->failAfterReplacement.store(true);
        REQUIRE(Terminal(*harness.service, harness.operations, Submit(harness)).state == OperationState::Succeeded);
        REQUIRE(harness.service->Published()->generation.durabilityError);
        CHECK(Assets::ResolveCurrentCookGeneration(harness.config.targetRoot).Value().manifestDigest ==
              harness.service->Published()->generation.manifestDigest);
    }

    TEST_CASE("Production cache reuse rejects valid foreign source envelopes and unchanged producer digests cannot hide edits") {
        BakeHarness harness;
        REQUIRE(Terminal(*harness.service, harness.operations, Submit(harness)).state == OperationState::Succeeded);
        harness.fixture.vertices.front().y = 0.2F;
        REQUIRE(Terminal(*harness.service, harness.operations, Submit(harness)).state == OperationState::Succeeded);
        CHECK(harness.service->Published()->rebuiltTiles == 2);
        CHECK(harness.service->Published()->reusedTiles == 2);
        const auto before = harness.service->Published();
        harness.service->Close();
        for (const auto &entry : std::filesystem::recursive_directory_iterator(harness.config.cacheRoot)) {
            if (entry.path().extension() != ".cooked")
                continue;
            std::ifstream file(entry.path(), std::ios::binary);
            std::vector<std::uint8_t> bytes{std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
            auto decoded = Assets::DecodeCookedArtifact(bytes);
            REQUIRE(decoded.HasValue());
            auto envelope = std::move(decoded).Value();
            envelope.sourceDigest = Digest(90);
            auto encoded = Assets::EncodeCookedArtifact(envelope).Value();
            file.close();
            std::ofstream output(entry.path(), std::ios::binary | std::ios::trunc);
            output.write(reinterpret_cast<const char *>(encoded.data()), static_cast<std::streamsize>(encoded.size()));
        }
        harness.service = NavigationBakeService::Create(harness.config, harness.operations, harness.jobs).Value();
        CHECK(Terminal(*harness.service, harness.operations, Submit(harness)).state == OperationState::Failed);
        CHECK_FALSE(harness.service->Published());
        CHECK(Assets::ResolveCurrentCookGeneration(harness.config.targetRoot).Value().manifestDigest == before->generation.manifestDigest);
    }

    TEST_CASE("Locked incremental publication preserves every unrelated artifact and rejects another writer") {
        BakeHarness harness;
        const auto otherId = Assets::AssetId::Parse("00000000-0000-0000-0000-000000000002").Value();
        const auto otherType = Assets::AssetTypeId::Parse("core.mesh").Value();
        const std::vector<std::uint8_t> payload{1, 2, 3};
        const auto artifact = Assets::EncodeCookedArtifact({.id = otherId,
                                                            .type = otherType,
                                                            .target = harness.config.target,
                                                            .cacheKeyDigest = Digest(70),
                                                            .sourceDigest = Digest(71),
                                                            .payloadDigest = ComputeSha256(std::as_bytes(std::span{payload})),
                                                            .payload = payload})
                                  .Value();
        const Assets::AssetCookManifestEntry other{.assetId = otherId,
                                                   .assetType = otherType,
                                                   .artifactFile = harness.config.definition.ToString() + ".cooked",
                                                   .artifactHash = ComputeSha256(std::as_bytes(std::span{artifact}))};
        PublishLegacyArtifact(harness, other, artifact);
        const auto legacy = Assets::ResolveCurrentCookGeneration(harness.config.targetRoot).Value();
        REQUIRE(Assets::ReadCookGenerationContents(legacy, harness.config.maximumCandidateBytes).HasValue());
        REQUIRE(Terminal(*harness.service, harness.operations, Submit(harness)).state == OperationState::Succeeded);
        const auto before = harness.service->Published();
        harness.fixture.ExcludeBorder();
        {
            auto lock = harness.config.files->TryAcquireExclusive(harness.config.targetRoot / ".cook-writer.lock", "competing test writer");
            REQUIRE(lock.HasValue());
            CHECK(Terminal(*harness.service, harness.operations, Submit(harness)).state == OperationState::Failed);
            CHECK(harness.service->Published() == before);
            CHECK(Assets::ResolveCurrentCookGeneration(harness.config.targetRoot).Value().manifestDigest ==
                  before->generation.manifestDigest);
        }
        REQUIRE(Terminal(*harness.service, harness.operations, Submit(harness)).state == OperationState::Succeeded);
        const auto current = Assets::ResolveCurrentCookGeneration(harness.config.targetRoot).Value();
        const auto read = Assets::ReadCookGenerationContents(current, harness.config.maximumCandidateBytes);
        REQUIRE(read.HasValue());
        const auto &contents = read.Value();
        REQUIRE(contents.entries.size() == 2);
        CHECK(contents.entries.back().assetId == otherId);
        CHECK(contents.entries.back().artifactHash == other.artifactHash);
        CHECK(contents.artifacts.back() == artifact);
        CHECK(contents.entries.back().artifactFile == otherId.ToString() + ".cooked");
        CHECK(Assets::ReadCookGenerationContents(legacy, harness.config.maximumCandidateBytes).Value().artifacts.front() == artifact);
    }

    TEST_CASE("Artifact replacement rejects a noncanonical filename without changing the current authority") {
        BakeHarness harness;
        REQUIRE(Terminal(*harness.service, harness.operations, Submit(harness)).state == OperationState::Succeeded);
        const auto before = harness.service->Published();
        const auto contents = Assets::ReadCookGenerationContents(before->generation, harness.config.maximumCandidateBytes).Value();
        auto entry = contents.entries.front();
        entry.artifactFile = "foreign.cooked";
        CHECK(Assets::PublishCookArtifactReplacement(harness.config.targetRoot, harness.config.target, entry, contents.artifacts.front(),
                                                     harness.config.maximumCandidateBytes, harness.config.cookLimits,
                                                     {.files = harness.config.files.get(), .newOperationId = harness.config.newOperationId})
                  .HasError());
        CHECK(Assets::ResolveCurrentCookGeneration(harness.config.targetRoot).Value().manifestDigest == before->generation.manifestDigest);
    }

    TEST_CASE("Removing all walkable geometry publishes a complete empty tile closure without retaining old topology") {
        BakeHarness harness;
        REQUIRE(Terminal(*harness.service, harness.operations, Submit(harness)).state == OperationState::Succeeded);
        const auto before = harness.service->Published();
        harness.fixture.ExcludeBorder();
        harness.fixture.modifiers.front().localBounds = {{-1, -1, -1}, {33, 3, 9}};
        REQUIRE(Terminal(*harness.service, harness.operations, Submit(harness)).state == OperationState::Succeeded);
        const auto after = harness.service->Published();
        REQUIRE(after->tiles.tiles.size() == 4);
        CHECK(after->rebuiltTiles == 4);
        CHECK(after->reusedTiles == 0);
        for (const auto &tile : after->tiles.tiles) {
            CHECK(tile->Topology().IsEmpty());
            CHECK(tile->Topology().polygons.empty());
        }
        CHECK(Query(before->tiles).Value().status == NavigationPathStatus::Reachable);
        const auto contents = Assets::ReadCookGenerationContents(after->generation, harness.config.maximumCandidateBytes).Value();
        const auto envelope = Assets::DecodeCookedArtifact(contents.artifacts.front()).Value();
        const auto decoded = DecodeNavigationCookedTileSet(envelope.payload, harness.config.maximumCandidateBytes).Value();
        CHECK(std::ranges::all_of(decoded.tiles, [](const auto &tile) {
            return tile->Topology().IsEmpty();
        }));
    }
}  // namespace Horo::Application
