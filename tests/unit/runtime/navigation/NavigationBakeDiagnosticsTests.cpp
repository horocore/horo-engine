#include "Horo/Navigation/Backends/RecastDetourProvider.h"
#include "PublicationOperationId.h"
#include "navigation/IncrementalBakeFixture.h"
#include "navigation/NavigationBakeDiagnosticsFixture.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <format>
#include <fstream>
#include <thread>

namespace Horo::Application {
    using namespace Navigation;
    using namespace Navigation::TestSupport;
    using namespace DiagnosticsTestSupport;

    namespace {
        /** @brief Executes the concrete Recast builder with an optional cancellation barrier. */
        class Builder final : public INavigationMeshBuilder {
        public:
            std::shared_ptr<const INavigationMeshBuilder> native{CreateRecastDetourNavigationMeshBuilder().Value()};
            mutable std::atomic<bool> entered{};
            std::atomic<bool> pause{};

            Result<NavigationTileBuildResult> BuildTile(const NavigationTileBuildRequest &request,
                                                        const CancellationToken &token) const override {
                entered.store(true);
                while (pause.load() && !token.IsCancellationRequested())
                    std::this_thread::yield();
                return native->BuildTile(request, token);
            }
        };

        [[nodiscard]] NavigationBakeServiceConfig BakeConfig(const Directory &directory, const std::shared_ptr<Builder> &builder,
                                                             const std::shared_ptr<NavigationBakeDiagnostics> &journal,
                                                             const IncrementalBakeFixture &fixture) {
            auto authority = std::make_shared<NavigationBakeSourceAuthority>();
            REQUIRE(authority->UpdateCurrent(fixture.revisions, fixture.Observations()).HasValue());
            return {.definition = Asset(),
                    .artifactType = Assets::AssetTypeId::Parse("core.navmesh").Value(),
                    .target = AssetCookTargetId::Parse("headless-null").Value(),
                    .cacheRoot = directory.root / "cache",
                    .targetRoot = directory.root / "cooked",
                    .builder = builder,
                    .files = std::make_shared<NativeDurableFileSystem>(),
                    .budget = {.maximumConcurrentJobs = 1,
                               .maximumResidentBytes = 1024ULL * 1024ULL * 1024ULL,
                               .maximumTemporaryBytes = 128U * 1024U * 1024U,
                               .maximumWorkItems = 8,
                               .maximumWorkUnits = 1024ULL * 1024ULL * 1024ULL,
                               .childDrainTimeout = Duration::FromMilliseconds(2000)},
                    .sourceAuthority = std::move(authority),
                    .newOperationId = Horo::TestSupport::NewPublicationOperationId,
                    .diagnostics = journal};
        }

        [[nodiscard]] NavigationBakeRequest Request(const IncrementalBakeFixture &fixture) {
            return {.input = fixture.Input(),
                    .compatibility = fixture.compatibility,
                    .tiles = fixture.Tiles(),
                    .sources = fixture.Observations(),
                    .diagnosticSources = {Source(fixture)}};
        }

        [[nodiscard]] OperationRecord AwaitTerminal(NavigationBakeService &service, const OperationStore &operations,
                                                    const OperationId id) {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
            while (std::chrono::steady_clock::now() < deadline) {
                service.Pump();
                const auto snapshot = operations.SnapshotIfChanged(0);
                if (const auto found = std::ranges::find(snapshot->operations, id, &OperationRecord::id);
                    found != snapshot->operations.end() && found->finishedAt)
                    return *found;
                std::this_thread::sleep_for(std::chrono::milliseconds{1});
            }
            FAIL("Bake did not finish within the test deadline");
            return {};
        }

        /** @brief Check terminal truth independently of trailing persistence warnings. */
        void RequireTerminalProjection(const BuildOutputStore &store, const OperationId id, const BuildOutputResult result) {
            const auto output = store.SnapshotIfChanged(0).value();
            REQUIRE(std::ranges::count_if(output.records, [id, result](const auto &record) {
                return record.operationId == id && record.result == result;
            }) == 1);
            REQUIRE(std::ranges::count_if(output.records, [id](const auto &record) {
                return record.operationId == id && record.result != BuildOutputResult::None;
            }) == 1);
        }

        void AwaitBuilder(const Builder &builder) {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
            while (!builder.entered.load() && std::chrono::steady_clock::now() < deadline)
                std::this_thread::yield();
            REQUIRE(builder.entered.load());
        }
    }  // namespace

