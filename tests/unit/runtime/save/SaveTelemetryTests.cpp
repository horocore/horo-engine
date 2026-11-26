#include "Horo/Application/HostObservability.h"
#include "Horo/Foundation/Telemetry/Operation.h"
#include "Horo/PlatformServices/PlatformRequest.h"
#include "Horo/Runtime/Save/SaveErrors.h"
#include "Horo/Runtime/Save/SaveMigration.h"
#include "Horo/Runtime/Save/SaveOperation.h"
#include "Horo/Runtime/Save/SaveStorageAdapter.h"
#include "Horo/Runtime/Save/SaveTelemetry.h"
#include "SaveTestUtils.h"
#include "support/AllocationProbe.h"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <type_traits>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <io.h>
#else
#include <csignal>
#include <unistd.h>
#endif
#include <fstream>
#include <iterator>
#include <string>

namespace {
    using namespace Horo;
    using namespace Horo::Runtime;

    static_assert(!std::is_default_constructible_v<SaveTelemetryRegistration>);
    static_assert(!std::is_copy_constructible_v<SaveTelemetryRegistration>);

    class FixtureFailure final : public std::runtime_error {
    public:
        using std::runtime_error::runtime_error;
    };

    /** @brief Atomically claims a fresh test directory before any fixture file is opened. */
    std::filesystem::path CreateTestDirectory() {
        const auto parent = std::filesystem::temp_directory_path();
        const auto identity = std::chrono::steady_clock::now().time_since_epoch().count();
        for (std::size_t attempt = 0; attempt < 32; ++attempt) {
            const auto path = parent / std::format("horo-save-telemetry-{}-{}", identity, attempt);
            if (std::filesystem::create_directory(path))
                return path;
        }
        throw FixtureFailure("Save telemetry fixture could not claim a fresh directory");
    }

    /** @brief Starts the host with the Save export policy used by the product entry points. */
    std::unique_ptr<Application::HostObservabilitySession> StartSaveHost(const std::filesystem::path &directory,
                                                                        const bool includeSave = true,
                                                                        const bool captureProcessFaults = false) {
        return Application::HostObservabilitySession::Start(
            {.logging = {.logDirectory = directory,
                         .baseName = "save",
                         .queueCapacity = 4096,
                         .subsystemPrefixes = includeSave ? std::vector<std::string>{} : std::vector<std::string>{"other"},
                         .metricCollectionLevel =
                             includeSave ? Telemetry::MetricCollectionLevel::Core : Telemetry::MetricCollectionLevel::Off,
                         .echoToStderr = false,
                         .captureProcessFaults = captureProcessFaults},
             .summaries = {&SummarizeSaveTelemetry},
             .summaryOnlyNamespaces = {std::string{SaveTelemetryDiagnosticNamespace}, std::string{SaveDiagnosticErrorDomain},
                                       std::string{SaveDiagnosticCodeNamespace}}});
    }

    class Session final {
    public:
        explicit Session(const bool includeSave = true) : host(StartSaveHost(directory, includeSave)) {}

        ~Session() {
            registration.reset();
            host.reset();
            std::error_code error;
            std::filesystem::remove_all(directory, error);
        }

        bool Register() {
            auto created = SaveTelemetryRegistration::Create();
            if (created.HasError())
                return false;
            registration = std::move(created).Value();
            return true;
        }

        std::filesystem::path directory{CreateTestDirectory()};
        std::unique_ptr<Application::HostObservabilitySession> host;
        std::unique_ptr<SaveTelemetryRegistration> registration;
    };

    std::string Read(const std::filesystem::path &path) {
        std::ifstream input{path, std::ios::binary};
        return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
    }

    /** @brief Retries a fixture record when the bounded telemetry queue is momentarily contended. */
    bool PublishLog(const std::string_view category, const std::string_view message,
                    const std::vector<Telemetry::Field> &fields = {}) {
        for (std::size_t attempt = 0; attempt < 1000; ++attempt) {
            if (Telemetry::Runtime::EmitRecord({.subsystem = std::string{category},
                                               .payload = Telemetry::LogRecord{.severity = Log::Level::Error,
                                                                               .category = std::string{category},
                                                                               .message = std::string{message},
                                                                               .fields = fields}}))
                return true;
            std::this_thread::yield();
        }
        return false;
    }

#if defined(_WIN32)
    constexpr auto DuplicateDescriptor = &_dup;
    constexpr auto ReplaceDescriptor = &_dup2;
    constexpr auto CloseDescriptor = &_close;
    constexpr auto FileDescriptor = &_fileno;
#else
    constexpr auto DuplicateDescriptor = &dup;
    constexpr auto ReplaceDescriptor = &dup2;
    constexpr auto CloseDescriptor = &close;
    constexpr auto FileDescriptor = &fileno;
#endif

    /** @brief Captures bounded emergency output and restores process stderr before assertion reporting. */
    class EmergencyCapture final {
    public:
        explicit EmergencyCapture(const std::filesystem::path &path) : file_(std::fopen(path.string().c_str(), "w+b")) {
            std::fflush(stderr);
            if (!file_ || saved_ < 0 || ReplaceDescriptor(FileDescriptor(file_), FileDescriptor(stderr)) < 0) {
                if (file_)
                    std::fclose(file_);
                if (saved_ >= 0)
                    CloseDescriptor(saved_);
                throw FixtureFailure("Emergency capture could not acquire stderr");
            }
        }

        EmergencyCapture(const EmergencyCapture &) = delete;
        EmergencyCapture &operator=(const EmergencyCapture &) = delete;

        ~EmergencyCapture() {
            std::fflush(stderr);
            ReplaceDescriptor(saved_, FileDescriptor(stderr));
            CloseDescriptor(saved_);
            std::fclose(file_);
        }

        std::string ReadOutput() const {
            std::fflush(stderr);
            std::rewind(file_);
            std::array<char, 4096> bytes{};
            return {bytes.data(), std::fread(bytes.data(), 1, bytes.size(), file_)};
        }

    private:
        std::FILE *file_;
        int saved_{DuplicateDescriptor(FileDescriptor(stderr))};
    };

    /** @brief Holds the dispatcher outside its queue lock for a bounded deterministic queue-full fixture. */
    class BlockingSink : public Telemetry::ISink {
    public:
        void Export(const Telemetry::Record &, const Telemetry::InstrumentDescriptor *) override {
            std::unique_lock lock(mutex_);
            entered_ = true;
            condition_.notify_all();
            condition_.wait_for(lock, std::chrono::seconds{2}, [this] {
                return released_;
            });
        }

        void Flush() override {
            // The blocking fixture retains no records to flush.
        }

        bool WaitUntilEntered() {
            std::unique_lock lock(mutex_);
            return condition_.wait_for(lock, std::chrono::seconds{2}, [this] {
                return entered_;
            });
        }

        /** @brief Retries permitted contention drops before requiring the dispatcher to enter the blocking sink. */
        bool Start(const std::string_view event) {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
            do {
                // EmitEvent is nonblocking: even an empty queue may reject a record while its writer owns the queue lock.
                if (Telemetry::Runtime::EmitEvent("runtime.save", event, Log::Level::Info, "Safe blocking fixture"))
                    return WaitUntilEntered();
                std::this_thread::yield();
            } while (std::chrono::steady_clock::now() < deadline);
            return false;
        }

