#include "../../helpers/ProjectReplicationTestSupport.h"
#include "CharacterSceneProductFixture.h"
#include "GameplayReplicationAdmissionTestSupport.h"
#include "HeadlessProjectScene.h"
#include "Horo/Gameplay/GameplayNavigationContext.h"
#include "Horo/Physics/PhysicsErrors.h"
#include "Horo/Platform/DynamicLibrary.h"
#include "Horo/Platform/ExternalProcess.h"
#include "navigation/ProjectSceneNavigationFixture.h"

#include <bit>
#include <catch2/generators/catch_generators.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>
#include <utility>
#include <vector>

namespace Horo::Application::CharacterSceneProductTest {
    namespace {
        // The >1 MiB/8192-object fixture also runs under parallel MSVC Debug CI.
        // Keep startup bounded independently of the child process's cleanup budget.
        constexpr auto SceneLoadTimeout = std::chrono::seconds{60};
        constexpr auto CliProcessTimeout = SceneLoadTimeout + std::chrono::seconds{30};

        ExternalProcessResult RunCli(std::vector<std::string> arguments, const std::string_view expectedDiagnostic = {}) {
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
            if (!expectedDiagnostic.empty())
                CHECK(output.find(expectedDiagnostic) != std::string::npos);
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

        std::vector<std::string> NavigationArguments(const NavigationProductFixture &fixture) {
            return {"--run-network-product",
                    "standalone",
                    "2",
                    "--project",
                    Utf8Path(fixture.project.project.dir.path),
                    "--scene",
                    "cooked:" + fixture.project.sceneId.ToString(),
                    "--cooked-root",
                    Utf8Path(fixture.project.cooked.path),
                    "--cook-target",
                    fixture.project.request.assets.target.Value(),
                    "--navigation",
                    "recast-detour"};
        }

        /** @brief Reads the actual generated fixture revision instead of substituting a module identity. */
        Internal::GameplayWorldSelection NavigationGameplay() {
            Internal::GameplayWorldSelection gameplay;
            gameplay.moduleId = "game.tests";
            gameplay.nativeArtifact = HORO_TEST_GAME_MODULE_PATH;
            gameplay.moduleRetirements = std::make_shared<Gameplay::GameModuleRetirementQueue>();
            std::ifstream input{HORO_TEST_GAME_MODULE_REVISION_PATH};
            REQUIRE((input >> gameplay.descriptorRevision));
            return gameplay;
        }

#if HORO_TEST_NAVIGATION_NATIVE
        /** @brief Pairs the real Scene and Physics World facades and preserves production shutdown order on test failure. */
        struct NativeNavigationProductWorld final {
            std::unique_ptr<Internal::HeadlessProjectSceneResources> resources;
            std::optional<Internal::NetworkProductComposition> composition;
            std::unique_ptr<Network::INetworkModeService> scene;
            std::unique_ptr<Network::INetworkModeService> physics;
            bool stopped{};
            std::uint64_t tick{};

            void Start(const NavigationProductFixture &fixture, const Internal::GameplayWorldSelection &gameplay) {
                const Internal::HeadlessProjectSceneOptions options{.projectRoot = fixture.project.project.dir.path,
                                                                    .scene = "cooked:" + fixture.project.sceneId.ToString(),
                                                                    .cookedRoot = fixture.project.cooked.path,
                                                                    .cookTarget = fixture.project.request.assets.target.Value(),
                                                                    .navigationMode = Internal::HeadlessNavigationMode::RecastDetour};
                auto prepared = Internal::PrepareHeadlessProjectSceneResources(options);
                REQUIRE(prepared.HasValue());
                resources = std::move(prepared).Value();
                auto &project = *resources;
                const auto nativePrefabs = gameplay.prefabPermission == Internal::GameplayPrefabPermission::Granted;
                auto configured =
                    Internal::ComposeHeadlessSceneNetworkServices(project.definition,
                                                                  {.mode = options.physicsMode,
                                                                   .settings = *project.settings,
                                                                   .registry = project.registry,
                                                                   .loads = *project.loads,
                                                                   .load = project.load,
                                                                   .prefabLoads = nativePrefabs ? project.prefabLoads.get() : nullptr,
                                                                   .navigation = project.navigation,
                                                                   .navigationPath =
                                                                       Internal::HeadlessNavigationPathPolicy{fixture.pathFilter, 512}},
                                                                  {}, gameplay);
                REQUIRE(configured.HasValue());
                composition.emplace(std::move(configured).Value());
                const auto makeService = [&](const Network::NetworkModeServiceKind kind) {
                    auto service = composition->factories.services[static_cast<std::size_t>(kind)](
                        {kind, Network::NetworkProjectRole::Standalone, {}, Network::NetworkModeWorldKind::Standalone, {31}});
                    REQUIRE(service.HasValue());
                    return std::move(service).Value();
                };
                scene = makeService(Network::NetworkModeServiceKind::Scene);
                REQUIRE(scene->Prepare().HasValue());
                physics = makeService(Network::NetworkModeServiceKind::Physics);
                REQUIRE(physics->Prepare().HasValue());
                REQUIRE(scene->Activate().HasValue());
                REQUIRE(physics->Activate().HasValue());
            }

            void RunFrame() {
                const auto cancellation = resources->cancellation.Token();
                const auto result = physics->RunFixedTick({++tick, Duration::FromNanoseconds(16'666'667), cancellation});
                if (result.HasError())
                    UNSCOPED_INFO(result.ErrorValue().message);
                REQUIRE(result.HasValue());
                const auto commit = physics->RunPhase(Runtime::RuntimePhase::CommitDeferredLifecycleChanges);
                if (commit.HasError())
                    UNSCOPED_INFO(commit.ErrorValue().message);
                REQUIRE(commit.HasValue());
            }

            void Await(const Prefab::PrefabOperation &operation, const Prefab::PrefabSpawnState expected) {
                const auto deadline = tick + 64;
                while (operation.State() == Prefab::PrefabSpawnState::Pending && tick < deadline) {
                    RunFrame();
                    if (operation.State() == Prefab::PrefabSpawnState::Pending)
                        std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
                if (const auto failure = operation.Failure())
                    UNSCOPED_INFO(failure->message);
                REQUIRE(operation.State() == expected);
            }

            ~NativeNavigationProductWorld() {
                Stop();
            }

            void Stop() {
                if (std::exchange(stopped, true))
                    return;
                if (physics)
                    physics->Shutdown();
                if (scene)
                    scene->Shutdown();
                if (resources) {
                    resources->cancellation.RequestCancellation();
                    if (resources->prefabLoads)
                        resources->prefabLoads->Shutdown();
                    resources->loads->Shutdown();
                    resources->jobs.Shutdown(ShutdownPolicy::Cancel);
                }
            }
        };
#endif

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
            CHECK(RunCli(NavigationArguments(fixture)).exitCode == (HORO_TEST_NAVIGATION_NATIVE ? 0 : 3));
        }
        SECTION("selected policy requires an actual world") {
            fixture.Publish(Input::MissingSurface);
            CHECK(RunCli(NavigationArguments(fixture)).exitCode == 3);
        }
        SECTION("foreign Scene scope cannot activate the artifact") {
            fixture.Publish(Input::ForeignScope);
            CHECK(RunCli(NavigationArguments(fixture)).exitCode == 3);
        }
        SECTION("stale cooked authority cannot publish") {
            fixture.Publish(Input::StalePolicy);
            CHECK(RunCli(NavigationArguments(fixture)).exitCode == 3);
        }
        SECTION("unsupported authored links cannot disappear from activation") {
            fixture.Publish(Input::UnsupportedLink);
            CHECK(RunCli(NavigationArguments(fixture)).exitCode == 3);
        }
    }

