#pragma once

#include "Horo/Application/NavigationProjectSettings.h"
#include "Horo/Navigation/NavigationDefinitionSerialization.h"
#include "Horo/Navigation/NavigationMeshBuilder.h"
#include "Horo/Prefab/CookedPrefab.h"
#include "Horo/Scene/CookedSceneDefinition.h"
#include "Horo/Scene/PrefabComponentProjection.h"
#include "Horo/Scene/SceneRuntimeConversion.h"
#include "PrefabSceneCookHostFixture.h"
#include "navigation/NavigationContentPolicyFixture.h"

#include <algorithm>
#include <functional>

namespace Horo::Application::CharacterSceneProductTest {
    using namespace PrefabCookTestSupport;

    /** @brief Publishes a real scoped NavMesh envelope and canonical project policy for the terminal application. */
    struct NavigationProductFixture final {
        using NavScope = Navigation::NavigationBakeScopeId;
        enum class Input {
            Valid,
            MissingSurface,
            ForeignScope,
            StalePolicy,
            UnsupportedLink
        };
        enum class Client {
            None,
            Scene,
            Prefab
        };
        HostFixture project;
        SceneSource::SceneSourceDocument source;
        const Navigation::NavigationFilterId pathFilter{Navigation::NavigationFilterId::Create(13).Value()};
        const Assets::AssetId definitionId{Id("11223344-5566-7788-99aa-bbccddeeff88")};

        Sha256Digest prefabDigest;

        struct AddedAsset final {
            Assets::AssetRecord record;
            std::vector<std::uint8_t> payload;
            std::vector<std::uint8_t> cookedEnvelope; /**< Complete canonical envelope when already produced by its real cooker. */
        };

