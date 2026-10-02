#include "Horo/Application/NavigationBakeService.h"
#include "Horo/Navigation/Backends/RecastDetourProvider.h"
#include "navigation/IncrementalBakeFixture.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <format>
#include <fstream>
#include <thread>

namespace Horo::Application {
    using namespace Navigation;
    using namespace Navigation::TestSupport;

    namespace {
        /** @brief Owns isolated files with realistic spaces and non-ASCII project names. */
        struct Directory {
            std::filesystem::path root{
                std::filesystem::temp_directory_path() /
                std::format("horo bake diagnostics ü {}", std::chrono::steady_clock::now().time_since_epoch().count())};

            Directory() {
                REQUIRE(std::filesystem::create_directory(root));
            }

            Directory(const Directory &) = delete;
            Directory &operator=(const Directory &) = delete;

            ~Directory() {
                std::error_code error;
                std::filesystem::remove_all(root, error);
            }
        };

        [[nodiscard]] Assets::AssetId Asset(const std::string_view suffix = "1") {
            return Assets::AssetId::Parse(std::string{"00000000-0000-0000-0000-00000000000"} + std::string{suffix}).Value();
        }

        [[nodiscard]] NavigationBakeDiagnosticsConfig DiagnosticConfig(const Directory &directory, const std::size_t capacity = 128,
                                                                       const std::size_t limit = 64) {
            auto history = Diagnostics::OperationHistorySink::Create({.directory = directory.root / "history",
                                                                      .baseName = "navigation",
                                                                      .maxFileBytes = 16384,
                                                                      .maxRolledFiles = 2,
                                                                      .maxRecoveredRecords = 128});
            REQUIRE(history);
            return {.project = NavigationDiagnosticProjectId::Create(1).Value(),
                    .definition = Asset(),
                    .projectRoot = directory.root,
                    .output = std::make_shared<BuildOutputStore>(capacity),
                    .history = std::move(history),
                    .capacity = capacity,
                    .maximumRecordsPerOperation = limit};
        }

        /** @brief Starts one explicitly composed dispatcher and drains it before releasing sink owners. */
        struct TelemetryOwner {
            explicit TelemetryOwner(const std::shared_ptr<NavigationBakeDiagnostics> &journal) {
                REQUIRE(Telemetry::Runtime::Initialize({.queueCapacity = 512}, std::shared_ptr<Telemetry::ISink>{journal}));
            }

            ~TelemetryOwner() {
                static_cast<void>(Telemetry::Runtime::Shutdown());
            }
        };

        [[nodiscard]] NavigationDiagnosticSource Source(const IncrementalBakeFixture &fixture) {
            return {.observation = fixture.Observations().front(),
                    .target = {.asset = Asset("2"), .scene = {7}, .object = {11}, .relativePath = "geometry ü source.scene"}};
        }

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
                                                             const std::shared_ptr<NavigationBakeDiagnostics> &journal) {
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
                const auto found = std::ranges::find(snapshot->operations, id, &OperationRecord::id);
                if (found != snapshot->operations.end() && found->finishedAt)
                    return *found;
                std::this_thread::sleep_for(std::chrono::milliseconds{1});
            }
            FAIL("Bake did not finish within the test deadline");
            return {};
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
        auto bake = BakeConfig(directory, builder, journal);
        bake.tileLimits.maximumVertices = 1;
        auto service = NavigationBakeService::Create(bake, operations, jobs).Value();
        IncrementalBakeFixture fixture;
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
        std::ofstream(directory.root / capturedSource.target.relativePath) << "source";
        {
            auto config = DiagnosticConfig(directory);
            auto journal = NavigationBakeDiagnostics::Create(config).Value();
            TelemetryOwner telemetry(journal);
            OperationStore operations{8, 16};
            JobSystem jobs{{.workerCount = 2, .maxQueuedJobs = 16}};
            auto builder = std::make_shared<Builder>();
            auto bake = BakeConfig(directory, builder, journal);
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
        auto routed = recovered->Navigate(failure->sequence, recoveredConfig.project, recoveredConfig.definition,
                                          std::span{&capturedSource, 1}, [&](const auto &target, const auto &path) {
            invoked = true;
            return target.object.value == 11 && path == directory.root / target.relativePath;
        });
        REQUIRE(routed.HasValue());
        REQUIRE(routed.Value());
        REQUIRE(invoked);
    }

