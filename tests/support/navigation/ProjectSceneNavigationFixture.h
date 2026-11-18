#pragma once

#include "Horo/Application/NavigationProjectSettings.h"
#include "Horo/Navigation/NavigationDefinitionSerialization.h"
#include "Horo/Scene/CookedSceneDefinition.h"
#include "Horo/Scene/SceneRuntimeConversion.h"
#include "PrefabSceneCookHostFixture.h"
#include "navigation/NavigationContentPolicyFixture.h"

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
        HostFixture project;
        SceneSource::SceneSourceDocument source;
        const Navigation::NavigationFilterId pathFilter{Navigation::NavigationFilterId::Create(13).Value()};
        const Assets::AssetId definitionId{Id("11223344-5566-7788-99aa-bbccddeeff88")};

        void Publish(const Input input, const bool nativeClient = false) {
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
            REQUIRE(project.registry
                        .Publish({AssetRecordFor(project.sceneId, "core.scene", "assets/level.horo"),
                                  AssetRecordFor(definitionId, "core.navmesh", "assets/nav.horoasset")})
                        .status == Assets::AssetRegistryBuildStatus::Complete);

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
            const auto cookedTile =
                NavigationCookedTile::Create(tile, {.state = NavigationTileBuildState::Built,
                                                    .bounds = tile.tile.bounds,
                                                    .vertices = {{0, 0, 0}, {10, 0, 0}, {0, 0, 10}},
                                                    .polygons = {{{0, 3}, {0, 3}, area}},
                                                    .polygonVertexIndices = {0, 1, 2},
                                                    .polygonAdjacencies = {NavMeshBoundaryAdjacency, NavMeshBoundaryAdjacency,
                                                                           NavMeshBoundaryAdjacency},
                                                    .provenance = {{.producer = NavigationSourceProducerId::Create(1).Value(),
                                                                    .contribution = NavigationSourceContributionId::Create(1).Value(),
                                                                    .revision = NavigationSourceRevision::Create(1).Value(),
                                                                    .sourceDigest = digest,
                                                                    .polygons = {0, 1}}}});
            CAPTURE((cookedTile.HasError() ? cookedTile.ErrorValue().message : "encoded"));
            REQUIRE(cookedTile.HasValue());
            if (input == Input::StalePolicy)
                policy.revision = NavigationProjectProfileRevision::Create(1).Value();
            const NavigationCookedTileSet set{digest,
                                              {cookedTile.Value()},
                                              NavigationCookedContentProvenance{{digest, digest, digest},
                                                                                NavigationProjectProfile::Create(policy).Value()}};
            source.objects.push_back({.id = {1}, .name = "Play camera", .components = {.camera = Runtime::CameraComponent{}}});
            if (nativeClient)
                source.objects.front().components.behaviors.push_back(
                    {{1},
                     Gameplay::BehaviorTypeId::Parse("game.tests.navigation_client").Value(),
                     1,
                     true,
                     {{"destinationX", 2.0}, {"destinationZ", 2.0}}});
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
            const SceneSource::SceneSourceView view{source.objects, source.prefabInstances};
            project.WriteText("assets/level.horo", SceneSource::EncodeSceneSource(view));
            const auto scene = SceneSource::ConvertSceneSourceToRuntime(view, SceneCook::SceneDefinitionIdForAsset(project.sceneId), {1});
            REQUIRE(scene.HasValue());
            PublishCook(Navigation::TestSupport::ContentEnvelope(set, definitionId, project.request.assets.target), scene.Value());
        }

    private:
        static Assets::AssetRecord AssetRecordFor(const Assets::AssetId id, const std::string_view type, const std::string_view path) {
            return {.id = id,
                    .type = Type(type),
                    .sourcePath = ProjectPath::Parse(path).Value(),
                    .metadataPath = ProjectPath::Parse(std::string(path) + ".horo").Value()};
        }

        void PublishCook(std::vector<std::uint8_t> navigationEnvelope, const Runtime::RuntimeSceneDefinition &scene) {
            const auto cooked = SceneCook::EncodeCookedSceneDefinition(scene);
            REQUIRE(cooked.HasValue());
            const auto target = project.request.assets.target;
            const auto digest = ComputeSha256(std::as_bytes(std::span{cooked.Value()}));
            auto sceneEnvelope =
                Assets::EncodeCookedArtifact({project.sceneId, Type("core.scene"), target, digest, digest, digest, cooked.Value()});
            REQUIRE(sceneEnvelope.HasValue());
            const std::array envelopes{std::move(sceneEnvelope).Value(), std::move(navigationEnvelope)};
            const auto entry = [](const Assets::AssetId id, const Assets::AssetTypeId &type, const std::vector<std::uint8_t> &bytes) {
                return Assets::AssetCookManifestEntry{id, type, id.ToString() + ".cooked", ComputeSha256(std::as_bytes(std::span{bytes}))};
            };
            const std::array entries{entry(project.sceneId, Type("core.scene"), envelopes[0]),
                                     entry(definitionId, Type("core.navmesh"), envelopes[1])};
            auto lease = project.files.TryAcquireExclusive(project.cooked.path / ".cook-writer.lock", "Navigation product fixture");
            REQUIRE(lease.HasValue());
            const Assets::AssetCookPublicationPolicy publication{.files = &project.files,
                                                                 .operationId = project.sceneId.ToString(),
                                                                 .writerLease = &lease.Value()};
            REQUIRE(Assets::PublishCookGeneration(project.cooked.path, target, entries, envelopes, {}, publication).HasValue());
        }
    };
}  // namespace Horo::Application::CharacterSceneProductTest