        void Release() {
            std::lock_guard lock(mutex_);
            released_ = true;
            condition_.notify_all();
        }

    private:
        std::mutex mutex_;
        std::condition_variable condition_;
        bool entered_{};
        bool released_{};
    };

#if HORO_ENABLE_TELEMETRY
    /** @brief Copies typed Save telemetry for assertions after the dispatcher is joined. */
    class SavePrivacySink final : public BlockingSink {
    public:
        struct Metric final {
            Telemetry::Record record;
            Telemetry::InstrumentDescriptor descriptor;
        };

        void Export(const Telemetry::Record &record, const Telemetry::InstrumentDescriptor *descriptor) override {
            BlockingSink::Export(record, descriptor);
            if (std::holds_alternative<Telemetry::SpanRecord>(record.payload))
                spans.push_back(record);
            if (descriptor && std::holds_alternative<Telemetry::MetricRecord>(record.payload))
                metrics.emplace_back(record, *descriptor);
        }

        std::vector<Telemetry::Record> spans;
        std::vector<Metric> metrics;
    };

    /** @brief Requires exactly the public span fields and the isolated numeric lineage. */
    void CheckSaveSpanPrivacy(const Telemetry::Record &record) {
        CHECK(record.subsystem == "runtime.save");
        const std::array lineage{Log::MdcField{"save.operation", "81"}, Log::MdcField{"save.parent_operation", "73"}};
        CHECK(std::ranges::is_permutation(record.context.Fields(), lineage));
        const auto &span = std::get<Telemetry::SpanRecord>(record.payload);
        CHECK(span.operationId == 81);
        CHECK(span.parentOperationId == 73);
        CHECK(span.status == Telemetry::SpanStatus::Failed);
        CHECK(span.name == "queue");
        constexpr std::array<std::string_view, 9> keys{"stage",   "outcome",      "operation",   "duration_ns",     "bytes",
                                                       "retries", "dropped_work", "queue_depth", "failure_category"};
        REQUIRE(std::ranges::equal(span.fields, keys, {}, &Telemetry::Field::key));
        CHECK(span.fields[0].value == Telemetry::FieldValue{std::string{"queue"}});
        CHECK(span.fields[1].value == Telemetry::FieldValue{std::string{"failed"}});
        for (const auto &field : std::span{span.fields}.subspan(2))
            CHECK(std::holds_alternative<std::uint64_t>(field.value));
    }

    /** @brief Checks the registered vocabulary and resolves each emitted dimension ID against it. */
    void CheckSaveMetricPrivacy(const SavePrivacySink::Metric &captured, std::string_view name, std::size_t dimensionCount) {
        const auto &descriptor = captured.descriptor;
        CAPTURE(name);
        CHECK(descriptor.name == name);
        CHECK(descriptor.subsystem == "runtime.save");
        CHECK(captured.record.context.Fields().empty());
        const auto &metric = std::get<Telemetry::MetricRecord>(captured.record.payload);
        REQUIRE(metric.dimensionCount == dimensionCount);
        REQUIRE(descriptor.dimensions.size() == dimensionCount);
        if (dimensionCount == 0)
            return;
        constexpr std::array<std::string_view, 9> stages{"capture", "encode", "migrate",  "commit",     "restore",
                                                         "sync",    "queue",  "recovery", "participant"};
        const auto &stage = descriptor.dimensions.front();
        CHECK(stage.key == "stage");
        REQUIRE(std::ranges::equal(stage.allowedValues, stages));
        CHECK(metric.dimensionValueIds[0] == 7);  // IDs are one-based, so this selects queue.
        if (dimensionCount == 2) {
            constexpr std::array<std::string_view, 4> outcomes{"succeeded", "failed", "cancelled", "interrupted"};
            const auto &outcome = descriptor.dimensions.back();
            CHECK(outcome.key == "outcome");
            REQUIRE(std::ranges::equal(outcome.allowedValues, outcomes));
            CHECK(metric.dimensionValueIds[1] == 2);  // Failed.
        }
    }
#endif

    /** @brief Counts accepted terminal logs separately from permitted low-severity dispatcher drops. */
    std::size_t CountTerminalRecords(const std::string_view log) {
        std::size_t count{};
        std::size_t offset{};
        while ((offset = log.find("Save operation terminal outcome", offset)) != std::string::npos) {
            ++count;
            ++offset;
        }
        return count;
    }

    class Provider final : public ISaveStorageProvider {
    public:
        SaveStorageCapabilities Capabilities() const noexcept override {
            return SaveStorageCapabilities::All();
        }

        Result<SaveStorageValue> Execute(const SaveStorageRequest &, const CancellationToken &) override {
            context = Telemetry::CaptureOperationContext();
            Log::Logger::Write("runtime.save.provider", Log::Level::Warn, "private-provider-log");
            Log::Logger::WriteEmergency("runtime.save.provider", Log::Level::Error, "private-provider-emergency");
            logAccepted = Telemetry::Runtime::EmitRecord(
                {.subsystem = "runtime.save.provider",
                 .context = context.diagnosticContext,
                 .payload = Telemetry::LogRecord{.category = "runtime.save.provider", .message = "private-provider-record"}});
            metricRegistered = static_cast<bool>(Telemetry::Runtime::RegisterCounter({.name = "runtime.save.provider.raw_count",
                                                                                      .subsystem = "runtime.save.provider",
                                                                                      .description = "private-provider-metric"}));
            std::jthread forwarded{[this, snapshot = context.diagnosticContext.With("provider.test", "forwarded")] {
                explicitEventAccepted = Telemetry::Runtime::EmitEvent("runtime.save.provider", "maintenance", Log::Level::Info,
                                                                      "private-provider-explicit-context", snapshot);
                const Log::ScopedLogContext scope{snapshot};
                forwardedEventAccepted =
                    Telemetry::Runtime::EmitEvent("runtime.save.provider", "maintenance", Log::Level::Info, "private-provider-forwarded");
            }};
            return fail ? Result<SaveStorageValue>::Failure(MakeError(SaveErrors::StoragePermanentIo, "private-provider-account"))
                        : Result<SaveStorageValue>::Success(true);
        }

        bool fail{};
        bool logAccepted{};
        bool metricRegistered{};
        bool explicitEventAccepted{};
        bool forwardedEventAccepted{};
        Telemetry::OperationContext context;
    };

    SaveStorageRequest Request() {
        using namespace Horo::Runtime::Test;
        return {.kind = SaveStorageOperationKind::Exists,
                .source = {.namespaceAccess = {.expected = {.product = Id<ProductStorageId>(1),
                                                            .environment = Id<EnvironmentStorageId>(2),
                                                            .owner = ServerWorldOwner{Id<ServerStorageOwnerId>(3)}},
                                               .expectedRevision = 1},
                           .slot = Id<SaveGameSlotId>(1)}};
    }
}  // namespace

