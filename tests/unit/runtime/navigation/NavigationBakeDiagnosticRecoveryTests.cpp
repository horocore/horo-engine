#include "navigation/NavigationBakeDiagnosticsFixture.h"

#include <algorithm>
#include <array>
#include <fstream>
#include <semaphore>
#include <stdexcept>
#include <thread>

namespace Horo::Application {
    using namespace Navigation;
    using namespace Navigation::TestSupport;
    using namespace DiagnosticsTestSupport;

    namespace {
        /** @brief Pauses the real dispatcher outside its queue lock while the retention burst is submitted. */
        class PausedDiagnosticSink final : public Telemetry::ISink {
        public:
            explicit PausedDiagnosticSink(std::shared_ptr<NavigationBakeDiagnostics> journal) : journal_(std::move(journal)) {}

            void Export(const Telemetry::Record &record, const Telemetry::InstrumentDescriptor *descriptor) override {
                if (record.subsystem == "test.navigation.pause") {
                    entered_.release();
                    resume_.acquire();
                } else {
                    journal_->Export(record, descriptor);
                }
            }

            void Flush() override {
                journal_->Flush();
            }

            /** @brief Bounds admission retries for the inert synchronization record, not diagnostic checkpoints. */
            void Pause() {
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
                bool accepted{};
                do {
                    accepted = Telemetry::Runtime::EmitRecord({.subsystem = "test.navigation.pause"});
                    if (!accepted)
                        std::this_thread::yield();
                } while (!accepted && std::chrono::steady_clock::now() < deadline);
                REQUIRE(accepted);
                REQUIRE(entered_.try_acquire_for(std::chrono::seconds{5}));
            }

            /** @brief Releases once on the test owner thread, including assertion-failure cleanup. */
            void Resume() {
                if (!resumed_) {
                    resumed_ = true;
                    resume_.release();
                }
            }

        private:
            std::shared_ptr<NavigationBakeDiagnostics> journal_;
            std::binary_semaphore entered_{0};
            std::binary_semaphore resume_{0};
            bool resumed_{};
        };

        /** @brief Resumes the worker before the telemetry owner drains it during stack unwinding. */
        struct ResumeDispatcher final {
            PausedDiagnosticSink &sink;

            explicit ResumeDispatcher(PausedDiagnosticSink &value) : sink(value) {}

            ResumeDispatcher(const ResumeDispatcher &) = delete;
            ResumeDispatcher &operator=(const ResumeDispatcher &) = delete;
            ResumeDispatcher(ResumeDispatcher &&) = delete;
            ResumeDispatcher &operator=(ResumeDispatcher &&) = delete;

            ~ResumeDispatcher() {
                sink.Resume();
            }
        };

        /** @brief Private source adapter failure normalized by its owning adapter. */
        class AdapterFailure final : public std::logic_error {
        public:
            AdapterFailure() : std::logic_error{"private source adapter failure"} {}
        };

        /** @brief Owned host adapter normalizing its private library failure before the public result boundary. */
        class FailingNavigator final : public INavigationDiagnosticNavigator {
        public:
            Error failure{WrapError(NavigationErrors::BakeInputFailed, MakeError(NavigationErrors::BakeInputStale))};
            bool privateException{};

            Result<bool> Navigate(const NavigationDiagnosticTarget &, const std::filesystem::path &) noexcept override {
                if (!privateException)
                    return Result<bool>::Failure(failure);
                try {
                    throw AdapterFailure{};
                } catch (const AdapterFailure &error) {
                    auto normalized = failure;
                    normalized.message = error.what();
                    return Result<bool>::Failure(std::move(normalized));
                }
            }
        };

        /** @brief Owns a valid routing destination and all stores beyond each callback. */
        struct RoutingFixture {
            Directory directory;
            NavigationBakeDiagnosticsConfig config{DiagnosticConfig(directory)};
            std::shared_ptr<NavigationBakeDiagnostics> journal{NavigationBakeDiagnostics::Create(config).Value()};
            [[no_unique_address]] TelemetryOwner telemetry{journal};
            IncrementalBakeFixture input;
            NavigationDiagnosticSource source{Source(input)};

