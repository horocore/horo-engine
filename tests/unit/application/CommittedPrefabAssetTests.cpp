#include "CommittedSceneAssets.h"
#include "Horo/Assets/NavMeshAssetType.h"
#include "Horo/Runtime/Scene/RuntimeScene.h"
#include "PrefabSceneCookHostFixture.h"

#include <algorithm>
#include <array>
#include <span>
#include <string_view>

namespace Horo::Application::PrefabCookTestSupport {
    namespace {
        /** @brief Publishes verified HCAF envelopes with distinct types; payload semantics belong to each downstream decoder. */
        struct RoutingPublication final {
            struct Route final {
                std::string_view type;
                std::string_view extension;
                bool sceneEnvelope;
            };

            static constexpr std::array routes{
                Route{"core.animation.skeleton", ".skeleton", true},
                Route{"core.animation.clip", ".clip", true},
                Route{"core.animation.graph", ".graph", true},
                Route{Assets::NavMeshAssetTypeName, ".navmesh", true},
                Route{"core.scene", ".scene", false},
                Route{"core.animation.graph.extra", ".extra", false},
                Route{"core.navmesh.extra", ".extra", false},
                Route{"core.prefab", ".prefab", false},
            };

            TempDir root;
            NativeDurableFileSystem files;
            AssetRegistry registry;
            const AssetCookTargetId target = Target("headless-null");
            std::vector<Assets::AssetCookManifestEntry> entries;
            std::vector<std::vector<std::uint8_t>> envelopes;