TEST_CASE("Save spans and metric dimensions exclude payload, display and provider data", "[save][telemetry][privacy]") {
#if HORO_ENABLE_TELEMETRY
    Session session;
    REQUIRE(session.host);
    auto sink = std::make_shared<SavePrivacySink>();
    REQUIRE(Telemetry::Runtime::Initialize({.queueCapacity = 64}, sink));
    REQUIRE(session.Register());
    REQUIRE(sink->Start("test.privacy.barrier"));
    {
        const Telemetry::ScopedOperationContext parent{
            {.operationId = 73,
             .diagnosticContext =
                 Log::LogContextSnapshot{{{"save.display", "private display name"}, {"save.payload", "private payload"}}}}};
        SaveStageObservation observation{SaveTelemetryStage::Queue, 81};
        REQUIRE(observation.IsActive());
        const auto error = WrapError(SaveErrors::StoragePermanentIo, MakeError(SaveErrors::CaptureIncomplete, "private provider data"),
                                     "private save title");
        observation.Fail(error, {.bytes = 29, .retries = 2, .droppedWork = 1, .queueDepth = 3});
    }
    session.registration.reset();
    sink->Release();
    REQUIRE(Telemetry::Runtime::Shutdown());

    REQUIRE(sink->spans.size() == 1);
    CheckSaveSpanPrivacy(sink->spans.front());
    constexpr std::array<std::string_view, 6> names{"save.stage.outcomes", "save.stage.duration", "save.stage.bytes",
                                                    "save.stage.retries",  "save.stage.dropped",  "save.queue.depth"};
    constexpr std::array<std::size_t, 6> dimensions{2, 1, 1, 1, 1, 0};
    REQUIRE(sink->metrics.size() == names.size());
    for (std::size_t index = 0; index < names.size(); ++index)
        CheckSaveMetricPrivacy(sink->metrics[index], names[index], dimensions[index]);
#else
    SKIP("Save telemetry is disabled in this build.");
#endif
}

TEST_CASE("Save telemetry production storage jobs isolate private ambient context and export retained summaries",
          "[save][telemetry][privacy][bundle]") {
    Session session;
    REQUIRE(session.host);
    REQUIRE(session.Register());
    REQUIRE(SaveTelemetryRegistration::Create().HasError());
    auto provider = std::make_shared<Provider>();
    JobSystem jobs{JobSystemConfig{.workerCount = 1, .maxQueuedJobs = 4}};
    SaveStorageAdapter adapter{jobs, provider};
    const EmergencyCapture emergency{session.directory / "provider-emergency.txt"};
    std::optional<SaveStorageOperation> operation;
    {
        Log::LogContext privateContext{"save.display_name",  "private-display", "account.id",      "private-account", "path",
                                       "/private/save/path", "payload",         "private-payload", "secret",          "private-secret"};
        auto submitted = adapter.Submit(71, Request());
        REQUIRE(submitted.HasValue());
        operation.emplace(std::move(submitted).Value());
    }
    jobs.Shutdown(ShutdownPolicy::Drain);
    REQUIRE(operation->Snapshot()->state == SaveOperationState::Completed);
    CHECK_FALSE(provider->logAccepted);
    CHECK_FALSE(provider->metricRegistered);
    CHECK_FALSE(provider->explicitEventAccepted);
    CHECK_FALSE(provider->forwardedEventAccepted);
    REQUIRE(provider->context.operationId == 71);
    REQUIRE(provider->context.diagnosticContext.IsIsolationBoundary());
    REQUIRE(provider->context.diagnosticContext.Fields().size() == 2);
    REQUIRE(provider->context.diagnosticContext.Fields().front().second == "71");
    const auto bundle = session.host->GenerateDiagnosticBundle({.outputPath = session.directory / "bundle.zip"});
    REQUIRE(bundle.HasValue());
    const auto archive = Read(bundle.Value().outputPath);
    REQUIRE(archive.find("save.summary") != std::string::npos);
    REQUIRE(archive.find("retained_logs") != std::string::npos);
    REQUIRE(archive.find("data_categories") != std::string::npos);
    CHECK(archive.find("private-provider-") == std::string::npos);
    CHECK(Read(session.directory / "save.jsonl").find("private-provider-") == std::string::npos);
    CHECK(emergency.ReadOutput().find("private-provider-") == std::string::npos);
    for (const auto secret : {"private-display", "private-account", "/private/save/path", "private-payload", "private-secret"})
        REQUIRE(archive.find(secret) == std::string::npos);
}

TEST_CASE("Save diagnostics across an interrupted host and restart", "[.][save-observability-process]") {
    const auto *mode = std::getenv("HORO_SAVE_OBSERVABILITY_PHASE");
    REQUIRE(mode);
    const std::string_view phase{mode};
    const auto directory = std::filesystem::current_path();
    REQUIRE(std::filesystem::is_regular_file(directory / "save-observability-fixture"));
#if defined(_WIN32)
    if (phase == "fault") {
        SetUnhandledExceptionFilter(+[](EXCEPTION_POINTERS *) -> LONG {
            TerminateProcess(GetCurrentProcess(), 74);
            return EXCEPTION_CONTINUE_SEARCH;
        });
    }
#elif defined(__linux__) || defined(__APPLE__)
    if (phase == "custom") {
        struct sigaction handler{};
        handler.sa_handler = +[](int) { std::_Exit(75); };
        sigemptyset(&handler.sa_mask);
        REQUIRE(sigaction(SIGBUS, &handler, nullptr) == 0);
    }
#endif
    const auto host = StartSaveHost(directory, true, true);
    REQUIRE(host);
    if (phase != "export") {
        REQUIRE(PublishLog("runtime.save.legacy", "private-interrupted-save-value"));
        REQUIRE(PublishLog("runtime.scene", "interrupted-control-log"));
        REQUIRE(Log::Logger::Flush());
#if defined(_WIN32)
        if (phase == "fault") {
            RaiseException(0xE0420145U, EXCEPTION_NONCONTINUABLE, 0, nullptr);
            FAIL("The native fault unexpectedly returned");
        }
#elif defined(__linux__) || defined(__APPLE__)
        if (phase == "fault" || phase == "custom") {
            REQUIRE(raise(SIGBUS) == 0);
            FAIL("The native signal unexpectedly returned");
        }
#endif
        REQUIRE(phase == "interrupt");
        std::_Exit(73);
    }
    REQUIRE(host->GenerateDiagnosticBundle({.outputPath = directory / "bundle.zip"}).HasValue());
}