            RoutingFixture() {
                std::ofstream(directory.root / Utf8Path(source.target.relativePath)) << "source";
            }

            [[nodiscard]] std::uint64_t Record() const {
                REQUIRE(journal->Record({.operation = 1, .event = NavigationBakeDiagnosticEvent::TileFailed, .source = source}));
                return journal->Snapshot().records.back().sequence;
            }
        };

        /** @brief Seeds hostile typed checkpoint payloads through the real history envelope and disk writer. */
        void AppendCheckpoint(Diagnostics::OperationHistorySink &history, const std::string &bytes) {
            history.Export({.subsystem = "navigation.bake",
                            .payload = Telemetry::SpanRecord{.name = "navigation.bake.diagnostic_checkpoint.v1",
                                                             .status = Telemetry::SpanStatus::Succeeded,
                                                             .fields = {{.key = "diagnostic", .value = bytes}}}},
                           nullptr);
        }

        /** @brief Corrupts independent schema/identity/value fields of one production-generated payload. */
        void AppendInvalidCheckpoints(Diagnostics::OperationHistorySink &history, const std::string &bytes,
                                      const IncrementalBakeFixture &fixture) {
            const std::array<std::pair<std::string, std::string>, 12> replacements{
                {{"\"schema\":1", "\"schema\":2"},
                 {"\"project\":1", "\"project\":2"},
                 {Asset().ToString(), Asset("3").ToString()},
                 {"\"event\":2", "\"event\":255"},
                 {"\"result\":0", "\"result\":255"},
                 {"\"operation\":1", "\"operation\":0"},
                 {"\"x\":0", "\"x\":18446744073709551615"},
                 {"\"x\":0", "\"x\":-2147483649"},
                 {"\"layer\":0", "\"layer\":65536"},
                 {FormatSha256(Source(fixture).observation.contentDigest), "invalid"},
                 {"\"scene\":7", "\"scene\":0"},
                 {"\"progress\":0.5", "\"progress\":2"}}};
            for (const auto &[from, to] : replacements) {
                INFO(from);
                auto corrupted = bytes;
                const auto offset = corrupted.find(from);
                REQUIRE(offset != std::string::npos);
                corrupted.replace(offset, from.size(), to);
                AppendCheckpoint(history, corrupted);
            }
            AppendCheckpoint(history, std::string(8193, 'x'));
            AppendCheckpoint(history, bytes.substr(0, bytes.size() - 1));
        }
    }  // namespace

    TEST_CASE("Suppression and bounded overwrite remain visible and restart readable", "[navigation][diagnostics][retention]") {
        Directory directory;
        {
            auto config = DiagnosticConfig(directory, 3, 1);
            auto journal = NavigationBakeDiagnostics::Create(config).Value();
            auto sink = std::make_shared<PausedDiagnosticSink>(journal);
            TelemetryOwner telemetry(sink);
            ResumeDispatcher resume{*sink};
            sink->Pause();
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
            sink->Resume();
            REQUIRE(Telemetry::Runtime::Flush());
            const auto snapshot = journal->Snapshot();
            REQUIRE(snapshot.suppressedRecords == 5);
            REQUIRE(snapshot.droppedRecords == 4);
            REQUIRE(snapshot.persistenceDrops == 0);
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
        RoutingFixture fixture;
        const auto &config = fixture.config;
        const auto &journal = fixture.journal;
        auto &source = fixture.source;
        const auto sequence = fixture.Record();
        std::size_t callbacks{};
        Navigator navigate{[&callbacks](const auto &, const auto &) noexcept {
            ++callbacks;
            return true;
        }};
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
            std::filesystem::remove(fixture.directory.root / Utf8Path(source.target.relativePath));
        }
        SECTION("duplicate current mapping") {
            const std::array duplicate{source, source};
            REQUIRE(journal->Navigate(sequence, config.project, config.definition, duplicate, navigate).HasError());
            REQUIRE(callbacks == 0);
            return;
        }
        REQUIRE(journal->Navigate(sequence, config.project, config.definition, std::span{&source, 1}, navigate).HasError());
        REQUIRE(callbacks == 0);
    }