            RoutingPublication() {
                std::vector<AssetRecord> records;
                for (std::size_t index = 0; index < routes.size(); ++index) {
                    const auto &route = routes[index];
                    std::array<std::uint8_t, 16> bytes{};
                    bytes.back() = static_cast<std::uint8_t>(index + 1);
                    const auto id = AssetId::FromBytes(bytes);
                    const auto type = Type(route.type);
                    const std::vector<std::uint8_t> payload{static_cast<std::uint8_t>(index + 10), 0, 255};
                    const auto digest = ComputeSha256(std::as_bytes(std::span(payload)));
                    auto encoded = Assets::EncodeCookedArtifact({id, type, target, digest, digest, digest, payload});
                    REQUIRE(encoded.HasValue());
                    entries.push_back({id, type, id.ToString() + ".cooked", ComputeSha256(std::as_bytes(std::span(encoded.Value())))});
                    envelopes.push_back(std::move(encoded).Value());
                    const auto path = "assets/" + id.ToString() + std::string{route.extension};
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

    TEST_CASE("The project cook pins direct child Scenes and committed lookup rejects mixed revisions",
              "[native][prefab-cook][host][scene-instances]") {
        HostFixture fixture;
        const auto child = Id("00000000-0000-0000-0000-0000000000c2");
        const auto childPath = ProjectPath::Parse("assets/child.scene").Value();
        const auto metadataPath = ProjectPath::Parse("assets/child.scene.horo").Value();
        const auto original = fixture.registry.Snapshot();
        const auto existing = original.Records();
        std::vector<AssetRecord> registered(existing.begin(), existing.end());
        registered.push_back({.id = child, .type = Type("core.scene"), .sourcePath = childPath, .metadataPath = metadataPath});
        REQUIRE(fixture.registry.Publish(std::move(registered)).status == AssetRegistryBuildStatus::Complete);
        fixture.WriteText(metadataPath.String(), SidecarJson(child.ToString(), "core.scene"));
        const auto writeChild = [&](float offset, std::span<const SceneSource::SceneChildReference> children = {}) {
            const std::array objects{
                SceneSource::SceneObjectSnapshot{.id = {700}, .name = "Child", .localTransform = {.translation = {offset, 0, 0}}}};
            fixture.WriteText(childPath.String(), SceneSource::EncodeSceneSource({objects, {}, children}));
        };
        const auto writeParent = [&](std::span<const SceneSource::SceneChildReference> children) {
            const std::array objects{SceneSource::SceneObjectSnapshot{.id = {900}, .name = "Parent"}};
            fixture.WriteText("assets/level.scene", SceneSource::EncodeSceneSource({objects, {}, children}));
        };
        writeChild(2.0F);
        const std::array children{SceneSource::SceneChildReference{{27}, child}, SceneSource::SceneChildReference{{4}, child}};
        writeParent(children);

        const auto first = fixture.Cook();
        REQUIRE(first.HasValue());
        const auto old = Capture(fixture);
        const auto parent = old->DecodeScene(fixture.sceneId);
        REQUIRE(parent.HasValue());
        REQUIRE(parent.Value().ChildScenes().size() == 2);
        CHECK(parent.Value().ChildScenes().front().occurrence == Runtime::SceneInstanceId{4});
        CHECK(parent.Value().ChildScenes().back().occurrence == Runtime::SceneInstanceId{27});
        const auto oldChild = parent.Value().ChildScenes().front();
        CHECK(oldChild.asset == child);
        CHECK(oldChild.definition == SceneCook::SceneDefinitionIdForAsset(child));
        const auto leaf = old->DecodeChildScene(oldChild);
        REQUIRE(leaf.HasValue());
        CHECK(leaf.Value().Revision() == oldChild.revision);
        REQUIRE(leaf.Value().Entities().size() == 1);
        CHECK(leaf.Value().Entities().front().localTransform.translation.x == 2.0F);
        CHECK(parent.Value().AssetDependencies().empty());
        CHECK(Runtime::RuntimeScene::Create(parent.Value(), {9}).HasError());

        const auto cached = fixture.Cook();
        REQUIRE(cached.HasValue());
        CHECK(cached.Value().generation.manifestDigest == first.Value().generation.manifestDigest);
        writeChild(6.0F);
        const auto next = fixture.Cook();
        REQUIRE(next.HasValue());
        const auto current = Capture(fixture);
        const auto recooked = current->DecodeScene(fixture.sceneId);
        REQUIRE(recooked.HasValue());
        CHECK(recooked.Value().Revision() != parent.Value().Revision());
        REQUIRE(recooked.Value().ChildScenes().size() == 2);
        const auto newChild = recooked.Value().ChildScenes().front();
        CHECK(newChild.revision != oldChild.revision);
        CHECK(newChild.payloadDigest != oldChild.payloadDigest);
        CHECK(current->DecodeChildScene(oldChild).HasError());
        CHECK(old->DecodeChildScene(newChild).HasError());
        const auto updated = current->DecodeChildScene(newChild);
        REQUIRE(updated.HasValue());
        REQUIRE(updated.Value().Entities().size() == 1);
        CHECK(updated.Value().Entities().front().localTransform.translation.x == 6.0F);

        SECTION("missing child") {
            const std::array missing{SceneSource::SceneChildReference{{4}, Id("00000000-0000-0000-0000-0000000000c3")}};
            writeParent(missing);
        }
        SECTION("wrong child asset type") {
            const std::array wrong{SceneSource::SceneChildReference{{4}, fixture.prefabId}};
            writeParent(wrong);
        }
        SECTION("self reference") {
            const std::array self{SceneSource::SceneChildReference{{4}, fixture.sceneId}};
            writeParent(self);
        }
        SECTION("cycle between authored Scenes") {
            const std::array back{SceneSource::SceneChildReference{{4}, fixture.sceneId}};
            writeChild(6.0F, back);
        }
        REQUIRE(fixture.Cook().HasError());
        fixture.AssertRetained(next.Value().generation);
    }

    TEST_CASE("Committed Scene view retains exact Animation and NavMesh envelopes for their canonical decoders",
              "[animation][navigation][host][committed][provider]") {
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
            const auto &route = RoutingPublication::routes[index];
            CAPTURE(route.type);
            CHECK(scene.Value() == (route.sceneEnvelope ? envelope.Value() : payload.Value()));
            if (route.sceneEnvelope) {
                const auto decoded = Assets::DecodeCookedArtifact(scene.Value());
                REQUIRE(decoded.HasValue());
                CHECK(decoded.Value().id == id);
                CHECK(decoded.Value().type == publication.entries[index].assetType);
                CHECK(decoded.Value().target == publication.target);
                CHECK(ComputeSha256(std::as_bytes(std::span(scene.Value()))) == publication.entries[index].artifactHash);
            }
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
