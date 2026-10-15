#include "Horo/Packages/PackageRequest.h"
#include "PackageApplicationServiceTestSupport.h"
#include "PackageProjectHost.h"

#include <fstream>
#include <type_traits>

namespace {
    using namespace Horo;
    using namespace Horo::Application;
    using namespace Horo::Application::Internal;
    using namespace Horo::Tests::PackageApplicationService;

    static_assert(!std::is_default_constructible_v<PackageApplication::ConstructionKey>);
    static_assert(!std::is_aggregate_v<PackageApplication::ConstructionKey>);
    static_assert(!std::is_default_constructible_v<PackageProjectHost::ConstructionKey>);
    static_assert(!std::is_aggregate_v<PackageProjectHost::ConstructionKey>);
    static_assert(!std::is_default_constructible_v<PackageProjectSession::ConstructionKey>);
    static_assert(!std::is_aggregate_v<PackageProjectSession::ConstructionKey>);
    static_assert(!std::is_constructible_v<PackageApplication, std::nullptr_t>);
    static_assert(!std::is_constructible_v<PackageProjectHost, std::nullptr_t>);
    static_assert(!std::is_constructible_v<PackageProjectSession, std::nullptr_t>);

    class ManualClock final : public Clock {
    public:
        Duration now;

        Duration MonotonicNow() const override {
            return now;
        }
    };

    struct HostFixture final {
        Horo::Tests::Packages::TemporaryDirectory project{"package host ü"}, content{"package host content"};
        NativeDurableFileSystem files;
        ManualClock clock;
        SystemWallClock wall;
        NoExecutionTrust trust;
        AssetGraph graph;
        JobSystem jobs{{.workerCount = 1, .maxQueuedJobs = 8, .ioWorkerCount = 1}};
        ControlledSource source{graph.bytes};
        std::unique_ptr<PackageProjectHost> host;

        HostFixture() {
            const auto root = std::filesystem::canonical(project.Path());
            std::filesystem::create_directories(root / ".horo");
            const nlohmann::json intent{{"sources", {{"fixture.source", {{"kind", "public-registry"}, {"registry", "official"}}}}},
                                        {"dependencies",
                                         {{"com.horo.application-fixture", {{"source", "fixture.source"}, {"version", "1.0.0"}}}}}};
            auto request = Packages::ValidatedPackageRequest::Parse(intent.dump());
            REQUIRE(request.HasValue());
            Packages::PackageResolutionPlan plan{.packages = {{graph.package, graph.version, graph.source, graph.archive.Digest(), {}}}};
            Packages::PackageLockArtifact artifact{.package = graph.package,
                                                   .version = graph.version,
                                                   .source = graph.source,
                                                   .artifactDigest = graph.archive.Digest(),
                                                   .manifestDigest = graph.archive.PackageManifestDigest(),
                                                   .fileManifestDigest = graph.archive.Manifest().Digest(),
                                                   .packageFormatVersion = 1,
                                                   .platforms = {},
                                                   .contributions = {"assets"}};
            auto lock = Packages::ValidatedPackageLockfileV1::Generate(plan, std::span{&graph.package, 1U}, request.Value().Digest(),
                                                                       std::span{&artifact, 1U});
            REQUIRE(lock.HasValue());
            Write("packages.json", intent.dump());
            Write("packages.lock", lock.Value().SerializeCanonical());
            auto created = PackageProjectHost::Create({jobs, files, clock, wall, trust},
                                                      {.cacheRoot = std::filesystem::canonical(content.Path()) / "cache",
                                                       .platform = {"linux", "x64", "horo-sdk-2"},
                                                       .lifecycle = {.temporaryRoot = std::filesystem::canonical(content.Path()),
                                                                     .artifactGate = Horo::Tests::CreateAcceptingArtifactGate()},
                                                       .mode = Packages::PackageRestoreMode::Online},
                                                      Duration::FromMilliseconds(1000), &source);
            REQUIRE(created.HasValue());
            host = std::move(created).Value();
        }

        ~HostFixture() {
            source.Unblock();
            if (host)
                static_cast<void>(host->Close());
            jobs.Shutdown(ShutdownPolicy::Cancel);
            if (host && host->Close().HasError())
                std::terminate();
        }

        void Write(const char *name, const std::string &bytes) {
            std::ofstream output{project.Path() / ".horo" / name, std::ios::binary | std::ios::trunc};
            output << bytes;
            REQUIRE(output.good());
        }

        std::shared_ptr<PackageApplication> Acquire() {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
            while (std::chrono::steady_clock::now() < deadline) {
                auto acquired = host->Acquire(std::filesystem::canonical(project.Path()));
                if (acquired.HasValue())
                    return std::move(acquired).Value();
                REQUIRE(acquired.ErrorValue().code.Value() == "packages.host.preparing");
                std::this_thread::yield();
            }
            FAIL("Metadata preparation did not complete");
            return {};
        }
    };

    TEST_CASE("Package project root rejects stale screen boundaries and retains closed capabilities safely",
              "[packages][application][host]") {
        HostFixture fixture;
        auto application = fixture.Acquire();
        const auto generation = fixture.host->Generation();
        CHECK(generation > 0);
        CHECK(fixture.host->PumpProject(fixture.content.Path(), Packages::PackageActivationBoundary::Quiescent).HasError());
        REQUIRE(fixture.host->Retire(std::filesystem::canonical(fixture.project.Path())).HasValue());
        CHECK(application->Restore().HasError());
        REQUIRE(fixture.host->Close().HasValue());
        fixture.host.reset();
        // The alias keeps concrete closed authorities alive, and no callback can touch the destroyed host.
        CHECK(application->Install().HasError());
        REQUIRE(application->Close().HasValue());
    }

    TEST_CASE("Package project metadata changes revoke old admission before activation", "[packages][application][host]") {
        HostFixture fixture;
        auto application = fixture.Acquire();
        const auto generation = fixture.host->Generation();
        fixture.Write("packages.json", "{}");
        fixture.clock.now = Duration::FromMilliseconds(1001);
        CHECK(application->Restore().HasError());
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
        while (fixture.host->Generation() == generation && std::chrono::steady_clock::now() < deadline) {
            static_cast<void>(fixture.host->Pump(Packages::PackageActivationBoundary::Quiescent));
            std::this_thread::yield();
        }
        CHECK(fixture.host->Generation() != generation);
        CHECK(application->Activate({}).HasError());
        CHECK(fixture.host->Acquire(std::filesystem::canonical(fixture.project.Path())).HasError());
        CHECK(fixture.source.calls == 0);
    }

    TEST_CASE("Package root shutdown retains blocked I/O until cancellation joins before Close", "[packages][application][host]") {
        HostFixture fixture;
        auto application = fixture.Acquire();
        fixture.source.block = true;
        REQUIRE(application->Restore().HasValue());
        REQUIRE(fixture.source.entered.try_acquire_for(std::chrono::seconds{2}));
        REQUIRE(fixture.host->Close().HasError());
        CHECK(fixture.host->Generation() == 0);
        fixture.source.Unblock();
        fixture.jobs.Shutdown(ShutdownPolicy::Cancel);
        REQUIRE(fixture.host->Close().HasValue());
        CHECK(application->Restore().HasError());
    }
}  // namespace