        void Publish(const Input input, const Client client = Client::None, std::span<const AddedAsset> added = {},
                     const std::function<void(SceneSource::SceneSourceDocument &)> &editScene = {},
                     std::optional<Runtime::SceneAssetDependency> explicitDependency = std::nullopt,
                     const Navigation::INavigationMeshBuilder *tileBuilder = nullptr) {
            using namespace Navigation;
            const auto profile = NavigationAgentProfileId::Create(7).Value();
            const auto surface = SurfaceId::Create(53).Value();
            const auto area = NavigationAreaId::Create(11).Value();
            const auto filter = pathFilter;
            const auto origin = NavigationDescriptorSource{.id = NavigationDescriptorSourceId::Create(2).Value()};
            const NavigationAgentBuildGeometry geometry{.cellSizeMeters = 0.5F};
            auto definition = NavigationDefinition::Create({.profiles = {{profile, "Grounded", geometry}},
                                                            .areas = {{area, origin, 1.0F, {.bits = 1}}},
                                                            .filters = {{filter, origin, {.bits = 1}}}});
            REQUIRE(definition.HasValue());
            const auto record = EncodeNavigationDefinitionRecord(definition.Value(), NavigationAuthoredRecordId::Create(1).Value());
            REQUIRE(record.HasValue());
            const std::array support{NavigationDefinitionRecordSupport()};
            const auto records =
                NavigationSourceRecords::Create(CurrentNavigationSourceSchemaVersion, {record.Value()}, {.supportedRecords = support});
            REQUIRE(records.HasValue());
            const auto sourceBytes = SerializeNavigationSourceRecords(records.Value());
            REQUIRE(sourceBytes.HasValue());
            project.WriteText("assets/nav.horoasset",
                              {reinterpret_cast<const char *>(sourceBytes.Value().data()), sourceBytes.Value().size()});
            project.WriteText("assets/nav.horoasset.horo", SidecarJson(definitionId.ToString(), "core.navmesh"));
            std::filesystem::rename(project.project.dir.path / "assets/level.scene", project.project.dir.path / "assets/level.horo");
            std::filesystem::rename(project.project.dir.path / "assets/level.scene.horo",
                                    project.project.dir.path / "assets/level.horo.horo");
            std::vector recordsToPublish{AssetRecordFor(project.sceneId, "core.scene", "assets/level.horo"),
                                         AssetRecordFor(definitionId, "core.navmesh", "assets/nav.horoasset")};
            for (const auto &asset : added)
                recordsToPublish.push_back(asset.record);
            REQUIRE(project.registry.Publish(std::move(recordsToPublish)).status == Assets::AssetRegistryBuildStatus::Complete);

            NavigationProjectProfileInput policy{.id = NavigationProjectProfileId::Create(5).Value(),
                                                 .revision = NavigationProjectProfileRevision::Create(3).Value(),
                                                 .capacities = {8, 1, 4, 2, 65536, 16U * 1024U * 1024U, 8192},
                                                 .maximumQuery = {NavigationQueryKind::Path,
                                                                  NavigationQualityLevel::Balanced,
                                                                  {64, 16, 100}}};
            policy.capabilities.fill(NavigationCapabilityRequirement::Optional);
            policy.capabilities[static_cast<std::size_t>(NavigationCapability::GroundedQueries)] =
                NavigationCapabilityRequirement::Required;
            const auto settings = NavigationProjectSettings::Create(
                {.revision = 4, .definition = definitionId, .defaultProvider = "recast_detour", .runtimeProfile = policy});
            REQUIRE(settings.HasValue());
            project.WriteText(".horo/navigation.json", EncodeNavigationProjectSettings(settings.Value()));

            const auto scope = NavScope::ForScene((input == Input::ForeignScope ? definitionId : project.sceneId).Bytes()).Value();
            const auto digest = ComputeSha256(std::as_bytes(std::span{"nav product fixture", std::size_t{19}}));
            const NavigationPreparedTile tile{.tile = {.key = {profile, surface, {}, scope},
                                                       .bounds = {{0, -1, 0}, {10, 1, 10}},
                                                       .tileSizeMeters = 10},
                                              .geometry = geometry,
                                              .borderSizeCells = 4,
                                              .dependencyKey = digest};
            auto topology = NavigationTileBuildResult{
                .state = NavigationTileBuildState::Built,
                .bounds = tile.tile.bounds,
                .vertices = {{0, 0, 0}, {10, 0, 0}, {0, 0, 10}},
                .polygons = {{{0, 3}, {0, 3}, area}},
                .polygonVertexIndices = {0, 1, 2},
                .polygonAdjacencies = {NavMeshBoundaryAdjacency, NavMeshBoundaryAdjacency, NavMeshBoundaryAdjacency},
                .provenance = {{.producer = NavigationSourceProducerId::Create(1).Value(),
                                .contribution = NavigationSourceContributionId::Create(1).Value(),
                                .revision = NavigationSourceRevision::Create(1).Value(),
                                .sourceDigest = digest,
                                .polygons = {0, 1}}}};
            if (tileBuilder)
                topology = BakeGroundTile(*tileBuilder, tile, area);
            const auto cookedTile = NavigationCookedTile::Create(tile, std::move(topology));
            CAPTURE((cookedTile.HasError() ? cookedTile.ErrorValue().message : "encoded"));
            REQUIRE(cookedTile.HasValue());
            if (input == Input::StalePolicy)
                policy.revision = NavigationProjectProfileRevision::Create(1).Value();
            const NavigationCookedTileSet set{digest,
                                              {cookedTile.Value()},
                                              NavigationCookedContentProvenance{{digest, digest, digest},
                                                                                NavigationProjectProfile::Create(policy).Value()}};
            source.objects.push_back({.id = {1}, .name = "Play camera", .components = {.camera = Runtime::CameraComponent{}}});
            if (client == Client::Scene)
                source.objects.front().components.behaviors.push_back(NavigationClient(false));
            else if (client == Client::Prefab)
                source.objects.front().components.behaviors.push_back(
                    {{1}, Gameplay::BehaviorTypeId::Parse("game.tests.prefab_capability_probe").Value()});
            if (input != Input::MissingSurface) {
                source.objects.push_back(
                    {.id = {2},
                     .name = "Navigation surface",
                     .components = {.navigationSurface = Runtime::NavigationSurfaceComponent{.id = surface,
                                                                                             .definition = definitionId,
                                                                                             .generation = 1,
                                                                                             .profiles = {profile}}}});
                source.objects.push_back(
                    {.id = {3},
                     .name = "Navigation agent",
                     .components = {.navigationAgent = Runtime::NavigationAgentComponent{.profile = profile, .filter = filter}}});
            }
            if (input == Input::UnsupportedLink)
                source.objects.push_back(
                    {.id = {4},
                     .name = "Unsupported link",
                     .components = {.navigationLink =
                                        Runtime::NavigationLinkComponent{.id = NavigationLinkId::Create(5).Value(),
                                                                         .start = {.surface = surface},
                                                                         .end = {.surface = surface, .localPosition = {2, 0, 2}},
                                                                         .profiles = {profile}}}});
            if (editScene)
                editScene(source);
            const SceneSource::SceneSourceView view{source.objects, source.prefabInstances};
            project.WriteText("assets/level.horo", SceneSource::EncodeSceneSource(view));
            auto scene = SceneSource::ConvertSceneSourceToRuntime(view, SceneCook::SceneDefinitionIdForAsset(project.sceneId), {1});
            REQUIRE(scene.HasValue());
            if (explicitDependency)
                scene = WithRequiredAsset(scene.Value(), *explicitDependency);
            REQUIRE(scene.HasValue());
            auto prefab = client == Client::Prefab ? EncodeNavigationPrefab(profile) : std::vector<std::uint8_t>{};
            PublishCook(Navigation::TestSupport::ContentEnvelope(set, definitionId, project.request.assets.target), scene.Value(),
                        std::move(prefab), added);
        }