    TEST_CASE("Suppression and bounded overwrite remain visible and restart readable", "[navigation][diagnostics][retention]") {
        Directory directory;
        {
            auto config = DiagnosticConfig(directory, 3, 1);
            auto journal = NavigationBakeDiagnostics::Create(config).Value();
            TelemetryOwner telemetry(journal);
            REQUIRE(
                journal->Record({.operation = 1, .event = NavigationBakeDiagnosticEvent::Queued, .stage = "queued", .message = "queued"}));
            for (int i = 0; i < 5; ++i)
                REQUIRE_FALSE(journal->Record({.operation = 1,
                                               .event = NavigationBakeDiagnosticEvent::Progress,
                                               .stage = "tile_build",
                                               .message = "progress",
                                               .progress = 0.5F}));
            REQUIRE(journal->Record({.operation = 1,
                                     .event = NavigationBakeDiagnosticEvent::Succeeded,
                                     .stage = "complete",
                                     .result = BuildOutputResult::Succeeded,
                                     .message = "completed",
                                     .progress = 1.0F}));
            REQUIRE(Telemetry::Runtime::Flush());
            const auto snapshot = journal->Snapshot();
            REQUIRE(snapshot.suppressedRecords == 5);
            REQUIRE(snapshot.droppedRecords == 4);
            const auto output = config.output->SnapshotIfChanged(0).value();
            REQUIRE(output.droppedRecordCount == 4);
            REQUIRE(std::ranges::any_of(output.records, [](const auto &record) {
                return record.code.Value() == "navigation.bake.suppressed" && record.message.starts_with("5 ");
            }));
            REQUIRE(output.records.back().result == BuildOutputResult::Succeeded);
        }
        auto config = DiagnosticConfig(directory, 3, 1);
        auto recovered = NavigationBakeDiagnostics::Create(config).Value();
        REQUIRE(recovered->Snapshot().suppressedRecords == 5);
        REQUIRE(recovered->Snapshot().droppedRecords == 4);
        REQUIRE(config.output->SnapshotIfChanged(0)->records.size() == 3);
    }

    TEST_CASE("Navigation rejects stale or hostile source ownership before any callback", "[navigation][diagnostics][security]") {
        Directory directory;
        auto config = DiagnosticConfig(directory);
        auto journal = NavigationBakeDiagnostics::Create(config).Value();
        TelemetryOwner telemetry(journal);
        IncrementalBakeFixture fixture;
        auto source = Source(fixture);
        std::ofstream(directory.root / source.target.relativePath) << "source";
        REQUIRE(journal->Record({.operation = 1,
                                 .event = NavigationBakeDiagnosticEvent::TileFailed,
                                 .stage = "tile_build",
                                 .message = "failed",
                                 .source = source}));
        const auto sequence = journal->Snapshot().records.back().sequence;
        std::size_t callbacks{};
        const auto navigate = [&](const auto &, const auto &) {
            ++callbacks;
            return true;
        };
        SECTION("source revision changed") {
            source.observation.revision = Id<NavigationSourceRevision>(2);
        }
        SECTION("source digest changed") {
            source.observation.contentDigest = Digest(2);
        }
        SECTION("source producer changed") {
            source.observation.producer = Id<NavigationSourceProducerId>(2);
        }
        SECTION("source object changed") {
            source.target.object.value = 12;
        }
        SECTION("source asset changed") {
            source.target.asset = Asset("3");
        }
        SECTION("source deleted") {
            std::filesystem::remove(directory.root / source.target.relativePath);
        }
        SECTION("duplicate current mapping") {
            const std::array duplicate{source, source};
            REQUIRE(journal->Navigate(sequence, config.project, config.definition, duplicate, navigate).HasError());
            REQUIRE(callbacks == 0);
            return;
        }
        REQUIRE(journal->Navigate(sequence, config.project, config.definition, std::span{&source, 1}, navigate).HasError());
        REQUIRE(
            journal
                ->Navigate(sequence, NavigationDiagnosticProjectId::Create(2).Value(), config.definition, std::span{&source, 1}, navigate)
                .HasError());
        REQUIRE(journal->Navigate(sequence, config.project, Asset("3"), std::span{&source, 1}, navigate).HasError());
        REQUIRE(callbacks == 0);
    }

