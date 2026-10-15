#include "GameplayModuleTestSupport.h"
#include "Horo/Gameplay/ComponentRegistry.h"
#include "Horo/Gameplay/GameEventRegistry.h"
#include "Horo/Packages/GameplayPackageModuleHost.h"
#include "PackageLifecycleTestSupport.h"

#include <future>
using namespace Horo::Packages::Tests::Support;

namespace {
    std::shared_ptr<PackageRestoreGraph> GameplayGraph(const bool withBackend = false) {
        const auto entry = "module" + std::filesystem::path{HORO_TEST_GAME_MODULE_PATH}.extension().string();
        nlohmann::json descriptor{{"id", "fixture.gameplay"},
                                  {"version", "1.0.0"},
                                  {"modules",
                                   {{{"id", "game.tests"},
                                     {"version", "1.0.0"},
                                     {"kind", "native"},
                                     {"roles", {"runtime-participant"}},
                                     {"entry", entry},
                                     {"requiredCapabilities", {"runtime.participate"}}}}}};
        std::vector<std::pair<std::string, std::string>> files;
        if (withBackend) {
            const auto backend = "backend" + std::filesystem::path{HORO_PACKAGE_NATIVE_FIXTURE}.extension().string();
            descriptor["modules"].push_back({{"id", "fixture.backend"},
                                             {"version", "1.0.0"},
                                             {"kind", "native"},
                                             {"roles", {"backend-capability"}},
                                             {"entry", backend}});
            descriptor["contributions"] = {{{"type", "asset.importer"}, {"id", "fixture.importer"}, {"module", "fixture.backend"}}};
            files.emplace_back("extensions/native/" + backend, FileBytes(HORO_PACKAGE_NATIVE_FIXTURE));
        }
        return Graph(HORO_TEST_GAME_MODULE_PATH, true, true, "fixture.gameplay", std::move(descriptor), std::move(files));
    }

    struct GameplayFixture final {
        Fixture base;
        std::shared_ptr<PackageModuleRetirementQueue> retirement{std::make_shared<PackageModuleRetirementQueue>(4)};
        std::shared_ptr<GameplayPackageModuleHost> gameplay;
        std::shared_ptr<NativePackageModuleHost> backend;
        bool withBackend{};
        EnabledPackageModule enabled{Parse<HoroPackageId>("fixture.gameplay"), Parse<PackagePath>("extensions/native/extension.json"),
                                     "game.tests", Extensions::ExtensionModuleRole::RuntimeParticipant};
        unsigned worldFactories{};

        explicit GameplayFixture(const bool approved = true, const bool correctFingerprint = true, const bool composeHost = true,
                                 const bool mixed = false)
            : withBackend(mixed) {
            base.service.reset();
            base.trust.capabilities = {{"runtime.participate"}};
            base.trust.permissions = {{"runtime.participate"}};
            if (approved)
                base.trust.approvals = base.trust.permissions;
            GameplayPackageModuleBinding binding{enabled.package,
                                                 enabled.module,
                                                 correctFingerprint ? std::string{Gameplay::CurrentGameplayBuildFingerprint()}
                                                                    : "wrong-sdk",
                                                 Horo::Tests::ReadDescriptorRevision(HORO_TEST_GAME_MODULE_REVISION_PATH),
                                                 {},
                                                 [this](const VerifiedPackageModule &) {
                ++worldFactories;
                return Result<std::shared_ptr<Gameplay::GameplayPhysicsContext>>::Success({});
            }};
            gameplay =
                std::make_shared<GameplayPackageModuleHost>(GameplayPackageHostConfiguration{std::filesystem::canonical(
                                                                                                 base.temporary.Path()),
                                                                                             ::Horo::Tests::CreateAcceptingArtifactGate()},
                                                            std::vector<GameplayPackageModuleBinding>{std::move(binding)});
            PackageLifecycleConfiguration config{.temporaryRoot = std::filesystem::canonical(base.temporary.Path()),
                                                 .profile = Extensions::ExtensionHostProfile::Headless,
                                                 .capabilities = {"runtime.participate"},
                                                 .artifactGate = ::Horo::Tests::CreateAcceptingArtifactGate()};
            if (composeHost)
                config.moduleHosts = {gameplay};
            if (mixed) {
                backend = std::make_shared<NativePackageModuleHost>(config);
                config.moduleHosts.push_back(backend);
            }
            config.moduleRetirements = retirement;
            auto created = PackageLifecycleService::Create(base.install, base.trust, std::move(config));
            REQUIRE(created.HasValue());
            base.service = std::move(created).Value();
            base.Install(GameplayGraph(mixed));
        }

        ~GameplayFixture() {
            base.service.reset();
            CHECK(retirement->Close().HasValue());
        }

        Result<void> Activate() {
            std::vector<EnabledPackageModule> selected;
            if (withBackend)
                selected.push_back(
                    {enabled.package, enabled.descriptor, "fixture.backend", Extensions::ExtensionModuleRole::BackendCapability});
            selected.push_back(enabled);
            return base.service->ActivateModules(selected, PackageActivationBoundary::Quiescent, base.cancellation.Token());
        }
    };
}  // namespace