    TEST_CASE("Actual Recast failure retains tile profile source and suggested fix in shared output", "[navigation][diagnostics]") {
        Directory directory;
        auto config = DiagnosticConfig(directory);
        auto journal = NavigationBakeDiagnostics::Create(config).Value();
        TelemetryOwner telemetry(journal);
        OperationStore operations{8, 16};
        JobSystem jobs{{.workerCount = 2, .maxQueuedJobs = 16}};
        auto builder = std::make_shared<Builder>();
        IncrementalBakeFixture fixture;
        auto bake = BakeConfig(directory, builder, journal, fixture);
        bake.tileLimits.maximumVertices = 1;
        auto service = NavigationBakeService::Create(bake, operations, jobs).Value();
        const auto id = service->Submit(Request(fixture)).Value();
        REQUIRE(AwaitTerminal(*service, operations, id).state == OperationState::Failed);
        jobs.Shutdown(ShutdownPolicy::Drain);
        REQUIRE(Telemetry::Runtime::Flush());
        const auto snapshot = journal->Snapshot();
        const auto failed =
            std::ranges::find(snapshot.records, NavigationBakeDiagnosticEvent::TileFailed, &NavigationBakeDiagnosticRecord::event);
        REQUIRE(failed != snapshot.records.end());
        REQUIRE(failed->tile->profile == fixture.profile.id);
        REQUIRE(failed->tile->tile.x == 0);
        REQUIRE(failed->source == Source(fixture));
        REQUIRE(failed->severity == DiagnosticSeverity::Error);
        REQUIRE_FALSE(failed->causeCode.empty());
        REQUIRE_FALSE(failed->suggestedFix.empty());
        REQUIRE(std::ranges::count_if(snapshot.records, [](const auto &record) {
            return record.result != BuildOutputResult::None;
        }) == 1);
        const auto output = config.output->SnapshotIfChanged(0).value();
        REQUIRE(std::ranges::any_of(output.records, [](const auto &record) {
            return record.code.Value() == "navigation.bake.tile_failed" && record.message.find("profile=1") != std::string::npos &&
                   !record.source;
        }));
        REQUIRE(snapshot.persistenceDrops == 0);
    }

    TEST_CASE("Bake diagnostics survive observer closure and project restart without live operation aliasing",
              "[navigation][diagnostics][recovery]") {
        Directory directory;
        IncrementalBakeFixture fixture;
        const auto capturedSource = Source(fixture);
        std::ofstream(directory.root / Utf8Path(capturedSource.target.relativePath)) << "source";
        {
            auto config = DiagnosticConfig(directory);
            auto journal = NavigationBakeDiagnostics::Create(config).Value();
            TelemetryOwner telemetry(journal);
            OperationStore operations{8, 16};
            JobSystem jobs{{.workerCount = 2, .maxQueuedJobs = 16}};
            auto builder = std::make_shared<Builder>();
            auto bake = BakeConfig(directory, builder, journal, fixture);
            bake.tileLimits.maximumVertices = 1;
            auto service = NavigationBakeService::Create(bake, operations, jobs).Value();
            auto panelSnapshot = config.output->SnapshotIfChanged(0);
            panelSnapshot.reset();
            const auto id = service->Submit(Request(fixture)).Value();
            REQUIRE(AwaitTerminal(*service, operations, id).state == OperationState::Failed);
            service.reset();
            jobs.Shutdown(ShutdownPolicy::Drain);
            REQUIRE(Telemetry::Runtime::Flush());
            REQUIRE(config.output->SnapshotIfChanged(0)->records.back().result == BuildOutputResult::Failed);
        }
        auto recoveredConfig = DiagnosticConfig(directory);
        auto recovered = NavigationBakeDiagnostics::Create(recoveredConfig).Value();
        const auto snapshot = recovered->Snapshot();
        REQUIRE_FALSE(snapshot.records.empty());
        REQUIRE(std::ranges::all_of(snapshot.records, &NavigationBakeDiagnosticRecord::recovered));
        REQUIRE(std::ranges::all_of(recoveredConfig.output->SnapshotIfChanged(0)->records, [](const auto &record) {
            return !record.operationId;
        }));
        const auto failure =
            std::ranges::find(snapshot.records, NavigationBakeDiagnosticEvent::TileFailed, &NavigationBakeDiagnosticRecord::event);
        REQUIRE(failure != snapshot.records.end());
        bool invoked{};
        Navigator navigator{[&invoked, &directory](const auto &target, const auto &path) noexcept {
            invoked = true;
            return target.object.value == 11 && path == directory.root / Utf8Path(target.relativePath);
        }};
        auto routed = recovered->Navigate(failure->sequence, recoveredConfig.project, recoveredConfig.definition,
                                          std::span{&capturedSource, 1}, navigator);
        REQUIRE(routed.HasValue());
        REQUIRE(routed.Value());
        REQUIRE(invoked);
    }