    TEST_CASE("Foreign project and definition cannot invoke a valid source navigator", "[navigation][diagnostics][security]") {
        RoutingFixture fixture;
        const auto &config = fixture.config;
        const auto &journal = fixture.journal;
        auto &source = fixture.source;
        const auto sequence = fixture.Record();
        std::size_t callbacks{};
        Navigator navigate{[&callbacks](const auto &, const auto &) noexcept {
            ++callbacks;
            return true;
        }};
        SECTION("foreign project") {
            REQUIRE(journal
                        ->Navigate(sequence, NavigationDiagnosticProjectId::Create(2).Value(), config.definition, std::span{&source, 1},
                                   navigate)
                        .HasError());
            REQUIRE(callbacks == 0);
            return;
        }
        SECTION("foreign definition") {
            REQUIRE(journal->Navigate(sequence, config.project, Asset("3"), std::span{&source, 1}, navigate).HasError());
            REQUIRE(callbacks == 0);
            return;
        }
    }

    TEST_CASE("Even matching source identities cannot route malicious or symlinked paths", "[navigation][diagnostics][security]") {
        RoutingFixture routing;
        auto &source = routing.source;
        const auto &directory = routing.directory;
        SECTION("parent traversal") {
            source.target.relativePath = "../escape.scene";
        }
        SECTION("absolute") {
            const auto absolute = (directory.root / "asset.scene").generic_u8string();
            REQUIRE(std::filesystem::path{absolute}.is_absolute());
            source.target.relativePath.assign(absolute.begin(), absolute.end());
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
        const auto sequence = routing.Record();
        std::size_t callbacks{};
        Navigator navigator{[&callbacks](const auto &, const auto &) noexcept {
            ++callbacks;
            return true;
        }};
        auto routed =
            routing.journal->Navigate(sequence, routing.config.project, routing.config.definition, std::span{&source, 1}, navigator);
        REQUIRE(routed.HasError());
        REQUIRE(callbacks == 0);
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
        std::ofstream(directory.root / Utf8Path(source.target.relativePath)) << "source";
        REQUIRE(journal->Record({.operation = 1, .event = NavigationBakeDiagnosticEvent::TileFailed, .source = source}));
        std::size_t callbacks{};
        Navigator navigator{[&callbacks, &source, &directory](const auto &target, const auto &path) noexcept {
            ++callbacks;
            return target == source.target && path == directory.root / Utf8Path(source.target.relativePath);
        }};
        auto routed = journal->Navigate(journal->Snapshot().records.back().sequence, config.project, config.definition,
                                        std::span{&source, 1}, navigator);
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
            AppendInvalidCheckpoints(*config.history, std::get<std::string>(stored.front().fields.front().value), fixture);
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
        SECTION("dispatcher unavailable") { /* Intentionally leave the dispatcher stopped. */ }
        SECTION("history record exceeds configured file policy") {
            config.history = Diagnostics::OperationHistorySink::Create(
                {.directory = directory.root / "small history", .baseName = "navigation", .maxFileBytes = 128});
            REQUIRE(config.history);
            dispatcher = true;
        }
        auto journal = NavigationBakeDiagnostics::Create(config).Value();
        std::shared_ptr<PausedDiagnosticSink> paused;
        std::optional<TelemetryOwner> telemetry;
        std::optional<ResumeDispatcher> resume;
        if (dispatcher) {
            paused = std::make_shared<PausedDiagnosticSink>(journal);
            telemetry.emplace(paused);
            resume.emplace(*paused);
            paused->Pause();
        }
        REQUIRE(journal->Record({.operation = 1,
                                 .event = NavigationBakeDiagnosticEvent::Succeeded,
                                 .result = BuildOutputResult::Succeeded,
                                 .message = "completed"}));
        if (dispatcher) {
            REQUIRE(journal->Snapshot().persistenceDrops == 0);
            paused->Resume();
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

    TEST_CASE("Invalid producer evidence is counted and oversized encoded checkpoints cannot silently disappear",
              "[navigation][diagnostics][retention]") {
        Directory directory;
        auto config = DiagnosticConfig(directory);
        auto journal = NavigationBakeDiagnostics::Create(config).Value();
        TelemetryOwner telemetry(journal);
        REQUIRE_FALSE(journal->Record({.operation = 0}));
        REQUIRE(journal->Snapshot().submissionFailures == 1);
        journal->NoteSubmissionFailure();
        REQUIRE(journal->Snapshot().submissionFailures == 2);
        IncrementalBakeFixture fixture;
        auto source = Source(fixture);
        source.target.relativePath = std::string(1024, '\1');
        REQUIRE(journal->Record(
            {.operation = 1, .event = NavigationBakeDiagnosticEvent::TileFailed, .message = std::string(1024, '\1'), .source = source}));
        REQUIRE(Telemetry::Runtime::Flush());
        REQUIRE(journal->Snapshot().persistenceDrops == 1);
        REQUIRE(config.history->Snapshot().empty());
        REQUIRE(config.output->SnapshotIfChanged(0)->records.back().code.Value() == "navigation.bake.history_unavailable");
    }

    TEST_CASE("Owned source navigation failures preserve typed identity and cause after private normalization",
              "[navigation][diagnostics][routing]") {
        RoutingFixture fixture;
        FailingNavigator navigator;
        SECTION("typed host failure") { /* Return the already normalized failure. */ }
        SECTION("private adapter exception") {
            navigator.privateException = true;
        }
        const auto routed = fixture.journal->Navigate(fixture.Record(), fixture.config.project, fixture.config.definition,
                                                      std::span{&fixture.source, 1}, navigator);
        REQUIRE(routed.HasError());
        REQUIRE(routed.ErrorValue().code.Value() == navigator.failure.code.Value());
        REQUIRE(routed.ErrorValue().domain.Value() == navigator.failure.domain.Value());
        REQUIRE(routed.ErrorValue().cause.Get() == navigator.failure.cause.Get());
        if (navigator.privateException)
            REQUIRE(routed.ErrorValue().message == "private source adapter failure");
    }

    TEST_CASE("A valid source adapter may decline navigation without fabricating a failure", "[navigation][diagnostics][routing]") {
        RoutingFixture fixture;
        Navigator navigator{[](const auto &, const auto &) noexcept {
            return false;
        }};
        const auto routed = fixture.journal->Navigate(fixture.Record(), fixture.config.project, fixture.config.definition,
                                                      std::span{&fixture.source, 1}, navigator);
        REQUIRE(routed.HasValue());
        REQUIRE_FALSE(routed.Value());
    }

    TEST_CASE("Invalid UTF-8 source hints are rejected before native path conversion or host navigation",
              "[navigation][diagnostics][security]") {
        RoutingFixture fixture;
        fixture.source.target.relativePath = std::string{"\xC3\x28", 2};
        REQUIRE_FALSE(
            fixture.journal->Record({.operation = 1, .event = NavigationBakeDiagnosticEvent::TileFailed, .source = fixture.source}));
        REQUIRE(fixture.journal->Snapshot().submissionFailures == 1);
        std::size_t callbacks{};
        Navigator navigator{[&callbacks](const auto &, const auto &) noexcept {
            ++callbacks;
            return true;
        }};
        const auto routed = fixture.journal->Navigate(fixture.journal->Snapshot().records.back().sequence, fixture.config.project,
                                                      fixture.config.definition, std::span{&fixture.source, 1}, navigator);
        REQUIRE(routed.HasError());
        REQUIRE(callbacks == 0);
    }
}  // namespace Horo::Application
