#include "Horo/Application/HostObservability.h"
#include "Horo/PlatformServices/PlatformRequest.h"
#include "Horo/Runtime/Save/SaveErrors.h"
#include "Horo/Runtime/Save/SaveMigration.h"
#include "Horo/Runtime/Save/SaveOperation.h"
#include "Horo/Runtime/Save/SaveStorageAdapter.h"
#include "Horo/Runtime/Save/SaveTelemetry.h"
#include "SaveTestUtils.h"
#include "support/AllocationProbe.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <format>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <type_traits>

#if defined(_WIN32)
#include <io.h>
#else
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

    class Session final {
    public:
        explicit Session(const bool includeSave = true) {
            host = Application::HostObservabilitySession::Start(
                {.logging = {.logDirectory = directory,
                             .baseName = "save",
                             .queueCapacity = 4096,
                             .subsystemPrefixes = includeSave ? std::vector<std::string>{} : std::vector<std::string>{"other"},
                             .metricCollectionLevel =
                                 includeSave ? Telemetry::MetricCollectionLevel::Core : Telemetry::MetricCollectionLevel::Off,
                             .echoToStderr = false},
                 .summaries = {&SummarizeSaveTelemetry}});
        }

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
    class BlockingSink final : public Telemetry::ISink {
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
            // The producer fixture proves admission directly; concurrent Save stages may also update Logger counters.
            for (std::size_t attempt = 0; attempt < 1000 && !logAccepted; ++attempt)
                logAccepted = Telemetry::Runtime::EmitRecord({.subsystem = "runtime.save.provider",
                                                              .context = context.diagnosticContext,
                                                              .payload = Telemetry::LogRecord{.severity = Log::Level::Info,
                                                                                              .category = "runtime.save.provider",
                                                                                              .message = "Safe provider request"}});
            return fail ? Result<SaveStorageValue>::Failure(MakeError(SaveErrors::StoragePermanentIo, "private-provider-account"))
                        : Result<SaveStorageValue>::Success(true);
        }

        bool fail{};
        bool logAccepted{};
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

TEST_CASE("Save telemetry production storage jobs isolate private ambient context and export retained summaries",
          "[save][telemetry][privacy][bundle]") {
    Session session;
    REQUIRE(session.host);
    REQUIRE(session.Register());
    REQUIRE(SaveTelemetryRegistration::Create().HasError());
    auto provider = std::make_shared<Provider>();
    JobSystem jobs{JobSystemConfig{.workerCount = 1, .maxQueuedJobs = 4}};
    SaveStorageAdapter adapter{jobs, provider};
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
    REQUIRE(provider->logAccepted);
    REQUIRE(provider->context.operationId == 71);
    REQUIRE(provider->context.diagnosticContext.IsIsolationBoundary());
    REQUIRE(provider->context.diagnosticContext.Fields().size() == 2);
    REQUIRE(provider->context.diagnosticContext.Fields().front().second == "71");
    const auto bundle = session.host->GenerateDiagnosticBundle({.outputPath = session.directory / "bundle.zip"});
    REQUIRE(bundle.HasValue());
    const auto archive = Read(bundle.Value().outputPath);
    REQUIRE(archive.find("save.summary") != std::string::npos);
    REQUIRE(archive.find("retained_logs") != std::string::npos);
    REQUIRE(archive.find("Safe provider request") != std::string::npos);
    for (const auto secret : {"private-display", "private-account", "/private/save/path", "private-payload", "private-secret"})
        REQUIRE(archive.find(secret) == std::string::npos);
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
    {
        SaveStageObservation cancelled{SaveTelemetryStage::Capture, 81};
        cancelled.Fail(MakeError(SaveErrors::OperationCancelled, "private-cancellation"));
        cancelled.Complete(SaveTelemetryOutcome::Succeeded);
        SaveStageObservation interrupted{SaveTelemetryStage::Participant, 81};
        SaveStageObservation sync{SaveTelemetryStage::Sync, 81};
        sync.Complete(SaveTelemetryOutcome::Failed, {.bytes = 64, .retries = 2});
    }
    REQUIRE(Log::Logger::Flush());
    const std::array paths{session.directory / "save.jsonl"};
    const auto summary = SummarizeSaveTelemetry(paths);
    REQUIRE(summary.HasValue());
    REQUIRE(summary.Value().second.find("cancelled") != std::string::npos);
    REQUIRE(summary.Value().second.find("interrupted") != std::string::npos);
    REQUIRE(summary.Value().second.find("private-cancellation") == std::string::npos);
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
