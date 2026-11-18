#include "CharacterSceneProductFixture.h"
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
        fixture.Publish(NavigationProductFixture::Input::Valid, true);
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
        fixture.Publish(NavigationProductFixture::Input::Valid, true);
        const auto gameplay = NavigationGameplay();
        NativeNavigationProductWorld product;
        const Internal::HeadlessProjectSceneOptions options{.projectRoot = fixture.project.project.dir.path,
                                                            .scene = "cooked:" + fixture.project.sceneId.ToString(),
                                                            .cookedRoot = fixture.project.cooked.path,
                                                            .cookTarget = fixture.project.request.assets.target.Value(),
                                                            .navigationMode = Internal::HeadlessNavigationMode::RecastDetour};
        auto resources = Internal::PrepareHeadlessProjectSceneResources(options);
        REQUIRE(resources.HasValue());
        product.resources = std::move(resources).Value();
        auto &project = *product.resources;
        const auto filter = fixture.pathFilter;
        auto composition =
            Internal::ComposeHeadlessSceneNetworkServices(project.definition,
                                                          {.mode = options.physicsMode,
                                                           .settings = *project.settings,
                                                           .registry = project.registry,
                                                           .loads = *project.loads,
                                                           .load = project.load,
                                                           .navigation = project.navigation,
                                                           .navigationPath = Internal::HeadlessNavigationPathPolicy{filter, 512}},
                                                          {}, gameplay);
        REQUIRE(composition.HasValue());
        product.composition.emplace(std::move(composition).Value());
        const auto makeService = [&](const Network::NetworkModeServiceKind kind) {
            auto service = product.composition->factories.services[static_cast<std::size_t>(kind)](
                {kind, Network::NetworkProjectRole::Standalone, {}, Network::NetworkModeWorldKind::Standalone, {31}});
            REQUIRE(service.HasValue());
            return std::move(service).Value();
        };
        product.scene = makeService(Network::NetworkModeServiceKind::Scene);
        REQUIRE(product.scene->Prepare().HasValue());
        product.physics = makeService(Network::NetworkModeServiceKind::Physics);
        REQUIRE(product.physics->Prepare().HasValue());
        REQUIRE(product.scene->Activate().HasValue());
        REQUIRE(product.physics->Activate().HasValue());

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
        std::uint64_t tick{};
        const auto runFrame = [&] {
            const auto completed =
                product.physics->RunFixedTick({++tick, Duration::FromNanoseconds(16'666'667), project.cancellation.Token()});
            if (completed.HasError())
                UNSCOPED_INFO(completed.ErrorValue().message);
            REQUIRE(completed.HasValue());
            const auto committed = product.physics->RunPhase(Runtime::RuntimePhase::CommitDeferredLifecycleChanges);
            if (committed.HasError())
                UNSCOPED_INFO(committed.ErrorValue().message);
            REQUIRE(committed.HasValue());
        };
        for (; tick < 512 && !output;) {
            runFrame();
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
        runFrame();
        product.Stop();
        auto closed = product.composition->pollRetirement(64);
        REQUIRE(closed.HasValue());
        CHECK(closed.Value());
        const auto rejected = pending.Value().Poll();
        REQUIRE(rejected.HasError());
        CHECK(rejected.ErrorValue().code.Value() == Gameplay::NavigationCapabilityErrors::ScopeRevoked.code.Value());
        CHECK(gameplay.moduleRetirements->Close().HasValue());
    }
#endif

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