TEST_CASE("Support bundles keep only summarized Save records across logs, history and crash breadcrumbs",
          "[save][telemetry][privacy][bundle]") {
    Session session;
    REQUIRE(session.host);
    REQUIRE(session.Register());
    REQUIRE(PublishLog("runtime.save.legacy", "private-current-row-payload"));
    REQUIRE(PublishLog("runtime.saver.control", "control-unrelated-log"));
    REQUIRE(Log::Logger::Flush());
    REQUIRE(Read(session.directory / "save.jsonl").find("private-current-row-payload") != std::string::npos);

    const auto write = [](const std::filesystem::path &path, const std::string_view contents) {
        CAPTURE(path);
        std::ofstream output{path, std::ios::binary};
        REQUIRE(output.is_open());
        output << contents;
        output.close();
        REQUIRE(output.good());
    };
    const auto rolledLog = session.directory / "save.jsonl.1";
    write(rolledLog, R"({"category":"runtime.save.stage","message":"private-rolled-row-display",)"
                     R"("fields":{"stage":"capture","outcome":"succeeded","duration_ns":9,"bytes":8,"retries":0,)"
                     R"("dropped_work":0,"queue_depth":1,"note":"private-unknown-field","slot":"private-slot"}})"
                     "\n"
                     R"({"category":"runtime.scene","message":"control-unrelated-rolled-log"})"
                     "\n");
    write(session.directory / "operations/save.jsonl.1",
          R"({"subsystem":"runtime.save","name":"private-history-name","fields":{"note":"private-history-field"}})"
          "\n"
          R"({"subsystem":"runtime.scene","name":"private-history-wrapper-copy",)"
          R"("cause":{"domain":"horo.save","message":"private-history-cause"}})"
          "\n"
          R"({"subsystem":"runtime.scene","name":"control-unrelated-history"})"
          "\n");
    const auto crash = session.directory / "crash.json";
    write(crash, R"({"breadcrumbs":[{"category":"runtime.save.restore","message":"private-crash-breadcrumb"},)"
                 R"({"category":"editor.scene","message":"control-unrelated-crash"}],)"
                 R"("error":{"domain":"horo.scene","code":"scene.restore.failed","message":"private-outer-copy",)"
                 R"("fields":{"display":"private-outer-display"},)"
                 R"("cause":{"domain":"horo.scene","message":"private-inner-copy",)"
                 R"("cause":{"domain":"horo.save","code":"save.restore.failed","message":"private-save-cause",)"
                 R"("cause":{"domain":"horo.platform","code":"platform.storage.failed","message":"private-provider-cause"}}}},)"
                 R"("unrelated_error":{"domain":"horo.scene","message":"control-outer-error",)"
                 R"("cause":{"domain":"horo.platform","message":"control-unrelated-cause"}},)"
                 R"("other":{"domain":"horo.saver","message":"control-domain-prefix"}})");
    const auto bundled = session.host->GenerateDiagnosticBundle(
        {.outputPath = session.directory / "bundle.zip",
         .crashMetadata = Application::HostDiagnosticFile{.sourcePath = crash, .archivePath = "crash/report.json"}});
    REQUIRE(bundled.HasValue());
    const auto archive = Read(bundled.Value().outputPath);
    // Diagnostic bundles use ZIP32 stored entries, so the payload sentinels remain literal archive bytes.
    for (const auto control :
         {"logs/save.1.jsonl", "history/save.1.operations.jsonl", "control-unrelated-log", "control-unrelated-rolled-log",
          "control-unrelated-history", "control-unrelated-crash", "control-outer-error", "control-unrelated-cause", "control-domain-prefix",
          "data_categories", "save.summary", R"("observations\":1)"}) {
        CAPTURE(control);
        CHECK(archive.find(control) != std::string::npos);
    }
    for (const auto secret :
         {"private-current-row-payload", "private-rolled-row-display", "private-unknown-field", "private-slot", "private-history-name",
          "private-history-field", "private-history-wrapper-copy", "private-history-cause", "private-crash-breadcrumb",
          "private-outer-copy", "private-outer-display", "private-inner-copy", "private-save-cause", "private-provider-cause"}) {
        CAPTURE(secret);
        CHECK(archive.find(secret) == std::string::npos);
    }
    CHECK(Read(rolledLog).find("private-rolled-row-display") != std::string::npos);
}

TEST_CASE("Support bundles withhold Save wrappers with causal lists", "[save][telemetry][privacy][bundle][causal-lists]") {
    Session session;
    REQUIRE(session.host);
    REQUIRE(session.Register());
    const auto crash = session.directory / "crash.json";
    {
        std::ofstream output{crash, std::ios::binary};
        output << R"({"errors":[
            {"domain":"horo.mcp","message":"private-list-wrapper",
             "causes":[{"domain":"horo.platform","code":"storage.failed"},{"domain":"horo.save","code":"save.restore.failed"}]},
            {"domain":"horo.scene","message":"private-outer-list-wrapper",
             "cause":{"domain":"horo.mcp","message":"private-inner-list-wrapper",
                      "causes":[{"domain":"horo.save","message":"private-causal-element"}]}},
            {"domain":"horo.scene","message":"retained-sibling",
             "causes":[{"domain":"horo.platform","message":"retained-unrelated-cause"}]}
        ]})";
        REQUIRE(output.good());
    }
    const auto bundled = session.host->GenerateDiagnosticBundle(
        {.outputPath = session.directory / "bundle.zip",
         .crashMetadata = Application::HostDiagnosticFile{.sourcePath = crash, .archivePath = "crash/report.json"}});
    REQUIRE(bundled.HasValue());
    const auto archive = Read(bundled.Value().outputPath);
    for (const auto text : {"private-list-wrapper", "private-outer-list-wrapper", "private-inner-list-wrapper", "private-causal-element"}) {
        CAPTURE(text);
        CHECK(archive.find(text) == std::string::npos);
    }
    CHECK(archive.find("retained-sibling") != std::string::npos);
    CHECK(archive.find("retained-unrelated-cause") != std::string::npos);
}

TEST_CASE("Support bundle rejects ambiguous diagnostic members before classifying Save privacy", "[save][telemetry][privacy][bundle]") {
    Session session;
    REQUIRE(session.host);
    const auto input = session.directory / "duplicate-member";
    const auto output = session.directory / "bundle.zip";
    std::filesystem::path entry = "crash/duplicate-member.json";
    SECTION("JSON crash metadata") {
        // Keep the default JSON archive entry for this case.
    }
    SECTION("Final JSONL record without a newline") {
        entry.replace_extension(".jsonl");
    }
    const auto write = [&input](const std::string_view text) {
        std::ofstream file{input, std::ios::binary};
        REQUIRE(file.is_open());
        file << text;
        file.close();
        REQUIRE(file.good());
    };
    const Application::HostDiagnosticBundleRequest request{.outputPath = output,
                                                           .crashMetadata =
                                                               Application::HostDiagnosticFile{.sourcePath = input, .archivePath = entry}};
    write(R"({"error":{"domain":"horo.save","\u0064omain":"horo.scene","message":"private-save-value"}})");
    CHECK(session.host->GenerateDiagnosticBundle(request).HasError());
    CHECK_FALSE(std::filesystem::exists(output));

    write(R"({"separate":[{"domain":"horo.save","message":"private-save-value"},)"
          R"({"domain":"horo.scene","message":"unrelated-scene-result"}]})");
    const auto accepted = session.host->GenerateDiagnosticBundle(request);
    REQUIRE(accepted.HasValue());
    const auto archive = Read(accepted.Value().outputPath);
    CHECK(archive.find("private-save-value") == std::string::npos);
    CHECK(archive.find("unrelated-scene-result") != std::string::npos);
}

