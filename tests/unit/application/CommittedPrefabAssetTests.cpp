#include "CommittedSceneAssets.h"
#include "PrefabSceneCookHostFixture.h"

#include <algorithm>

namespace Horo::Application::PrefabCookTestSupport {
    namespace {
        /** @brief Pins actual canonical envelopes from the same complete publication as the decoded Scene resources. */
        std::unique_ptr<Internal::CommittedSceneAssets> Capture(const HostFixture &fixture) {
            auto captured =
                Internal::CommittedSceneAssets::Capture({fixture.cooked.path, fixture.request.assets.target}, fixture.registry.Snapshot());
            REQUIRE(captured.HasValue());
            return std::move(captured).Value();
        }
    }  // namespace

    TEST_CASE("Committed prefab app view preserves original framing while the Scene provider keeps decoded payloads",
              "[prefab][host][committed][provider]") {
        HostFixture fixture;
        fixture.request.runtimePrefabRoots = {fixture.prefabId};
        REQUIRE(fixture.Cook().HasValue());
        const auto generation = ResolveCurrentCookGeneration(fixture.cooked.path);
        REQUIRE(generation.HasValue());
        const auto contents = ReadCookGenerationContents(generation.Value(), 1024U * 1024U);
        REQUIRE(contents.HasValue());
        const auto found = std::ranges::find(contents.Value().entries, fixture.prefabId, &Assets::AssetCookManifestEntry::assetId);
        REQUIRE(found != contents.Value().entries.end());
        const auto index = static_cast<std::size_t>(found - contents.Value().entries.begin());
        const auto captured = Capture(fixture);
        const auto original = captured->ArtifactProvider().Load(fixture.prefabId, {});
        const auto payload = captured->Provider().Load(fixture.prefabId, {});
        REQUIRE(original.HasValue());
        REQUIRE(payload.HasValue());
        CHECK(original.Value() == contents.Value().artifacts[index]);
        CHECK(ComputeSha256(std::as_bytes(std::span(original.Value()))) == found->artifactHash);
        REQUIRE(captured->Find(fixture.prefabId));
        CHECK(payload.Value() == captured->Find(fixture.prefabId)->payload);
        CHECK(payload.Value() != original.Value());
        const auto decoded = Assets::DecodeCookedArtifact(original.Value());
        REQUIRE(decoded.HasValue());
        CHECK(decoded.Value().payload == payload.Value());
        CancellationSource cancelled;
        cancelled.RequestCancellation();
        CHECK(captured->ArtifactProvider().Load(fixture.prefabId, cancelled.Token()).HasError());
    }

    TEST_CASE("Pinned committed prefab bytes survive generation replacement and reject later corrupt framing",
              "[prefab][host][committed][lifetime]") {
        HostFixture fixture;
        fixture.request.runtimePrefabRoots = {fixture.prefabId};
        REQUIRE(fixture.Cook().HasValue());
        const auto captured = Capture(fixture);
        const auto before = captured->ArtifactProvider().Load(fixture.prefabId, {});
        REQUIRE(before.HasValue());
        fixture.WritePrefab(8);
        REQUIRE(fixture.Cook().HasValue());
        const auto replacement = Capture(fixture);
        const auto after = replacement->ArtifactProvider().Load(fixture.prefabId, {});
        REQUIRE(after.HasValue());
        CHECK(after.Value() != before.Value());
        CHECK(captured->ArtifactProvider().Load(fixture.prefabId, {}).Value() == before.Value());
        const auto current = ResolveCurrentCookGeneration(fixture.cooked.path);
        REQUIRE(current.HasValue());
        WriteFile(current.Value().generationRoot / (fixture.prefabId.ToString() + ".cooked"), std::array<std::uint8_t, 3>{1, 2, 3});
        CHECK(Internal::CommittedSceneAssets::Capture({fixture.cooked.path, fixture.request.assets.target}, fixture.registry.Snapshot())
                  .HasError());
        CHECK(captured->ArtifactProvider().Load(fixture.prefabId, {}).Value() == before.Value());
        const auto wrongTarget = Target("other-target");
        CHECK(Internal::CommittedSceneAssets::Capture({fixture.cooked.path, wrongTarget}, fixture.registry.Snapshot()).HasError());
    }
}  // namespace Horo::Application::PrefabCookTestSupport
