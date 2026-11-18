#include "CharacterSceneProductFixture.h"
#include "Horo/Application/NavigationProjectSettings.h"
#include "Horo/Navigation/NavigationDefinitionSerialization.h"
#include "Horo/Physics/PhysicsErrors.h"
#include "Horo/Platform/ExternalProcess.h"
#include "Horo/Scene/CookedSceneDefinition.h"
#include "navigation/NavigationContentPolicyFixture.h"

#include <catch2/generators/catch_generators.hpp>
#include <chrono>
#include <filesystem>
#include <vector>

namespace Horo::Application::CharacterSceneProductTest {
    namespace {
        // The >1 MiB/8192-object fixture also runs under parallel MSVC Debug CI.
        // Keep startup bounded independently of the child process's cleanup budget.
        constexpr auto SceneLoadTimeout = std::chrono::seconds{60};
        constexpr auto CliProcessTimeout = SceneLoadTimeout + std::chrono::seconds{30};

        ExternalProcessResult RunCli(std::vector<std::string> arguments) {
            NativeExternalProcessRunner runner;
            std::string output;
            ExternalProcessRequest request{.executable = HORO_TEST_CHARACTER_CLI_PATH,
                                           .arguments = std::move(arguments),
                                           .timeout = CliProcessTimeout,
                                           .onOutput = [&output](ProcessOutputLine line) {
                output += line.text + '\n';
            }};
            const auto result = runner.Run(request, {});
            UNSCOPED_INFO(output);
            if (result.HasError())
                UNSCOPED_INFO(result.ErrorValue().message);
            REQUIRE(result.HasValue());
            REQUIRE(result.Value().reason == ProcessTerminationReason::Exited);
            UNSCOPED_INFO(output);
            return result.Value();
        }

        std::string Utf8Path(const std::filesystem::path &path) {
            const auto value = path.generic_u8string();
            return {value.begin(), value.end()};
        }

        std::vector<std::string> SceneArguments(const Fixture &fixture, const std::string &scene, const std::string &mode) {
            return {"--run-network-product",
                    "standalone",
                    "2",
                    "--project",
                    Utf8Path(fixture.project.project.dir.path),
                    "--scene",
                    scene,
                    "--physics",
                    mode,
                    "--cooked-root",
                    Utf8Path(fixture.project.cooked.path),
                    "--cook-target",
                    fixture.project.request.assets.target.Value(),
                    "--load-timeout-ms",
                    std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(SceneLoadTimeout).count())};
        }

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
            const Assets::AssetId definitionId{Id("11223344-5566-7788-99aa-bbccddeeff88")};

            void Publish(const Input input) {
                using namespace Navigation;
                const auto profile = NavigationAgentProfileId::Create(7).Value();
                const auto surface = SurfaceId::Create(53).Value();
                const auto area = NavigationAreaId::Create(11).Value();
                const auto filter = NavigationFilterId::Create(13).Value();
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
                const auto source = SerializeNavigationSourceRecords(records.Value());
                REQUIRE(source.HasValue());
                project.WriteText("assets/nav.horoasset", {reinterpret_cast<const char *>(source.Value().data()), source.Value().size()});
                project.WriteText("assets/nav.horoasset.horo", SidecarJson(definitionId.ToString(), "core.navmesh"));
                std::filesystem::rename(project.project.dir.path / "assets/level.scene", project.project.dir.path / "assets/level.horo");
                std::filesystem::rename(project.project.dir.path / "assets/level.scene.horo",
                                        project.project.dir.path / "assets/level.horo.horo");
                REQUIRE(project.registry
                            .Publish({Fixture::Record(project.sceneId, "core.scene", "assets/level.horo"),
                                      Fixture::Record(definitionId, "core.navmesh", "assets/nav.horoasset")})
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
                Runtime::SceneDefinitionBuilder builder{SceneCook::SceneDefinitionIdForAsset(project.sceneId), {1}};
                if (input != Input::MissingSurface) {
                    builder.Add({.object = {1},
                                 .components = {.navigationSurface = Runtime::NavigationSurfaceComponent{.id = surface,
                                                                                                         .definition = definitionId,
                                                                                                         .generation = 1,
                                                                                                         .profiles = {profile}}}});
                    builder.Add(
                        {.object = {2},
                         .components = {.navigationAgent = Runtime::NavigationAgentComponent{.profile = profile, .filter = filter}}});
                }
                if (input == Input::UnsupportedLink)
                    builder.Add(
                        {.object = {3},
                         .components = {.navigationLink =
                                            Runtime::NavigationLinkComponent{.id = NavigationLinkId::Create(5).Value(),
                                                                             .start = {.surface = surface},
                                                                             .end = {.surface = surface, .localPosition = {2, 0, 2}},
                                                                             .profiles = {profile}}}});
                const auto scene = std::move(builder).Build();
                REQUIRE(scene.HasValue());
                PublishCook(Navigation::TestSupport::ContentEnvelope(set, definitionId, project.request.assets.target), scene.Value());
            }

            std::vector<std::string> Arguments() const {
                return {"--run-network-product",
                        "standalone",
                        "2",
                        "--project",
                        Utf8Path(project.project.dir.path),
                        "--scene",
                        "cooked:" + project.sceneId.ToString(),
                        "--cooked-root",
                        Utf8Path(project.cooked.path),
                        "--cook-target",
                        project.request.assets.target.Value(),
                        "--navigation",
                        "recast-detour"};
            }

        private:
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
                    return Assets::AssetCookManifestEntry{id, type, id.ToString() + ".cooked",
                                                          ComputeSha256(std::as_bytes(std::span{bytes}))};
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
    }  // namespace

#if HORO_TEST_PHYSICS_NATIVE
    TEST_CASE("Actual terminal CLI activates authored and committed cooked Character Scenes",
              "[character][scene][cli][product][canonical]") {
        const auto extraObjects = GENERATE(std::size_t{0}, std::size_t{8192});
        CAPTURE(extraObjects);
        Fixture fixture{extraObjects};
        if (extraObjects != 0)
            REQUIRE(std::filesystem::file_size(fixture.project.project.dir.path / "assets/level.horo") > (1U << 20U));
        CHECK(RunCli(SceneArguments(fixture, "source:assets/level.horo", "canonical")).exitCode == 0);
        CHECK(RunCli(SceneArguments(fixture, "cooked:" + fixture.project.sceneId.ToString(), "canonical")).exitCode == 0);
    }
#endif