TEST_CASE("Support bundles withhold diagnostic records that embed a private Save error",
          "[save][telemetry][privacy][bundle][embedded-error]") {
    Session session;
    REQUIRE(session.host);
    const auto input = session.directory / "diagnostics";
    std::filesystem::path entry;
    {
        std::ofstream output{input, std::ios::binary};
        SECTION("A log message quotes a Save failure embedded in its fields") {
            entry = "crash/records.jsonl";
            output << R"({"category":"editor.scene","message":"private-message-copy",)"
                      R"("fields":{"domain":"horo.save","display":"private-display"}})"
                   << '\n'
                   << R"({"category":"editor.scene","message":"unrelated-diagnostic"})" << '\n';
        }
        SECTION("A crash aggregate has a diagnostic record with an embedded private cause") {
            entry = "crash/report.json";
            output << R"({"records":[
                {"subsystem":"editor.scene","name":"private-message-copy",
                 "fields":{"detail":{"error":{"domain":"horo.save","message":"private-display"}}}},
                {"domain":"horo.scene","message":"unrelated-diagnostic",
                 "cause":{"domain":"horo.platform","message":"unrelated-cause"}}
            ]})";
        }
        SECTION("A flattened diagnostic identifies its Save namespace only through the error code") {
            entry = "crash/report.json";
            output << R"({"records":[
                {"domain":"horo.scene","message":"private-message-copy",
                 "diagnostics":[{"code":"save.archive.chunk_hash_mismatch","message":"private-display"}]},
                {"code":"saver.archive.changed","message":"unrelated-diagnostic"}
            ]})";
        }
        REQUIRE(output.good());
    }
    const auto bundled = session.host->GenerateDiagnosticBundle(
        {.outputPath = session.directory / "bundle.zip",
         .crashMetadata = Application::HostDiagnosticFile{.sourcePath = input, .archivePath = entry}});
    REQUIRE(bundled.HasValue());
    const auto archive = Read(bundled.Value().outputPath);
    CHECK(archive.find("private-message-copy") == std::string::npos);
    CHECK(archive.find("private-display") == std::string::npos);
    CHECK(archive.find("unrelated-diagnostic") != std::string::npos);
}

TEST_CASE("Support bundles withhold forwarded Save errors from real structured logs", "[save][telemetry][privacy][bundle]") {
    Session session;
    REQUIRE(session.host);
    constexpr std::string_view category = "cinematic.property_binding";
    REQUIRE(PublishLog(category, "private-forwarded-save-domain",
                       {{"error_domain", std::string{SaveDiagnosticErrorDomain}}, {"display", std::string{"private-player-display"}}}));
    REQUIRE(PublishLog(category, "private-forwarded-save-code",
                       {{"error_code", std::string{"save.archive.chunk_hash_mismatch"}}, {"slot", std::uint64_t{56740092811}}}));
    REQUIRE(PublishLog(category, "retained-forwarded-unrelated-error",
                       {{"error_domain", std::string{"horo.saver"}}, {"error_code", std::string{"saver.archive.changed"}}}));
    const auto bundled = session.host->GenerateDiagnosticBundle({.outputPath = session.directory / "bundle.zip"});
    REQUIRE(bundled.HasValue());
    const auto source = Read(session.directory / "save.jsonl");
    const auto archive = Read(bundled.Value().outputPath);
    for (const std::string_view value : {"private-forwarded-save-domain", "private-player-display", "private-forwarded-save-code",
                                        "56740092811"}) {
        REQUIRE(source.find(value) != std::string::npos);
        CHECK(archive.find(value) == std::string::npos);
    }
    CHECK(archive.find("retained-forwarded-unrelated-error") != std::string::npos);
}

TEST_CASE("Support bundles withhold unclassified scalar values alongside private Save evidence", "[save][telemetry][privacy][bundle]") {
    Session session;
    REQUIRE(session.host);
    const auto input = session.directory / "diagnostics";
    auto entry = std::filesystem::path{"crash/reports.json"};
    constexpr std::string_view wrapper =
        R"({"message":"private-wrapper-copy","error":{"domain":"horo.save","message":"private-save-message"}})";
    constexpr std::string_view mixed = R"(["private-array-copy",{"domain":"horo.save","message":"private-save-message"}])";
    constexpr std::string_view numericWrapper =
        R"({"numericSlotId":6915374168053928091,"error":{"domain":"horo.save","message":"private-save-message"}})";
    constexpr std::string_view numericArray = R"([6517473283808069338,{"domain":"horo.save","message":"private-save-message"}])";
    constexpr std::string_view booleanWrapper =
        R"({"private-flag-name":true,"error":{"domain":"horo.save","message":"private-save-message"}})";
    constexpr std::string_view safe = R"({"message":"retained-standalone-report","count":23786038})";
    {
        std::ofstream output{input, std::ios::binary};
        SECTION("Crash archive with unclassified object and array entries") {
            output << '[' << wrapper << ',' << mixed << ',' << numericWrapper << ',' << numericArray << ',' << booleanWrapper << ','
                   << safe << ']';
        }
        SECTION("Crash JSONL with independent unclassified records") {
            entry.replace_extension(".jsonl");
            output << wrapper << '\n' << mixed << '\n' << numericWrapper << '\n' << numericArray << '\n' << booleanWrapper << '\n'
                   << safe << '\n';
        }
        REQUIRE(output.good());
    }
    const auto bundled = session.host->GenerateDiagnosticBundle(
        {.outputPath = session.directory / "bundle.zip",
         .crashMetadata = Application::HostDiagnosticFile{.sourcePath = input, .archivePath = entry}});
    REQUIRE(bundled.HasValue());
    const auto archive = Read(bundled.Value().outputPath);
    CHECK(archive.find("private-wrapper-copy") == std::string::npos);
    CHECK(archive.find("private-array-copy") == std::string::npos);
    CHECK(archive.find("private-save-message") == std::string::npos);
    CHECK(archive.find("6915374168053928091") == std::string::npos);
    CHECK(archive.find("6517473283808069338") == std::string::npos);
    CHECK(archive.find("private-flag-name") == std::string::npos);
    CHECK(archive.find("retained-standalone-report") != std::string::npos);
    CHECK(archive.find("23786038") != std::string::npos);
}

TEST_CASE("Support bundles discard keys used to index withheld Save diagnostics", "[save][telemetry][privacy][bundle]") {
    Session session;
    REQUIRE(session.host);
    const auto input = session.directory / "diagnostics";
    auto entry = std::filesystem::path{"crash/reports.json"};
    SECTION("JSON crash snapshot") {}
    SECTION("JSONL crash snapshot") {
        entry.replace_extension(".jsonl");
    }
    {
        std::ofstream output{input, std::ios::binary};
        output << R"({"records":{)"
               << R"("private-direct-slot-key":{"domain":"horo.save","message":"private-save-detail"},)"
               << R"("private-nested-player-key":{"latest":{"domain":"horo.save","message":"private-save-detail"}},)"
               << R"("private-array-slot-key":[{"domain":"horo.save","message":"private-save-detail"}],)"
               << R"("/home/private-player/hidden-save-slot":{"message":"private-path-indexed-payload"},)"
               << R"("C:\\Users\\private-profile\\pending-save-slot":{"message":"private-windows-indexed-payload"},)"
               << R"("private-mixed-slot-key":[{"domain":"horo.scene","message":"safe-first"},)"
               << R"({"domain":"horo.save","message":"private-save-detail"},)"
               << R"({"domain":"horo.scene","message":"safe-second"}],)"
               << R"("redactedEntries":{"domain":"horo.scene","message":"safe-existing-entry"},)"
               << R"("private-token-group":[{"domain":"horo.scene","message":"private-credential-copy"},)"
               << R"({"domain":"horo.save","message":"private-save-detail"}]}})" << '\n';
        REQUIRE(output.good());
    }
    const auto bundled = session.host->GenerateDiagnosticBundle(
        {.outputPath = session.directory / "bundle.zip",
         .crashMetadata = Application::HostDiagnosticFile{.sourcePath = input, .archivePath = entry}});
    REQUIRE(bundled.HasValue());
    const auto archive = Read(bundled.Value().outputPath);
    for (const std::string_view withheld : {"private-direct-slot-key", "private-nested-player-key", "private-array-slot-key",
                                           "private-mixed-slot-key", "private-token-group", "private-credential-copy",
                                           "private-save-detail"})
        CHECK(archive.find(withheld) == std::string::npos);
    for (const std::string_view withheld : {"private-player", "hidden-save-slot", "private-path-indexed-payload",
                                           "private-profile", "pending-save-slot", "private-windows-indexed-payload"})
        CHECK(archive.find(withheld) == std::string::npos);
    CHECK(archive.find("safe-first") < archive.find("safe-second"));
    CHECK(archive.find("safe-second") != std::string::npos);
    CHECK(archive.find("safe-existing-entry") != std::string::npos);
}