    TEST_CASE("Production progress cancellation reload and shutdown retain terminal truth", "[navigation][diagnostics][lifecycle]") {
        Directory directory;
        auto config = DiagnosticConfig(directory);
        auto journal = NavigationBakeDiagnostics::Create(config).Value();
        TelemetryOwner telemetry(journal);
        OperationStore operations{8, 16};
        JobSystem jobs{{.workerCount = 2, .maxQueuedJobs = 16}};
        auto builder = std::make_shared<Builder>();
        builder->pause.store(true);
        IncrementalBakeFixture fixture;
        auto service = NavigationBakeService::Create(BakeConfig(directory, builder, journal, fixture), operations, jobs).Value();
        const auto id = service->Submit(Request(fixture)).Value();
        AwaitBuilder(*builder);
        const auto running = journal->Snapshot();
        REQUIRE(std::ranges::any_of(running.records, [](const auto &record) {
            return record.event == NavigationBakeDiagnosticEvent::Progress && record.stage == "tile_build";
        }));
        SECTION("explicit cancellation") {
            REQUIRE(operations.RequestCancel(id));
        }
        SECTION("project source reload") {
            service->Invalidate();
        }
        SECTION("facade shutdown") {
            service->Close();
        }
        REQUIRE(AwaitTerminal(*service, operations, id).state == OperationState::Cancelled);
        service.reset();
        jobs.Shutdown(ShutdownPolicy::Drain);
        REQUIRE(Telemetry::Runtime::Flush());
        REQUIRE(journal->Snapshot().records.back().result == BuildOutputResult::Cancelled);
        REQUIRE(std::ranges::count_if(journal->Snapshot().records, [](const auto &record) {
            return record.result != BuildOutputResult::None;
        }) == 1);
    }

    TEST_CASE("Production success and scheduler rejection preserve one terminal diagnostic", "[navigation][diagnostics][lifecycle]") {
        Directory directory;
        auto config = DiagnosticConfig(directory);
        auto journal = NavigationBakeDiagnostics::Create(config).Value();
        TelemetryOwner telemetry(journal);
        OperationStore operations{8, 16};
        JobSystem jobs{{.workerCount = 2, .maxQueuedJobs = 16}};
        IncrementalBakeFixture fixture;
        auto bake = BakeConfig(directory, std::make_shared<Builder>(), journal, fixture);
        auto expected = OperationState::Succeeded;
        auto result = BuildOutputResult::Succeeded;
        bool unavailableHistory{};
        SECTION("native success") { /* Exercise the unmodified production configuration. */ }
        SECTION("successful operation with unavailable persistence") {
            REQUIRE(Telemetry::Runtime::Shutdown());
            unavailableHistory = true;
        }
        SECTION("admitted budget failure") {
            bake.budget.maximumWorkItems = 1;
            expected = OperationState::Failed;
            result = BuildOutputResult::Failed;
        }
        SECTION("stopped scheduler") {
            jobs.Shutdown(ShutdownPolicy::Drain);
            expected = OperationState::Failed;
            result = BuildOutputResult::Failed;
        }
        auto service = NavigationBakeService::Create(bake, operations, jobs).Value();
        const auto id = service->Submit(Request(fixture)).Value();
        REQUIRE(AwaitTerminal(*service, operations, id).state == expected);
        jobs.Shutdown(ShutdownPolicy::Drain);
        if (!unavailableHistory)
            REQUIRE(Telemetry::Runtime::Flush());
        const auto snapshot = journal->Snapshot();
        REQUIRE(snapshot.records.back().result == result);
        REQUIRE(std::ranges::count_if(snapshot.records, [](const auto &record) {
            return record.result != BuildOutputResult::None;
        }) == 1);
        if (expected == OperationState::Failed)
            REQUIRE_FALSE(snapshot.records.back().causeCode.empty());
        if (unavailableHistory) {
            REQUIRE(snapshot.persistenceDrops > 0);
            const auto output = config.output->SnapshotIfChanged(0).value();
            REQUIRE(std::ranges::any_of(output.records, [](const auto &record) {
                return record.code.Value() == "navigation.bake.history_unavailable";
            }));
        }
        RequireTerminalProjection(*config.output, id, result);
    }

}  // namespace Horo::Application
