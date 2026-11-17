#include "CommittedSceneAssets.h"
#include "PrefabSceneCookHostFixture.h"

#include <algorithm>

namespace Horo::Application::PrefabCookTestSupport {
    namespace {
        /** @brief Publishes verified HCAF envelopes with distinct types; payload semantics belong to each downstream decoder. */
        struct RoutingPublication final {
            TempDir root;
            NativeDurableFileSystem files;
            AssetRegistry registry;
            const AssetCookTargetId target = Target("headless-null");
            std::vector<Assets::AssetCookManifestEntry> entries;
            std::vector<std::vector<std::uint8_t>> envelopes;

            RoutingPublication() {
                const std::array types{"core.animation.skeleton",    "core.animation.clip", "core.animation.graph", "core.scene",
                                       "core.animation.graph.extra", "core.prefab"};
                const std::array extensions{".skeleton", ".clip", ".graph", ".scene", ".extra", ".prefab"};
                std::vector<AssetRecord> records;
                for (std::size_t index = 0; index < types.size(); ++index) {
                    std::array<std::uint8_t, 16> bytes{};
                    bytes.back() = static_cast<std::uint8_t>(index + 1);
                    const auto id = AssetId::FromBytes(bytes);
                    const auto type = Type(types[index]);
                    const std::vector<std::uint8_t> payload{static_cast<std::uint8_t>(index + 10), 0, 255};
                    const auto digest = ComputeSha256(std::as_bytes(std::span(payload)));
                    auto encoded = Assets::EncodeCookedArtifact({id, type, target, digest, digest, digest, payload});
                    REQUIRE(encoded.HasValue());
                    entries.push_back({id, type, id.ToString() + ".cooked", ComputeSha256(std::as_bytes(std::span(encoded.Value())))});
                    envelopes.push_back(std::move(encoded).Value());
                    const auto path = "assets/" + id.ToString() + extensions[index];
                    records.push_back({.id = id,
                                       .type = type,
                                       .sourcePath = ProjectPath::Parse(path).Value(),
                                       .metadataPath = ProjectPath::Parse(path + ".horo").Value()});
                }
                REQUIRE(registry.Publish(std::move(records)).status == AssetRegistryBuildStatus::Complete);
                auto lease = files.TryAcquireExclusive(root.path / ".cook-writer.lock", "Scene envelope routing test");
                REQUIRE(lease.HasValue());
                const Assets::AssetCookPublicationPolicy policy{.files = &files,
                                                                .operationId = entries.front().assetId.ToString(),
                                                                .writerLease = &lease.Value()};
                REQUIRE(Assets::PublishCookGeneration(root.path, target, entries, envelopes, {}, policy).HasValue());
            }

            std::unique_ptr<Internal::CommittedSceneAssets> Capture() const {
                auto captured = Internal::CommittedSceneAssets::Capture(files, {root.path, target}, registry.Snapshot());
                REQUIRE(captured.HasValue());
                return std::move(captured).Value();
            }
        };

        /** @brief Pins actual canonical envelopes from the same complete publication as the decoded Scene resources. */
        std::unique_ptr<Internal::CommittedSceneAssets> Capture(const HostFixture &fixture) {
            auto captured =
                Internal::CommittedSceneAssets::Capture(Horo::NativeDurableFileSystem{},
                                                        {fixture.cooked.path, fixture.request.assets.target}, fixture.registry.Snapshot());
            REQUIRE(captured.HasValue());
            return std::move(captured).Value();
        }
    }  // namespace

    TEST_CASE("Committed Scene view routes exactly three Animation types without changing legacy payloads",
              "[animation][host][committed][provider]") {
        RoutingPublication publication;
        const auto captured = publication.Capture();
        for (std::size_t index = 0; index < publication.entries.size(); ++index) {
            const auto id = publication.entries[index].assetId;
            const auto *artifact = captured->Find(id);
            REQUIRE(artifact);
            const auto payload = captured->Provider().Load(id, {});
            const auto scene = captured->SceneProvider().Load(id, {});
            const auto envelope = captured->ArtifactProvider().Load(id, {});
            REQUIRE(payload.HasValue());
            REQUIRE(scene.HasValue());
            REQUIRE(envelope.HasValue());
            CHECK(payload.Value() == artifact->payload);
            CHECK(envelope.Value() == publication.envelopes[index]);
            CHECK(scene.Value() == (index < 3 ? envelope.Value() : payload.Value()));
            CHECK(captured->SceneProvider().Exists(id, {}).Value());
        }
    }

    TEST_CASE("Committed Scene routing preserves provider cancellation and absence for both routes",
              "[animation][host][committed][provider]") {
        RoutingPublication publication;
        const auto captured = publication.Capture();
        CancellationSource cancelled;
        cancelled.RequestCancellation();
        for (const auto &entry : publication.entries) {
            CHECK(captured->SceneProvider().Load(entry.assetId, cancelled.Token()).HasError());
            CHECK(captured->SceneProvider().Exists(entry.assetId, cancelled.Token()).HasError());
        }
        const auto missing = Id("00000000-0000-0000-0000-000000000099");
        const auto exists = captured->SceneProvider().Exists(missing, {});
        REQUIRE(exists.HasValue());
        CHECK_FALSE(exists.Value());
        CHECK(captured->SceneProvider().Load(missing, {}).HasError());
    }

    TEST_CASE("Committed prefab app view preserves original framing while the Scene provider keeps decoded payloads",
              "[prefab][host][committed][provider]") {
        HostFixture fixture;
        fixture.request.runtimePrefabRoots = {fixture.prefabId};
        REQUIRE(fixture.Cook().HasValue());
        const auto generation = ResolveCurrentCookGeneration(Horo::NativeDurableFileSystem{}, fixture.cooked.path);
        REQUIRE(generation.HasValue());
        const auto contents = ReadCookGenerationContents(Horo::NativeDurableFileSystem{}, generation.Value(), 1024U * 1024U);
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
        const auto current = ResolveCurrentCookGeneration(Horo::NativeDurableFileSystem{}, fixture.cooked.path);
        REQUIRE(current.HasValue());
        WriteFile(current.Value().generationRoot / (fixture.prefabId.ToString() + ".cooked"), std::array<std::uint8_t, 3>{1, 2, 3});
        CHECK(Internal::CommittedSceneAssets::Capture(Horo::NativeDurableFileSystem{}, {fixture.cooked.path, fixture.request.assets.target},
                                                      fixture.registry.Snapshot())
                  .HasError());
        CHECK(captured->ArtifactProvider().Load(fixture.prefabId, {}).Value() == before.Value());
        const auto wrongTarget = Target("other-target");
        CHECK(Internal::CommittedSceneAssets::Capture(Horo::NativeDurableFileSystem{}, {fixture.cooked.path, wrongTarget},
                                                      fixture.registry.Snapshot())
                  .HasError());
    }
}  // namespace Horo::Application::PrefabCookTestSupport