TEST_CASE("Support bundle rejects deeply nested private Save diagnostics before publication", "[save][telemetry][privacy][bundle]") {
    Session session;
    REQUIRE(session.host);
    const auto input = session.directory / "nested";
    const auto output = session.directory / "bundle.zip";
    std::filesystem::path entry = "crash/nested.json";
    SECTION("JSON crash metadata") {
        // Exercise the default JSON entry.
    }
    SECTION("JSONL crash metadata without a trailing newline") {
        entry.replace_extension(".jsonl");
    }
    const auto write = [&input](const std::size_t depth) {
        std::ofstream file{input, std::ios::binary};
        REQUIRE(file.is_open());
        for (std::size_t level = 0; level < depth; ++level)
            file << R"({"cause":)";
        file << R"({"domain":"horo.save","message":"private-deep-save-payload"})" << std::string(depth, '}');
        file.close();
        REQUIRE(file.good());
    };
    write(96);
    const auto rejected = session.host->GenerateDiagnosticBundle(
        {.outputPath = output, .crashMetadata = Application::HostDiagnosticFile{.sourcePath = input, .archivePath = entry}});
    REQUIRE(rejected.HasError());
    CHECK_FALSE(std::filesystem::exists(output));

    write(16);
    const auto accepted = session.host->GenerateDiagnosticBundle(
        {.outputPath = output, .crashMetadata = Application::HostDiagnosticFile{.sourcePath = input, .archivePath = entry}});
    REQUIRE(accepted.HasValue());
    CHECK(Read(output).find("private-deep-save-payload") == std::string::npos);
}

TEST_CASE("Support bundles withhold complete local paths from crash text and record keys", "[save][telemetry][privacy][bundle]") {
    Session session;
    REQUIRE(session.host);
    const auto input = session.directory / "diagnostics";
    auto entry = std::filesystem::path{"crash/paths.json"};
    const bool jsonl = GENERATE(false, true);
    if (jsonl)
        entry.replace_extension(".jsonl");
    {
        std::ofstream output{input, std::ios::binary};
        output << R"({"records":[)"
               << R"json({"message":"posix-diagnostic at (/home/Private Player/private-slot)"},)json"
               << R"({"message":"compact-diagnostic path:/home/Another Player/compact-slot"},)"
               << R"({"message":"windows-diagnostic at:C:\\Users\\Private Profile\\secret-slot"},)"
               << R"({"message":"uri-diagnostic at FiLe:///home/Other%20Player/local-slot"},)"
               << R"({"source:/home/private-compact-key":{"message":"private-compact-indexed-row"}},)"
               << R"({"file:///home/private-uri-key":{"message":"private-indexed-row"}},)"
               << R"({"message":"unrelated-diagnostic at https://example.invalid/diagnostics"}]})" << '\n';
        REQUIRE(output.good());
    }
    const auto bundled = session.host->GenerateDiagnosticBundle(
        {.outputPath = session.directory / "bundle.zip",
         .crashMetadata = Application::HostDiagnosticFile{.sourcePath = input, .archivePath = entry}});
    REQUIRE(bundled.HasValue());
    const auto archive = Read(bundled.Value().outputPath);
    for (const std::string_view withheld : {"Private Player", "Player/private-slot", "Another Player", "compact-slot", "Profile",
                                           "secret-slot", "Other%20Player", "local-slot", "private-uri-key", "private-indexed-row",
                                           "private-compact-key", "private-compact-indexed-row"})
        CHECK(archive.find(withheld) == std::string::npos);
    for (const std::string_view retained : {"posix-diagnostic", "compact-diagnostic", "windows-diagnostic", "uri-diagnostic",
                                           "https://example.invalid/diagnostics", "[REDACTED_PATH]"})
        CHECK(archive.find(retained) != std::string::npos);
}

TEST_CASE("Support bundle manifest rejects metadata indexed by a local path", "[save][telemetry][privacy][bundle]") {
    Session session;
    REQUIRE(session.host);
    const auto output = session.directory / "bundle.zip";
    const auto *key = GENERATE("file:///home/private-save-slot", "source:/home/private-save-slot");
    const auto result = session.host->GenerateDiagnosticBundle({.outputPath = output, .metadata = {{key, "failed"}}});
    REQUIRE(result.HasError());
    CHECK_FALSE(std::filesystem::exists(output));
}

TEST_CASE("Save telemetry preserves production failure results and discards private error text", "[save][telemetry][failure]") {
    Session session;
    REQUIRE(session.host);
    REQUIRE(session.Register());
    auto provider = std::make_shared<Provider>();
    provider->fail = true;
    JobSystem jobs{JobSystemConfig{.workerCount = 1, .maxQueuedJobs = 4}};
    SaveStorageAdapter adapter{jobs, provider};
    EmergencyCapture emergency{session.directory / "emergency.txt"};
    const auto emergencyBefore = Log::Logger::Statistics().emergencyRecords;
    auto submitted = adapter.Submit(72, Request());
    REQUIRE(submitted.HasValue());
    jobs.Shutdown(ShutdownPolicy::Drain);
    const auto snapshot = submitted.Value().Snapshot();
    REQUIRE(snapshot->state == SaveOperationState::Failed);
    REQUIRE(snapshot->terminalError->code.Value() == SaveErrors::StoragePermanentIo.code.Value());
    REQUIRE(SaveMigrationExecutor::Migrate({}, {}, {}).HasError());
    REQUIRE(Log::Logger::Flush());
    const auto log = Read(session.directory / "save.jsonl");
    REQUIRE(log.find("private-provider-account") == std::string::npos);
    const auto output = emergency.ReadOutput();
    REQUIRE(output.find("private-provider-account") == std::string::npos);
    const bool retainedFailure = log.find("failed") != std::string::npos && log.find("failure_category") != std::string::npos &&
                                 log.find("migrate") != std::string::npos;
    const bool emergencyFailure = Log::Logger::Statistics().emergencyRecords > emergencyBefore &&
                                  output.find("Save stage migrate failed operation=0") != std::string::npos;
    REQUIRE((retainedFailure || emergencyFailure));
}