    TEST_CASE("Terminal native Navigation path requires its explicit filter and deadline", "[navigation][scene][cli][product]") {
        NavigationProductFixture fixture;
        fixture.Publish(NavigationProductFixture::Input::Valid, NavigationProductFixture::Client::Scene);
        const auto gameplay = NavigationGameplay();
        auto arguments = NavigationArguments(fixture);
        arguments.insert(arguments.end(), {"--game-module", Utf8Path(gameplay.nativeArtifact), gameplay.moduleId,
                                           std::to_string(gameplay.descriptorRevision), "deny", "--game-navigation-path"});
        auto missing = arguments;
        CHECK(RunCli(std::move(missing)).exitCode == 2);
        const auto filter = std::to_string(fixture.pathFilter.Value());
        auto unlimited = arguments;
        unlimited.insert(unlimited.end(), {filter, std::to_string(Internal::HeadlessNavigationPathPolicy::MaximumDeadlineTicks + 1)});
        CHECK(RunCli(std::move(unlimited)).exitCode == 2);
        auto unknown = arguments;
        unknown.insert(unknown.end(), {std::to_string(fixture.pathFilter.Value() + 1), "32"});
        const auto reason = HORO_TEST_NAVIGATION_NATIVE ? Navigation::NavigationErrors::FilterUnknown.summary : std::string_view{};
        CHECK(RunCli(std::move(unknown), reason).exitCode == 3);
        arguments.insert(arguments.end(), {filter, "32"});
        CHECK(RunCli(std::move(arguments)).exitCode == (HORO_TEST_NAVIGATION_NATIVE ? 0 : 3));
    }

#if HORO_TEST_NAVIGATION_NATIVE
    TEST_CASE("Native gameplay executes a cooked standalone path and rejects retained requests after world retirement",
              "[navigation][scene][gameplay][product]") {
        NavigationProductFixture fixture;
        fixture.Publish(NavigationProductFixture::Input::Valid, NavigationProductFixture::Client::Scene);
        const auto gameplay = NavigationGameplay();
        NativeNavigationProductWorld product;
        product.Start(fixture, gameplay);

        const auto scope = Internal::InspectHeadlessNavigationContext(*product.physics);
        REQUIRE(scope);
        CHECK(scope->Binding().queryGranted);
        CHECK_FALSE(scope->Binding().movementGranted);
        CHECK(scope->Binding().scene.scene.Value() == 31);
        const auto movement = scope->Submit({.operation = Gameplay::GameplayNavigationOperation::MoveTo});
        REQUIRE(movement.HasError());
        CHECK(movement.ErrorValue().code.Value() == Gameplay::NavigationCapabilityErrors::PermissionDenied.code.Value());

        // Read results emitted by the real native behavior activated on the cooked Scene, rather than a fake backend.
        auto image = Platform::LoadDynamicLibraryAtPath(gameplay.nativeArtifact);
        REQUIRE(image.HasValue());
        auto *symbol = image.Value()->GetSymbol("HoroTestTakeNavigationResult");
        REQUIRE(symbol);
        const auto take = std::bit_cast<void (*)(void *)>(symbol);
        std::shared_ptr<const Gameplay::GameplayNavigationResult> output;
        while (product.tick < 512 && !output) {
            product.RunFrame();
            take(&output);
            if (!output)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        REQUIRE(output);
        REQUIRE(output->path);
        CHECK(output->path->status == Navigation::NavigationPathStatus::Reachable);
        REQUIRE_FALSE(output->path->corridor.empty());
        CHECK(output->path->corridor.front().provenance.world == scope->Binding().scene.world);

        auto pending = scope->Submit({.start = {1, 0, 1}, .destination = {2, 0, 2}});
        REQUIRE(pending.HasValue());
        product.RunFrame();
        product.Stop();
        auto closed = product.composition->pollRetirement(64);
        REQUIRE(closed.HasValue());
        CHECK(closed.Value());
        const auto rejected = pending.Value().Poll();
        REQUIRE(rejected.HasError());
        CHECK(rejected.ErrorValue().code.Value() == Gameplay::NavigationCapabilityErrors::ScopeRevoked.code.Value());
        CHECK(gameplay.moduleRetirements->Close().HasValue());
    }

    TEST_CASE("Cooked prefab native path handles retire with the exact behavior agent while the module stays active",
              "[navigation][scene][gameplay][prefab][product]") {
        NavigationProductFixture fixture;
        fixture.Publish(NavigationProductFixture::Input::Valid, NavigationProductFixture::Client::Prefab);
        auto gameplay = NavigationGameplay();
        gameplay.prefabPermission = Internal::GameplayPrefabPermission::Granted;
        NativeNavigationProductWorld product;
        product.Start(fixture, gameplay);
        const auto moduleScope = Internal::InspectHeadlessNavigationContext(*product.physics);
        REQUIRE(moduleScope);

        auto image = Platform::LoadDynamicLibraryAtPath(gameplay.nativeArtifact);
        REQUIRE(image.HasValue());
        const auto prefabSymbol = image.Value()->GetSymbol("HoroTestCopyPrefabCapability");
        const auto pathSymbol = image.Value()->GetSymbol("HoroTestTakeNavigationSubmission");
        const auto resultSymbol = image.Value()->GetSymbol("HoroTestTakeNavigationResult");
        REQUIRE(prefabSymbol);
        REQUIRE(pathSymbol);
        REQUIRE(resultSymbol);
        using PrefabProbe = void (*)(std::shared_ptr<const Gameplay::GameplayPrefabContext> *);
        using PathProbe =
            void (*)(std::shared_ptr<const Gameplay::GameplayNavigationContext> *, std::optional<Gameplay::GameplayNavigationHandle> *);
        const auto copyPrefab = std::bit_cast<PrefabProbe>(prefabSymbol);
        const auto takePath = std::bit_cast<PathProbe>(pathSymbol);
        const auto takeResult = std::bit_cast<void (*)(void *)>(resultSymbol);
        std::shared_ptr<const Gameplay::GameplayPrefabContext> prefabs;
        product.RunFrame();
        copyPrefab(&prefabs);
        REQUIRE(prefabs);
        const auto spawned =
            prefabs->Spawn({.source = {fixture.project.prefabId, fixture.prefabDigest, fixture.project.request.assets.target},
                            .notBeforeTick = product.tick + 1});
        REQUIRE(spawned.HasValue());
        product.Await(spawned.Value(), Prefab::PrefabSpawnState::Committed);
        const auto instance = spawned.Value().Spawned();
        REQUIRE(instance.HasValue());
        const auto agent = instance.Value().Root();
        REQUIRE(agent.IsValid());
        product.RunFrame();

        std::shared_ptr<const Gameplay::GameplayNavigationContext> agentScope;
        std::optional<Gameplay::GameplayNavigationHandle> completed;
        takePath(&agentScope, &completed);
        REQUIRE(agentScope);
        REQUIRE(completed);
        CHECK(agentScope != moduleScope);
        const Gameplay::GameplayNavigationRequest path{.start = {1, 0, 1}, .destination = {2, 0, 2}};
        const auto quota = moduleScope->Submit(path);
        REQUIRE(quota.HasError());
        CHECK(quota.ErrorValue().code.Value() == Navigation::NavigationErrors::AdmissionRejected.code.Value());

        std::shared_ptr<const Gameplay::GameplayNavigationResult> result;
        while (product.tick < 512 && !result) {
            product.RunFrame();
            takeResult(&result);
            if (!result)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        REQUIRE(result);
        REQUIRE(result->path);
        REQUIRE(result->path->status == Navigation::NavigationPathStatus::Reachable);

        CancellationSource cancelled;
        const auto preserved = prefabs->Despawn(instance.Value(), product.tick + 1, cancelled.Token());
        REQUIRE(preserved.HasValue());
        cancelled.RequestCancellation();
        product.Await(preserved.Value(), Prefab::PrefabSpawnState::Cancelled);
        CHECK(agentScope->IsUsable());
        REQUIRE(completed->Poll().HasValue());

        const auto pending = agentScope->Submit(path);
        REQUIRE(pending.HasValue());
        const auto retired = prefabs->Despawn(instance.Value(), product.tick + 1);
        REQUIRE(retired.HasValue());
        product.Await(retired.Value(), Prefab::PrefabSpawnState::Committed);
        const auto current = Internal::InspectHeadlessScene(*product.physics);
        REQUIRE(current);
        CHECK(current->Get(agent).HasError());
        const auto &completedRequest = *completed;
        for (const auto *request : {&completedRequest, &pending.Value()}) {
            const auto response = request->Poll();
            REQUIRE(response.HasError());
            CHECK(response.ErrorValue().code.Value() == Gameplay::NavigationCapabilityErrors::ScopeRevoked.code.Value());
        }
        CHECK_FALSE(agentScope->IsUsable());
        CHECK(agentScope->Submit(path).HasError());
        REQUIRE(moduleScope->IsUsable());

        // The canonical request keeps its slot charged until accepted worker work drains. The module resumes when that fence exits.
        bool moduleAvailable = false;
        const auto deadline = product.tick + 64;
        while (product.tick < deadline && !moduleAvailable) {
            product.RunFrame();
            const auto unrelated = moduleScope->Submit(path);
            moduleAvailable = unrelated.HasValue();
            if (!moduleAvailable)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        CHECK(moduleAvailable);
        product.Stop();
        const auto drained = product.composition->pollRetirement(64);
        REQUIRE(drained.HasValue());
        CHECK(drained.Value());
        CHECK(gameplay.moduleRetirements->Close().HasValue());
    }
#endif

    TEST_CASE("Cooked project Scene selection captures the root's explicit network authority", "[scene][network][product]") {
        Fixture fixture;
        Internal::TestSupport::ReplicatedScriptFixture script;
        const auto &inventory = script.selection.replicationAdmission->inventory;
        Horo::Tests::WriteReplicationProject(fixture.project.project.dir.path, inventory);
        const Internal::HeadlessProjectSceneOptions options{.projectRoot = fixture.project.project.dir.path,
                                                            .scene = "cooked:" + fixture.project.sceneId.ToString(),
                                                            .cookedRoot = fixture.project.cooked.path,
                                                            .cookTarget = fixture.project.request.assets.target.Value(),
                                                            .captureNetworkSettings = true};
        auto prepared = Internal::PrepareHeadlessProjectSceneResources(options);
        if (prepared.HasError())
            UNSCOPED_INFO(prepared.ErrorValue().message);
        REQUIRE(prepared.HasValue());
        REQUIRE(prepared.Value()->networkSettings);
        const auto &captured = prepared.Value()->networkSettings->ReplicationInventory();
        CHECK(captured.completeness == inventory.completeness);
        REQUIRE(captured.declarations.size() == 1);
        CHECK(captured.declarations.front().schema == inventory.declarations.front().schema);
        CHECK(prepared.Value()->sceneAsset == fixture.project.sceneId);
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