    TEST_CASE("Explicit Null terminal Scene host refuses enabled Character without fallback", "[character][scene][cli][product][null]") {
        Fixture fixture;
        CHECK(RunCli(SceneArguments(fixture, "source:assets/level.horo", "null")).exitCode == 3);
    }

    TEST_CASE("Terminal Navigation startup uses the exact selected cooked scope and project policy", "[navigation][scene][cli][product]") {
        NavigationProductFixture fixture;
        using Input = NavigationProductFixture::Input;
        SECTION("valid world publishes and shuts down") {
            fixture.Publish(Input::Valid);
            CHECK(RunCli(fixture.Arguments()).exitCode == (HORO_TEST_NAVIGATION_NATIVE ? 0 : 3));
        }
        SECTION("selected policy requires an actual world") {
            fixture.Publish(Input::MissingSurface);
            CHECK(RunCli(fixture.Arguments()).exitCode == 3);
        }
        SECTION("foreign Scene scope cannot activate the artifact") {
            fixture.Publish(Input::ForeignScope);
            CHECK(RunCli(fixture.Arguments()).exitCode == 3);
        }
        SECTION("stale cooked authority cannot publish") {
            fixture.Publish(Input::StalePolicy);
            CHECK(RunCli(fixture.Arguments()).exitCode == 3);
        }
        SECTION("unsupported authored links cannot disappear from activation") {
            fixture.Publish(Input::UnsupportedLink);
            CHECK(RunCli(fixture.Arguments()).exitCode == 3);
        }
    }

    TEST_CASE("Terminal Scene options reject malformed incomplete and escaping input", "[character][scene][cli][product]") {
        CHECK(RunCli({"--run-network-product", "standalone", "1", "--project", "relative", "--scene", "source:level.scene"}).exitCode == 2);
        CHECK(RunCli({"--run-network-product", "standalone", "1", "--physics", "canonical"}).exitCode == 2);
        CHECK(RunCli({"--run-network-product", "standalone", "1", "--navigation", "recast-detour"}).exitCode == 2);
        CHECK(RunCli({"--run-network-product", "standalone", "1", "--project", "/", "--scene", "untyped.scene"}).exitCode == 2);
        Fixture fixture;
        CHECK(RunCli(SceneArguments(fixture, "source:../escape.scene", "null")).exitCode == 3);
        CHECK(RunCli(SceneArguments(fixture, "cooked:not-an-asset-id", "null")).exitCode == 3);
    }

    TEST_CASE("Terminal Scene options reject duplicate and unbounded requests before project startup", "[character][scene][cli][product]") {
        const std::string root = Utf8Path(std::filesystem::absolute(std::filesystem::temp_directory_path()));
        const std::vector<std::vector<std::string>> invalidSuffixes{{"--scene"},
                                                                    {"--scene", ""},
                                                                    {"--unknown", "value"},
                                                                    {"--project", root},
                                                                    {"--scene", "source:unused.horo"},
                                                                    {"--cooked-root", root},
                                                                    {"--cook-target", "native"},
                                                                    {"--project", root, "--scene", "source:unused.horo", "--physics",
                                                                     "automatic"},
                                                                    {"--project", root, "--scene", "source:unused.horo", "--physics",
                                                                     "null", "--physics", "canonical"},
                                                                    {"--project", root, "--scene", "source:unused.horo", "--project", root},
                                                                    {"--project", root, "--scene", "source:unused.horo", "--cooked-root",
                                                                     "relative", "--cook-target", "native"}};
        for (const auto &suffix : invalidSuffixes) {
            CAPTURE(suffix);
            std::vector<std::string> arguments{"--run-network-product", "standalone", "1"};
            arguments.insert(arguments.end(), suffix.begin(), suffix.end());
            CHECK(RunCli(std::move(arguments)).exitCode == 2);
        }
        for (const std::string_view timeout : {"0", "-1", "60001", "1ms", "18446744073709551616"}) {
            CAPTURE(timeout);
            CHECK(RunCli({"--run-network-product", "standalone", "1", "--project", root, "--scene", "source:unused.horo",
                          "--load-timeout-ms", std::string{timeout}})
                      .exitCode == 2);
        }
    }
}  // namespace Horo::Application::CharacterSceneProductTest