    private:
        static Navigation::NavigationTileBuildResult BakeGroundTile(const Navigation::INavigationMeshBuilder &builder,
                                                                  const Navigation::NavigationPreparedTile &tile,
                                                                  const Navigation::NavigationAreaId area) {
            using namespace Navigation;
            const NavigationTriangleProvenance first{.kind = NavigationSourceProducerKind::StaticCollider,
                                                     .producer = NavigationSourceProducerId::Create(1).Value(),
                                                     .contribution = NavigationSourceContributionId::Create(1).Value(),
                                                     .revision = NavigationSourceRevision::Create(1).Value(),
                                                     .contentDigest = tile.dependencyKey,
                                                     .sourceTriangleIndex = 0};
            auto second = first;
            second.sourceTriangleIndex = 1;
            const std::array triangles{
                NavigationTileBuildTriangle{.vertices = {{{0, 0, 0}, {10, 0, 10}, {10, 0, 0}}},
                                            .area = area,
                                            .traversalCost = 1,
                                            .materialSlot = {.value = 1},
                                            .provenance = first},
                NavigationTileBuildTriangle{.vertices = {{{0, 0, 0}, {0, 0, 10}, {10, 0, 10}}},
                                            .area = area,
                                            .traversalCost = 1,
                                            .materialSlot = {.value = 1},
                                            .provenance = second}};
            auto baked = builder.BuildTile({.key = tile.tile.key.tile,
                                            .bounds = tile.tile.bounds,
                                            .tileSizeMeters = tile.tile.tileSizeMeters,
                                            .buildGeometry = tile.geometry,
                                            .triangles = triangles,
                                            .borderSizeCells = tile.borderSizeCells}, {});
            if (baked.HasError())
                UNSCOPED_INFO(baked.ErrorValue().code.Value() << ": " << baked.ErrorValue().message);
            REQUIRE(baked.HasValue());
            CHECK(baked.Value().statistics.walkableTriangleCount == triangles.size());
            REQUIRE_FALSE(baked.Value().IsEmpty());
            return std::move(baked).Value();
        }

        static Result<Runtime::RuntimeSceneDefinition> WithRequiredAsset(const Runtime::RuntimeSceneDefinition &scene,
                                                                         const Runtime::SceneAssetDependency &dependency) {
            Runtime::SceneDefinitionBuilder builder{scene.Id(), scene.Revision()};
            for (const auto &entity : scene.Entities())
                builder.Add(entity);
            for (const auto &asset : scene.AssetDependencies())
                REQUIRE(builder.RequireAsset(asset).HasValue());
            for (const auto &trigger : scene.TriggerVolumeEvidence())
                REQUIRE(builder.RequireTriggerVolume(trigger).HasValue());
            for (const auto &child : scene.ChildScenes())
                REQUIRE(builder.RequireChildScene(child).HasValue());
            REQUIRE(builder.RequireAsset(dependency).HasValue());
            return std::move(builder).Build();
        }

        /** @brief One real native callback, used by either ordinary or cooked prefab Scene composition. */
        static Gameplay::BehaviorComponent NavigationClient(const bool capture) {
            return {{1},
                    Gameplay::BehaviorTypeId::Parse("game.tests.navigation_client").Value(),
                    1,
                    true,
                    {{"destinationX", 2.0}, {"destinationZ", 2.0}, {"captureSubmission", capture}}};
        }