TEST_CASE("Save telemetry cancelled interrupted and sync contract observations use closed safe evidence", "[save][telemetry][outcomes]") {
    Session session;
    REQUIRE(session.host);
    REQUIRE(session.Register());
    const auto path = session.directory / "save.jsonl";
    const auto retain = [&path](const auto &emit, const std::string_view marker) {
        // A producer can lose the bounded logging queue during dispatch. Verify the
        // exact retained record before asserting over aggregated counts below.
        for (std::size_t attempt = 0; attempt < 32; ++attempt) {
            emit();
            REQUIRE(Log::Logger::Flush());
            if (Read(path).find(marker) != std::string::npos)
                return;
            std::this_thread::yield();
        }
        FAIL("The fixture did not retain the expected diagnostic record");
    };
    retain([] {
        SaveStageObservation cancelled{SaveTelemetryStage::Capture, 81};
        cancelled.Fail(MakeError(SaveErrors::OperationCancelled, "private-cancellation"));
        cancelled.Complete(SaveTelemetryOutcome::Succeeded);
    }, R"("outcome":"cancelled")");
    retain([] {
        SaveStageObservation interrupted{SaveTelemetryStage::Participant, 81};
    }, R"("outcome":"interrupted")");
    retain([] {
        SaveStageObservation sync{SaveTelemetryStage::Sync, 81};
        sync.Complete(SaveTelemetryOutcome::Failed, {.bytes = 64, .retries = 2});
    }, R"("stage":"sync")");
    const std::array paths{path};
    const auto summary = SummarizeSaveTelemetry(paths);
    REQUIRE(summary.HasValue());
    REQUIRE(summary.Value().second.find("cancelled") != std::string::npos);
    REQUIRE(summary.Value().second.find("interrupted") != std::string::npos);
    REQUIRE(summary.Value().second.find("private-cancellation") == std::string::npos);
    CHECK(summary.Value().second.find("\"cancellation\":1") != std::string::npos);
    CHECK(summary.Value().second.find("\"unclassified\":1") != std::string::npos);
}

TEST_CASE("Save retained failure categories accept only canonical numeric evidence", "[save][telemetry][summary]") {
    Session session;
    REQUIRE(session.host);
    const auto log = session.directory / "failure-categories.jsonl";
    {
        std::ofstream rows{log};
        const auto append = [&](std::string_view category) {
            rows
                << R"({"category":"runtime.save.stage","fields":{"stage":"commit","outcome":"failed","duration_ns":1,"bytes":0,"retries":0,"dropped_work":0,"queue_depth":0)"
                << category << "}}\n";
        };
        const auto storage = static_cast<std::uint64_t>(SaveFailureCategory::Storage);
        append(",\"failure_category\":" + std::to_string(storage));
        append(",\"failure_category\":4294967296");
        append(R"(,"failure_category":"untrusted-account-tag")");
        append("");
        REQUIRE(rows.good());
    }
    const std::array inputs{log};
    const auto summary = SummarizeSaveTelemetry(inputs);
    REQUIRE(summary.HasValue());
    const auto &content = summary.Value().second;
    CHECK(content.find("\"storage\":1") != std::string::npos);
    CHECK(content.find("\"unclassified\":3") != std::string::npos);
    CHECK(content.find("\"failed\":4") != std::string::npos);
    CHECK(content.find("untrusted-account-tag") == std::string::npos);
}

TEST_CASE("Save retained summary bounds malformed oversized and missing inputs without exporting source text",
          "[save][telemetry][summary]") {
    Session session;
    REQUIRE(session.host);
    const auto malformed = session.directory / "private-account.jsonl";
    std::ofstream{malformed} << "private-payload\n" << std::string(5000, 'x') << '\n';
    const std::array paths{malformed, session.directory / "missing-private-path.jsonl"};
    const auto summary = SummarizeSaveTelemetry(paths);
    REQUIRE(summary.HasValue());
    REQUIRE(summary.Value().second.find("private") == std::string::npos);
    REQUIRE(summary.Value().second.find("\"truncated\":true") != std::string::npos);
    REQUIRE(summary.Value().second.find("\"invalid_rows\":1") != std::string::npos);
    REQUIRE(summary.Value().second.find("\"missing_sources\":1") != std::string::npos);
}

TEST_CASE("Unregistered or disabled Save observations do not bind context or change results", "[save][telemetry][disabled]") {
    Log::Logger::Shutdown();
    Log::LogContext ambient{"ordinary", "preserved"};
    auto registration = SaveTelemetryRegistration::Create();
    REQUIRE(registration.HasValue());
    SaveStageObservation observation{SaveTelemetryStage::Capture, 99};
    REQUIRE_FALSE(observation.IsActive());
    REQUIRE_FALSE(Log::CaptureLogContext().IsIsolationBoundary());
    const auto result = ObserveSaveStage(SaveTelemetryStage::Encode, 99, [] {
        return Result<void>::Success();
    });
    REQUIRE(result.HasValue());
}

TEST_CASE("Save safe lineage survives retained platform request completion under private ambient context",
          "[save][telemetry][privacy][platform-request]") {
    using namespace Horo::PlatformServices;
    Session session;
    REQUIRE(session.host);
    REQUIRE(session.Register());
    PlatformRequestStore requests;
    std::optional<PlatformRequestHandle<int>> handle;
    {
        Telemetry::ScopedOperationContext parent{Telemetry::OperationContext{.operationId = 40}};
        Log::LogContext privateContext{"account.id", "private-account", "display.name", "private-display"};
        SaveStageObservation sync{SaveTelemetryStage::Sync, 41};
        auto admitted = requests.Admit<int>();
        REQUIRE(admitted.HasValue());
        handle.emplace(std::move(admitted).Value());
        sync.Complete(SaveTelemetryOutcome::Succeeded);
    }
    Log::LogContext unrelated{"payload", "private-later-payload"};
    Log::LogContextSnapshot observed;
    auto subscription = requests.OnComplete<int>(*handle, [&observed](const PlatformRequestSnapshot<int> &) {
        observed = Log::CaptureLogContext();
    });
    REQUIRE(subscription.HasValue());
    REQUIRE(requests.MarkRunning(*handle).HasValue());
    REQUIRE(requests.CompleteSuccess(*handle, 1).HasValue());
    REQUIRE(requests.DispatchCompletions() == 1);
    REQUIRE(observed.IsIsolationBoundary());
    REQUIRE(observed.Fields().size() == 2);
    REQUIRE((observed.Fields()[0] == Log::MdcField{"save.operation", "41"}));
    REQUIRE((observed.Fields()[1] == Log::MdcField{"save.parent_operation", "40"}));
    REQUIRE(Log::CaptureLogContext().Fields().size() == 1);
}