    TEST_CASE("Even matching source identities cannot route malicious or symlinked paths", "[navigation][diagnostics][security]") {
        Directory directory;
        auto config = DiagnosticConfig(directory);
        auto journal = NavigationBakeDiagnostics::Create(config).Value();
        TelemetryOwner telemetry(journal);
        IncrementalBakeFixture fixture;
        auto source = Source(fixture);
        SECTION("parent traversal") {
            source.target.relativePath = "../escape.scene";
        }
        SECTION("absolute") {
            source.target.relativePath = (directory.root / "asset.scene").string();
        }
        SECTION("Windows traversal") {
            source.target.relativePath = "..\\escape.scene";
        }
        SECTION("drive path") {
            source.target.relativePath = "C:/escape.scene";
        }
        SECTION("embedded NUL") {
            source.target.relativePath = std::string{"safe.scene\0other", 16};
        }
        SECTION("dot normalization") {
            source.target.relativePath = "./safe.scene";
        }
        SECTION("symlinked ancestor") {
            REQUIRE(std::filesystem::create_directory(directory.root / "real"));
            std::ofstream(directory.root / "real" / "safe.scene") << "source";
            std::filesystem::create_directory_symlink(directory.root / "real", directory.root / "link");
            source.target.relativePath = "link/safe.scene";
        }
        SECTION("symlinked file") {
            std::ofstream(directory.root / "real.scene") << "source";
            std::filesystem::create_symlink(directory.root / "real.scene", directory.root / "link.scene");
            source.target.relativePath = "link.scene";
        }
        REQUIRE(journal->Record({.operation = 1,
                                 .event = NavigationBakeDiagnosticEvent::TileFailed,
                                 .stage = "tile_build",
                                 .message = "failed",
                                 .source = source}));
        std::size_t callbacks{};
        auto routed = journal->Navigate(journal->Snapshot().records.back().sequence, config.project, config.definition,
                                        std::span{&source, 1}, [&](const auto &, const auto &) {
            ++callbacks;
            return true;
        });
        REQUIRE(routed.HasError());
        REQUIRE(callbacks == 0);
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
        auto service = NavigationBakeService::Create(BakeConfig(directory, builder, journal), operations, jobs).Value();
        IncrementalBakeFixture fixture;
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
        auto bake = BakeConfig(directory, std::make_shared<Builder>(), journal);
        auto expected = OperationState::Succeeded;
        auto result = BuildOutputResult::Succeeded;
        SECTION("native success") {}
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
        IncrementalBakeFixture fixture;
        const auto id = service->Submit(Request(fixture)).Value();
        REQUIRE(AwaitTerminal(*service, operations, id).state == expected);
        jobs.Shutdown(ShutdownPolicy::Drain);
        REQUIRE(Telemetry::Runtime::Flush());
        const auto snapshot = journal->Snapshot();
        REQUIRE(snapshot.records.back().result == result);
        REQUIRE(std::ranges::count_if(snapshot.records, [](const auto &record) {
            return record.result != BuildOutputResult::None;
        }) == 1);
        if (expected == OperationState::Failed)
            REQUIRE_FALSE(snapshot.records.back().causeCode.empty());
        REQUIRE(config.output->SnapshotIfChanged(0)->records.back().result == result);
    }

    TEST_CASE("Scene-only and asset-only destinations route after fresh ownership validation", "[navigation][diagnostics][routing]") {
        Directory directory;
        auto config = DiagnosticConfig(directory);
        auto journal = NavigationBakeDiagnostics::Create(config).Value();
        TelemetryOwner telemetry(journal);
        IncrementalBakeFixture fixture;
        auto source = Source(fixture);
        SECTION("asset") {
            source.target.scene = {};
            source.target.object = {};
        }
        SECTION("scene object") {
            source.target.asset = {};
        }
        std::ofstream(directory.root / source.target.relativePath) << "source";
        REQUIRE(journal->Record({.operation = 1, .event = NavigationBakeDiagnosticEvent::TileFailed, .source = source}));
        std::size_t callbacks{};
        auto routed = journal->Navigate(journal->Snapshot().records.back().sequence, config.project, config.definition,
                                        std::span{&source, 1}, [&](const auto &target, const auto &path) {
            ++callbacks;
            return target == source.target && path == directory.root / source.target.relativePath;
        });
        REQUIRE(routed.HasValue());
        REQUIRE(routed.Value());
        REQUIRE(callbacks == 1);
    }