TEST_CASE("Package runtime role invokes actual SDK owner and retains native callbacks until drain", "[packages][gameplay][hybrid]") {
    GameplayFixture fixture;
    REQUIRE(fixture.Activate().HasValue());
    const auto generation = fixture.base.service->Active()->generation;
    auto module = fixture.gameplay->Acquire(fixture.enabled.package, "game.tests", generation);
    REQUIRE(module);
    CHECK(module->Components().IsFrozen());
    CHECK(module->ActiveServices().size() == 1);
    auto acquired = module->Events().Acquire(90, 1, 91, 1, module->Cancellation());
    REQUIRE(acquired.HasValue());
    auto callback = std::move(acquired).Value();
    const auto artifact = module->LoadedArtifactPath();
    fixture.base.service->Shutdown();
    CHECK_FALSE(fixture.gameplay->Acquire(fixture.enabled.package, "game.tests", generation));
    CHECK(fixture.retirement->Drain() == 1);
    CHECK(std::filesystem::exists(artifact));
    module.reset();
    CHECK(fixture.retirement->Close().HasError());
    std::async(std::launch::async, [owned = std::move(callback)]() mutable {
        owned.reset();
    }).get();
    CHECK(fixture.retirement->Close().HasValue());
    CHECK_FALSE(std::filesystem::exists(artifact));
}

TEST_CASE("Package runtime permission is denied before explicit world and SDK factories", "[packages][gameplay][hybrid]") {
    GameplayFixture fixture(false);
    CHECK(fixture.Activate().HasError());
    CHECK(fixture.worldFactories == 0);
    CHECK_FALSE(fixture.base.service->Active());
}

TEST_CASE("Package runtime fingerprint mismatch is a failed required role rather than activation", "[packages][gameplay][hybrid]") {
    GameplayFixture fixture(true, false);
    CHECK(fixture.Activate().HasError());
    CHECK(fixture.worldFactories == 0);
    CHECK_FALSE(fixture.base.service->Active());
    CHECK(fixture.retirement->Close().HasValue());
}

TEST_CASE("Required gameplay role cannot be satisfied by native extension metadata", "[packages][gameplay][hybrid]") {
    GameplayFixture fixture(true, true, false);
    CHECK(fixture.Activate().HasError());
    CHECK(fixture.worldFactories == 0);
}

TEST_CASE("Package gameplay replacement requires exact previous generation retirement", "[packages][gameplay][hybrid]") {
    GameplayFixture fixture;
    REQUIRE(fixture.Activate().HasValue());
    const auto first = fixture.base.service->Active()->generation;
    auto previous = fixture.gameplay->Acquire(fixture.enabled.package, "game.tests", first);
    REQUIRE(previous);
    CHECK(fixture.Activate().HasError());
    CHECK(fixture.base.service->Active()->generation == first);
    fixture.base.service->Shutdown();
    previous.reset();
    REQUIRE(fixture.retirement->Close().HasValue());
}

TEST_CASE("Actual gameplay generations drain before repeated update and uninstall", "[packages][gameplay][hybrid]") {
    GameplayFixture fixture;
    std::uint64_t previousGeneration{};
    for (unsigned cycle = 0; cycle < 3; ++cycle) {
        fixture.base.Install(GameplayGraph());
        REQUIRE(fixture.Activate().HasValue());
        const auto generation = fixture.base.service->Active()->generation;
        CHECK(generation > previousGeneration);
        previousGeneration = generation;
        auto module = fixture.gameplay->Acquire(fixture.enabled.package, "game.tests", generation);
        REQUIRE(module);
        REQUIRE(
            fixture.base.service->ActivateModules({}, PackageActivationBoundary::Quiescent, fixture.base.cancellation.Token()).HasValue());
        CHECK_FALSE(fixture.gameplay->Acquire(fixture.enabled.package, "game.tests", generation));
        CHECK(fixture.retirement->Close().HasError());
        module.reset();
        REQUIRE(fixture.retirement->Close().HasValue());
    }
    auto removed = std::make_shared<PackageRestoreGraph>(*fixture.base.install.ActiveGraph());
    removed->packages.clear();
    REQUIRE(fixture.base.install.Install(removed, fixture.base.cancellation.Token()).HasValue());
    REQUIRE(fixture.base.service->ActivateModules({}, PackageActivationBoundary::Quiescent, fixture.base.cancellation.Token()).HasValue());
    CHECK(fixture.base.service->Active()->install->Graph()->packages.empty());
    REQUIRE(fixture.retirement->Close().HasValue());
}

TEST_CASE("Optional SDK failure preserves an actual native backend in the same verified archive", "[packages][gameplay][hybrid]") {
    GameplayFixture fixture(true, false, true, true);
    fixture.enabled.requirement = PackageModuleRequirement::Optional;
    REQUIRE(fixture.Activate().HasValue());
    auto active = fixture.base.service->Active();
    REQUIRE(active->modules.size() == 2);
    CHECK(fixture.backend->Importers(fixture.enabled.package, "fixture.backend", active->generation));
    CHECK_FALSE(fixture.gameplay->Acquire(fixture.enabled.package, "game.tests", active->generation));
    const auto runtime = std::ranges::find_if(active->modules, [](const auto &report) {
        return report.module == "game.tests";
    });
    REQUIRE(runtime != active->modules.end());
    CHECK(runtime->diagnostic.has_value());
}