        /** @brief Uses the canonical builtin producer and prefab envelope rather than manufacturing a registry handle. */
        std::vector<std::uint8_t> EncodeNavigationPrefab(const Navigation::NavigationAgentProfileId profile) const {
            const SceneSource::SceneObjectComponentSet components{
                .navigationAgent = Runtime::NavigationAgentComponent{.profile = profile, .filter = pathFilter}};
            auto agent = SceneSource::EncodePrefabNavigationComponent(Runtime::BuiltinSceneComponentKind::NavigationAgent,
                                                                      Prefab::PrefabComponentInstanceId::Create(1).Value(), components);
            REQUIRE(agent.HasValue());
            const auto content = ComputeSha256(std::as_bytes(std::span{"nav prefab", std::size_t{10}}));
            Prefab::CookedPrefabEntity root;
            root.provenance = {project.prefabId, {}, content};
            root.members.emplace_back(std::move(agent).Value());
            root.members.emplace_back(NavigationClient(true));
            const auto prefab = Prefab::CookedPrefab::Create({.assetId = project.prefabId, .entities = {std::move(root)}},
                                                             Prefab::PrefabLimitProfile::Create({}).Value());
            REQUIRE(prefab.HasValue());
            std::vector<std::uint8_t> payload;
            payload.reserve(prefab.Value().Bytes().size());
            for (const auto byte : prefab.Value().Bytes())
                payload.push_back(std::to_integer<std::uint8_t>(byte));
            auto envelope = Assets::EncodeCookedArtifact({.id = project.prefabId,
                                                          .type = Type("core.prefab"),
                                                          .target = project.request.assets.target,
                                                          .sourceDigest = content,
                                                          .payloadDigest = ComputeSha256(prefab.Value().Bytes()),
                                                          .payload = std::move(payload)});
            REQUIRE(envelope.HasValue());
            return std::move(envelope).Value();
        }

        static Assets::AssetRecord AssetRecordFor(const Assets::AssetId id, const std::string_view type, const std::string_view path) {
            return {.id = id,
                    .type = Type(type),
                    .sourcePath = ProjectPath::Parse(path).Value(),
                    .metadataPath = ProjectPath::Parse(std::string(path) + ".horo").Value()};
        }

        void PublishCook(std::vector<std::uint8_t> navigationEnvelope, const Runtime::RuntimeSceneDefinition &scene,
                         std::vector<std::uint8_t> prefab, std::span<const AddedAsset> added) {
            const auto cooked = SceneCook::EncodeCookedSceneDefinition(scene);
            REQUIRE(cooked.HasValue());
            const auto target = project.request.assets.target;
            const auto digest = ComputeSha256(std::as_bytes(std::span{cooked.Value()}));
            auto sceneEnvelope =
                Assets::EncodeCookedArtifact({project.sceneId, Type("core.scene"), target, digest, digest, digest, cooked.Value()});
            REQUIRE(sceneEnvelope.HasValue());
            std::vector envelopes{std::move(sceneEnvelope).Value(), std::move(navigationEnvelope)};
            const auto entry = [](const Assets::AssetId id, const Assets::AssetTypeId &type, const std::vector<std::uint8_t> &bytes) {
                return Assets::AssetCookManifestEntry{id, type, id.ToString() + ".cooked", ComputeSha256(std::as_bytes(std::span{bytes}))};
            };
            std::vector entries{entry(project.sceneId, Type("core.scene"), envelopes[0]),
                                entry(definitionId, Type("core.navmesh"), envelopes[1])};
            if (!prefab.empty()) {
                entries.insert(entries.begin(), entry(project.prefabId, Type("core.prefab"), prefab));
                prefabDigest = entries.front().artifactHash;
                envelopes.insert(envelopes.begin(), std::move(prefab));
            }
            for (const auto &asset : added) {
                auto bytes = asset.cookedEnvelope;
                if (bytes.empty()) {
                    const auto hash = ComputeSha256(std::as_bytes(std::span{asset.payload}));
                    auto encoded =
                        Assets::EncodeCookedArtifact({asset.record.id, asset.record.type, target, hash, hash, hash, asset.payload});
                    REQUIRE(encoded.HasValue());
                    bytes = std::move(encoded).Value();
                }
                const auto at = std::ranges::lower_bound(entries, asset.record.id, {}, &Assets::AssetCookManifestEntry::assetId);
                const auto manifestEntry = entry(asset.record.id, asset.record.type, bytes);
                envelopes.insert(envelopes.begin() + (at - entries.begin()), std::move(bytes));
                entries.insert(at, manifestEntry);
            }
            auto lease = project.files.TryAcquireExclusive(project.cooked.path / ".cook-writer.lock", "Navigation product fixture");
            REQUIRE(lease.HasValue());
            const Assets::AssetCookPublicationPolicy publication{.files = &project.files,
                                                                 .operationId = project.sceneId.ToString(),
                                                                 .writerLease = &lease.Value()};
            const auto generation = Assets::PublishCookGeneration(project.cooked.path, target, entries, envelopes, {}, publication);
            if (generation.HasError())
                UNSCOPED_INFO(generation.ErrorValue().code.Value() + ": " + generation.ErrorValue().message);
            REQUIRE(generation.HasValue());
        }
    };
}  // namespace Horo::Application::CharacterSceneProductTest