    TEST_CASE("Restart rejects corrupt foreign and unsupported checkpoint payloads", "[navigation][diagnostics][recovery]") {
        Directory directory;
        {
            auto config = DiagnosticConfig(directory);
            auto journal = NavigationBakeDiagnostics::Create(config).Value();
            IncrementalBakeFixture fixture;
            {
                TelemetryOwner telemetry(journal);
                REQUIRE(journal->Record({.operation = 1,
                                         .event = NavigationBakeDiagnosticEvent::TileFailed,
                                         .stage = "tile_build",
                                         .message = "failed",
                                         .tile = fixture.Tiles().front().key,
                                         .source = Source(fixture),
                                         .progress = 0.5F}));
                REQUIRE(Telemetry::Runtime::Flush());
            }
            const auto stored = config.history->Snapshot();
            REQUIRE(stored.size() == 1);
            auto bytes = std::get<std::string>(stored.front().fields.front().value);
            const auto replace = [&](const std::string_view from, const std::string_view to) {
                const auto offset = bytes.find(from);
                REQUIRE(offset != std::string::npos);
                bytes.replace(offset, from.size(), to);
            };
            SECTION("unsupported schema") {
                replace("\"schema\":1", "\"schema\":2");
            }
            SECTION("foreign project") {
                replace("\"project\":1", "\"project\":2");
            }
            SECTION("foreign definition") {
                replace(Asset().ToString(), Asset("3").ToString());
            }
            SECTION("unknown event") {
                replace("\"event\":2", "\"event\":255");
            }
            SECTION("unknown result") {
                replace("\"result\":0", "\"result\":255");
            }
            SECTION("zero operation") {
                replace("\"operation\":1", "\"operation\":0");
            }
            SECTION("oversized unsigned coordinate") {
                replace("\"x\":0", "\"x\":18446744073709551615");
            }
            SECTION("undersized signed coordinate") {
                replace("\"x\":0", "\"x\":-2147483649");
            }
            SECTION("oversized layer") {
                replace("\"layer\":0", "\"layer\":65536");
            }
            SECTION("invalid digest") {
                replace(FormatSha256(Source(fixture).observation.contentDigest), "invalid");
            }
            SECTION("orphan object") {
                replace("\"scene\":7", "\"scene\":0");
            }
            SECTION("invalid progress") {
                replace("\"progress\":0.5", "\"progress\":2");
            }
            SECTION("oversized payload") {
                bytes = std::string(8193, 'x');
            }
            SECTION("truncated payload") {
                bytes.pop_back();
            }
            config.history->Export({.subsystem = "navigation.bake",
                                    .payload = Telemetry::SpanRecord{.name = "navigation.bake.diagnostic_checkpoint.v1",
                                                                     .status = Telemetry::SpanStatus::Succeeded,
                                                                     .fields = {{.key = "diagnostic", .value = bytes}}}},
                                   nullptr);
            config.history->Flush();
        }
        auto recoveredConfig = DiagnosticConfig(directory);
        auto recovered = NavigationBakeDiagnostics::Create(recoveredConfig).Value();
        REQUIRE(recovered->Snapshot().records.size() == 1);
        REQUIRE(recoveredConfig.output->SnapshotIfChanged(0)->records.size() == 1);
    }

    TEST_CASE("Persistence rejection and sink failure remain separate from live terminal truth", "[navigation][diagnostics][retention]") {
        Directory directory;
        auto config = DiagnosticConfig(directory);
        bool dispatcher{};
        SECTION("dispatcher unavailable") {}
        SECTION("history record exceeds configured file policy") {
            config.history = Diagnostics::OperationHistorySink::Create(
                {.directory = directory.root / "small history", .baseName = "navigation", .maxFileBytes = 128});
            REQUIRE(config.history);
            dispatcher = true;
        }
        auto journal = NavigationBakeDiagnostics::Create(config).Value();
        if (dispatcher)
            REQUIRE(Telemetry::Runtime::Initialize({.queueCapacity = 512}, std::shared_ptr<Telemetry::ISink>{journal}));
        REQUIRE(journal->Record({.operation = 1,
                                 .event = NavigationBakeDiagnosticEvent::Succeeded,
                                 .result = BuildOutputResult::Succeeded,
                                 .message = "completed"}));
        if (dispatcher) {
            REQUIRE(Telemetry::Runtime::Flush());
            REQUIRE(journal->Snapshot().historyFailures == 1);
            REQUIRE(journal->Snapshot().persistenceDrops == 0);
            REQUIRE(Telemetry::Runtime::Shutdown());
        } else {
            REQUIRE(journal->Snapshot().persistenceDrops == 1);
            REQUIRE(journal->Snapshot().historyFailures == 0);
            REQUIRE(config.output->SnapshotIfChanged(0)->records.back().code.Value() == "navigation.bake.history_unavailable");
        }
        REQUIRE(journal->Snapshot().records.back().result == BuildOutputResult::Succeeded);
    }
}  // namespace Horo::Application