TEST_CASE("Save shutdown abandonment and completion observers retain one safe winning terminal record",
          "[save][telemetry][terminal][shutdown]") {
    Session session;
    REQUIRE(session.host);
    REQUIRE(session.Register());
    Log::LogContext privateContext{"account.id", "private-shutdown-account"};
    Log::LogContextSnapshot callbackContext;
    std::size_t callbacks{};
    const auto dropsBefore = Telemetry::Runtime::GetStatistics().droppedRecords;
    {
        auto created = CreateSaveOperation({.operation = 91, .kind = SaveOperationKind::Save, .maximumCompletionCallbacks = 4});
        REQUIRE(created.HasValue());
        auto controller = std::move(created).Value();
        REQUIRE(controller.Handle()
                    .OnCompletion([&](const SaveOperationSnapshot &) {
            ++callbacks;
            callbackContext = Log::CaptureLogContext();
            Log::Logger::Write("runtime.save.callback", Log::Level::Info, "Safe callback");
        }).HasValue());
        REQUIRE(controller.RequestShutdownCancellation() == SaveCancellationRequestResult::Requested);
        static_cast<void>(controller.ObserveCancellation());
        static_cast<void>(controller.ObserveCancellation());
        REQUIRE(controller.Handle().Snapshot()->state == SaveOperationState::Cancelled);
    }
    SaveOperationHandle abandonedHandle;
    {
        auto abandoned = CreateSaveOperation({.operation = 92, .kind = SaveOperationKind::Save, .maximumCompletionCallbacks = 4});
        REQUIRE(abandoned.HasValue());
        abandonedHandle = abandoned.Value().Handle();
        REQUIRE(abandonedHandle
                    .OnCompletion([&](const SaveOperationSnapshot &) {
            ++callbacks;
        }).HasValue());
    }
    REQUIRE(callbacks == 2);
    REQUIRE(abandonedHandle.Snapshot()->state == SaveOperationState::Failed);
    REQUIRE(abandonedHandle.Snapshot()->terminalError->code.Value() == SaveErrors::OperationAbandoned.code.Value());
    REQUIRE(callbackContext.IsIsolationBoundary());
    REQUIRE(callbackContext.Fields().front().second == "91");
    REQUIRE(Log::Logger::Flush());
    const auto log = Read(session.directory / "save.jsonl");
    REQUIRE(log.find("private-shutdown-account") == std::string::npos);
    const auto count = CountTerminalRecords(log);
    REQUIRE(count <= 2);
    REQUIRE(count + Telemetry::Runtime::GetStatistics().droppedRecords - dropsBefore >= 2);
}

TEST_CASE("Host policy may disable the Save subsystem without rejecting startup or binding private context",
          "[save][telemetry][policy][disabled]") {
    Session session{false};
    REQUIRE(session.host);
    REQUIRE(session.Register());
    Log::LogContext ambient{"payload.özel", std::string(4096, 'a')};
    SaveStageObservation observation{SaveTelemetryStage::Capture, 99};
    REQUIRE_FALSE(observation.IsActive());
    REQUIRE_FALSE(Log::CaptureLogContext().IsIsolationBoundary());
    observation.Fail(MakeError(SaveErrors::OperationCancelled));
    REQUIRE(Log::Logger::Flush());
    REQUIRE(Read(session.directory / "save.jsonl").find("runtime.save") == std::string::npos);
}

TEST_CASE("Save failed stages retain safe emergency evidence when the bounded normal queue is full", "[save][telemetry][emergency]") {
    Session session;
    REQUIRE(session.host);
    auto sink = std::make_shared<BlockingSink>();
    REQUIRE(Telemetry::Runtime::Initialize({.queueCapacity = 1, .metricCollectionLevel = Telemetry::MetricCollectionLevel::Off}, sink));
    REQUIRE(session.Register());
    REQUIRE(sink->Start("test.queue.block"));
    REQUIRE(Telemetry::Runtime::EmitEvent("runtime.save", "test.queue.fill", Log::Level::Info, "Safe queue fixture"));
    EmergencyCapture capture{session.directory / "emergency.txt"};
    const auto before = Log::Logger::Statistics().emergencyRecords;
    {
        Log::LogContext privateContext{"account.id", "private-account", "path", "/private/path"};
        SaveStageObservation failed{SaveTelemetryStage::Sync, 83};
        REQUIRE(failed.IsActive());
        failed.Fail(MakeError(SaveErrors::StoragePermanentIo, "private-provider-account"));
    }
    sink->Release();
    REQUIRE(Log::Logger::Statistics().emergencyRecords == before + 1);
    const auto output = capture.ReadOutput();
    REQUIRE(output.find("Save stage sync failed operation=83") != std::string::npos);
    REQUIRE(output.find("private-account") == std::string::npos);
    REQUIRE(output.find("/private/path") == std::string::npos);
    REQUIRE(output.find("private-provider-account") == std::string::npos);
}

namespace {
    /** @brief Throws foreign work failure to verify interrupted observations and caller exception preservation. */
    [[noreturn]] Result<void> ThrowSaveWorkFailure() {
        throw FixtureFailure("private-unwind-error");
    }
}  // namespace

TEST_CASE("Save stage failures preserve caller evidence and restore context after unwinding", "[save][telemetry][failure][privacy]") {
    Session session;
    REQUIRE(session.host);
    REQUIRE(session.Register());
    Log::LogContext ambient{"account.id", "private-unwind-account"};
    const SaveTelemetryEvidence evidence{.bytes = 64, .retries = 2};
    {
        SaveStageObservation observation{SaveTelemetryStage::Capture, 101};
        REQUIRE(observation.IsActive());
        observation.Fail(MakeError(SaveErrors::OperationCancelled, "private-cancel-text"), evidence);
        REQUIRE(evidence.failureCategory == SaveFailureCategory::Count);
        REQUIRE(evidence.bytes == 64);
        REQUIRE(evidence.retries == 2);
    }
    REQUIRE_FALSE(Log::CaptureLogContext().IsIsolationBoundary());
    REQUIRE_THROWS_AS(ObserveSaveStage(SaveTelemetryStage::Participant, 102, &ThrowSaveWorkFailure), FixtureFailure);
    REQUIRE_FALSE(Log::CaptureLogContext().IsIsolationBoundary());
    REQUIRE((Log::CaptureLogContext().Fields().front() == Log::MdcField{"account.id", "private-unwind-account"}));
    REQUIRE(Log::Logger::Flush());
    const auto log = Read(session.directory / "save.jsonl");
    REQUIRE(log.find("private-unwind") == std::string::npos);
    REQUIRE(log.find("private-cancel-text") == std::string::npos);
}

TEST_CASE("Save observation allocation failures report bounded safe evidence", "[save][telemetry][allocation][emergency]") {
    Session session;
    REQUIRE(session.host);
    auto sink = std::make_shared<BlockingSink>();
    REQUIRE(Telemetry::Runtime::Initialize({.queueCapacity = 4, .metricCollectionLevel = Telemetry::MetricCollectionLevel::Off}, sink));
    REQUIRE(session.Register());
    REQUIRE(sink->Start("test.block"));
    EmergencyCapture capture{session.directory / "allocation-emergency.txt"};
    Log::LogContext ambient{"account.id", "private-allocation-account"};
    const auto before = Log::Logger::Statistics().emergencyRecords;
    bool active{};
    {
        Tests::AllocationProbe::ScopedFailure failure;
        SaveStageObservation observation{SaveTelemetryStage::Capture, 103};
        active = observation.IsActive();
    }
    SaveStageObservation completion{SaveTelemetryStage::Encode, 103};
    {
        Tests::AllocationProbe::ScopedFailure failure;
        completion.Complete(SaveTelemetryOutcome::Succeeded);
    }
    sink->Release();
    REQUIRE_FALSE(active);
    REQUIRE(Log::Logger::Statistics().emergencyRecords == before + 2);
    const auto output = capture.ReadOutput();
    REQUIRE(output.find("Save telemetry observation could not be recorded") != std::string::npos);
    REQUIRE(output.find("private-allocation-account") == std::string::npos);
}
