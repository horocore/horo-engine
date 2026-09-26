#include "Horo/Foundation/Diagnostics/DiagnosticBundle.h"
#include "Horo/Foundation/Diagnostics/OperationHistory.h"
#include "Horo/Foundation/JobSystem.h"
#include "Horo/Foundation/Logging/LogContext.h"
#include "Horo/Foundation/Logging/Logger.h"
#include "Horo/Foundation/Logging/StructuredLogStore.h"
#include "Horo/Foundation/OperationStore.h"
#include "Horo/Foundation/Telemetry/Operation.h"
#include "Horo/Foundation/Telemetry/Telemetry.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <memory>
#include <miniz.h>
#include <mutex>
#include <ranges>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
    class TemporaryDirectory final {
    public:
        TemporaryDirectory() {
            static std::atomic<std::uint64_t> sequence{};
            path = std::filesystem::temp_directory_path() /
                   ("horo-observability-tests-" + std::to_string(sequence.fetch_add(1, std::memory_order_relaxed)) + "-" +
                    std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
            std::filesystem::create_directories(path);
        }

        ~TemporaryDirectory() {
            std::error_code error;
            std::filesystem::remove_all(path, error);
        }

        std::filesystem::path path;
    };

    class LoggerGuard final {
    public:
        LoggerGuard() {
            Horo::Log::Logger::Shutdown();
        }

        ~LoggerGuard() {
            Horo::Log::Logger::Shutdown();
        }
    };

    [[nodiscard]] std::string ReadText(const std::filesystem::path &path) {
        std::ifstream stream(path, std::ios::binary);
        return {std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{}};
    }

    class CollectingSink final : public Horo::Telemetry::ISink {
    public:
        void Export(const Horo::Telemetry::Record &record, const Horo::Telemetry::InstrumentDescriptor *descriptor) override {
            std::lock_guard lock(mutex);
            records.push_back(record);
            descriptors.push_back(descriptor == nullptr ? Horo::Telemetry::InstrumentDescriptor{} : *descriptor);
        }

        void Flush() override {
            std::lock_guard lock(mutex);
            flushed = true;
        }

        std::mutex mutex;
        std::vector<Horo::Telemetry::Record> records;
        std::vector<Horo::Telemetry::InstrumentDescriptor> descriptors;
        bool flushed{};
    };

    class BlockingSink final : public Horo::Telemetry::ISink {
    public:
        void Export(const Horo::Telemetry::Record &, const Horo::Telemetry::InstrumentDescriptor *) override {
            std::unique_lock lock(mutex);
            entered = true;
            condition.notify_all();
            condition.wait(lock, [this] {
                return released;
            });
        }

        void Flush() override {
            std::lock_guard lock(mutex);
            flushed = true;
            condition.notify_all();
        }

        void WaitUntilEntered() {
            std::unique_lock lock(mutex);
            condition.wait(lock, [this] {
                return entered;
            });
        }

        void Release() {
            std::lock_guard lock(mutex);
            released = true;
            condition.notify_all();
        }

        [[nodiscard]] bool WaitUntilFlushed(const std::chrono::milliseconds timeout) {
            std::unique_lock lock(mutex);
            return condition.wait_for(lock, timeout, [this] {
                return flushed;
            });
        }

        std::mutex mutex;
        std::condition_variable condition;
        bool entered{};
        bool released{};
        bool flushed{};
    };

    class ThrowingSink final : public Horo::Telemetry::ISink {
    public:
        void Export(const Horo::Telemetry::Record &, const Horo::Telemetry::InstrumentDescriptor *) override {
            throw std::runtime_error{"export failed"};
        }

        void Flush() override {
            throw std::runtime_error{"flush failed"};
        }
    };

    template <typename Recorder> [[nodiscard]] bool RecordTelemetryEventually(Recorder &&recorder) {
        const std::uint64_t acceptedBefore = Horo::Telemetry::Runtime::GetStatistics().acceptedRecords;
        for (int attempt = 0; attempt < 1000 && Horo::Telemetry::Runtime::GetStatistics().acceptedRecords == acceptedBefore; ++attempt)
            recorder();
        return Horo::Telemetry::Runtime::GetStatistics().acceptedRecords > acceptedBefore;
    }
}  // namespace

TEST_CASE("Asynchronous logger writes structured severity category timestamp and forwarded context",
          "[foundation][observability][logging]") {
    LoggerGuard loggerGuard;
    TemporaryDirectory temporary;
    Horo::Log::Logger::SetLevel(Horo::Log::Level::Trace);
    REQUIRE(Horo::Log::Logger::Init(Horo::Log::LoggerConfiguration{.logDirectory = temporary.path,
                                                                   .baseName = "structured",
                                                                   .queueCapacity = 64,
                                                                   .maxFileBytes = 1024U * 1024U,
                                                                   .maxRolledFiles = 2,
                                                                   .echoToStderr = false}));

    Horo::Log::LogContextSnapshot forwarded;
    {
        const Horo::Log::LogContext request("correlation.id", "request-42", "asset.id", "mesh-7");
        forwarded = Horo::Log::CaptureLogContext();
    }
    std::thread worker([forwarded] {
        const Horo::Log::ScopedLogContext context{forwarded};
        const std::array fields{
            Horo::Telemetry::Field{.key = "retry", .value = std::uint64_t{2}},
            Horo::Telemetry::Field{.key = "cached", .value = false},
            Horo::Telemetry::Field{.key = "duration_ms", .value = 12.5},
            Horo::Telemetry::Field{.key = "detail", .value = std::string{"quote=\" newline=\n"}},
            Horo::Telemetry::Field{.key = "auth.token", .value = std::string{"must-not-persist"}},
            Horo::Telemetry::Field{.key = "internal.pointer",
                                   .value = std::string{"forbidden"},
                                   .privacy = Horo::Telemetry::FieldPrivacy::Forbidden},
        };
        const auto acceptedBefore = Horo::Log::Logger::Statistics().acceptedRecords;
        for (int attempt = 0; attempt < 1000 && Horo::Log::Logger::Statistics().acceptedRecords == acceptedBefore; ++attempt) {
            Horo::Log::Logger::Write("assets.import", Horo::Log::Level::Error, "import failed", fields);
        }
    });
    worker.join();
    REQUIRE(Horo::Log::Logger::Flush());
    Horo::Log::Logger::Shutdown();

    const std::string log = ReadText(temporary.path / "structured.jsonl");
    REQUIRE(log.find(R"("timestamp":")") != std::string::npos);
    REQUIRE(log.find(R"("level":"error")") != std::string::npos);
    REQUIRE(log.find(R"("subsystem":"assets.import")") != std::string::npos);
    REQUIRE(log.find(R"("category":"assets.import")") != std::string::npos);
    REQUIRE(log.find(R"("thread":{"id":)") != std::string::npos);
    REQUIRE(log.find(R"("correlation.id":"request-42")") != std::string::npos);
    REQUIRE(log.find(R"("asset.id":"mesh-7")") != std::string::npos);
    REQUIRE(log.find(R"("retry":2)") != std::string::npos);
    REQUIRE(log.find(R"("cached":false)") != std::string::npos);
    REQUIRE(log.find(R"("duration_ms":12.5)") != std::string::npos);
    REQUIRE(log.find("\"detail\":\"quote=\\\" newline=\\n\"") != std::string::npos);
    REQUIRE(log.find(R"("auth.token":"[REDACTED]")") != std::string::npos);
    REQUIRE(log.find("must-not-persist") == std::string::npos);
    REQUIRE(log.find("internal.pointer") == std::string::npos);
}

TEST_CASE("Host observability session markers distinguish clean and interrupted shutdown", "[foundation][observability][host][lifecycle]") {
    LoggerGuard loggerGuard;
    TemporaryDirectory temporary;
    const Horo::Log::LoggerConfiguration configuration{.logDirectory = temporary.path,
                                                       .baseName = "host",
                                                       .hostName = "HoroTestHost",
                                                       .hostVersion = "1.2.3",
                                                       .echoToStderr = false};
    REQUIRE(Horo::Log::Logger::Init(configuration));
    const std::filesystem::path sessionPath = temporary.path / "host.session.json";
    const std::filesystem::path shutdownPath = temporary.path / "host.shutdown.json";
    REQUIRE(std::filesystem::is_regular_file(sessionPath));
    REQUIRE_FALSE(std::filesystem::exists(shutdownPath));
    const std::string firstSession = ReadText(sessionPath);
    REQUIRE(firstSession.find(R"("host":"HoroTestHost")") != std::string::npos);
    REQUIRE(firstSession.find(R"("version":"1.2.3")") != std::string::npos);
    REQUIRE(firstSession.find(R"("previousCleanShutdown":false)") != std::string::npos);

    Horo::Log::Logger::Shutdown();
    REQUIRE(std::filesystem::is_regular_file(shutdownPath));
    REQUIRE(Horo::Log::Logger::Init(configuration));
    REQUIRE(ReadText(sessionPath).find(R"("previousCleanShutdown":true)") != std::string::npos);
    Horo::Log::Logger::Shutdown();

    std::filesystem::remove(shutdownPath);
    REQUIRE(Horo::Log::Logger::Init(configuration));
    REQUIRE(ReadText(sessionPath).find(R"("previousCleanShutdown":false)") != std::string::npos);
}

TEST_CASE("Concurrent logger ingestion drains every accepted record during shutdown", "[foundation][observability][logging][concurrency]") {
    LoggerGuard loggerGuard;
    TemporaryDirectory temporary;
    const Horo::Log::LoggerStatistics before = Horo::Log::Logger::Statistics();
    REQUIRE(Horo::Log::Logger::Init(Horo::Log::LoggerConfiguration{.logDirectory = temporary.path,
                                                                   .baseName = "concurrent",
                                                                   .queueCapacity = 2048,
                                                                   .maxFileBytes = 4U * 1024U * 1024U,
                                                                   .maxRolledFiles = 1,
                                                                   .echoToStderr = false}));
    std::vector<std::thread> producers;
    for (int producer = 0; producer < 4; ++producer) {
        producers.emplace_back([producer] {
            for (int record = 0; record < 250; ++record)
                Horo::Log::Logger::Write("test.concurrent", Horo::Log::Level::Info,
                                         Horo::Log::FormatArgs("producer=%d record=%d", producer, record));
        });
    }
    for (std::thread &producer : producers)
        producer.join();
    Horo::Log::Logger::Shutdown();

    const Horo::Log::LoggerStatistics after = Horo::Log::Logger::Statistics();
    REQUIRE(after.acceptedRecords > before.acceptedRecords);
    REQUIRE((after.writtenRecords - before.writtenRecords == after.acceptedRecords - before.acceptedRecords));
    REQUIRE(std::filesystem::file_size(temporary.path / "concurrent.jsonl") > 0);
}

TEST_CASE("Disabled logging does not evaluate formatting arguments", "[foundation][observability][logging][fast-path]") {
    LoggerGuard loggerGuard;
    const Horo::Log::Level previous = Horo::Log::Logger::GetLevel();
    Horo::Log::Logger::SetLevel(Horo::Log::Level::Off);
    bool evaluated = false;
    const auto expensiveArgument = [&evaluated] {
        evaluated = true;
        return std::string{"allocated"};
    };
    LOG_ERROR("test.disabled", "%s", expensiveArgument().c_str());
    REQUIRE_FALSE(evaluated);
    Horo::Log::Logger::SetLevel(previous);
}

TEST_CASE("Logger rolls bounded files and retains the configured number of segments", "[foundation][observability][logging][rolling]") {
    LoggerGuard loggerGuard;
    TemporaryDirectory temporary;
    REQUIRE(Horo::Log::Logger::Init(Horo::Log::LoggerConfiguration{.logDirectory = temporary.path,
                                                                   .baseName = "rolling",
                                                                   .queueCapacity = 128,
                                                                   .maxFileBytes = 512,
                                                                   .maxRolledFiles = 2,
                                                                   .echoToStderr = false}));
    const std::uint64_t acceptedBefore = Horo::Log::Logger::Statistics().acceptedRecords;
    for (int attempt = 0; attempt < 10000 && Horo::Log::Logger::Statistics().acceptedRecords < acceptedBefore + 24U; ++attempt)
        Horo::Log::Logger::Write("test.rolling", Horo::Log::Level::Info,
                                 Horo::Log::FormatArgs("record-%d-abcdefghijklmnopqrstuvwxyz", attempt));
    REQUIRE(Horo::Log::Logger::Statistics().acceptedRecords >= acceptedBefore + 24U);
    Horo::Log::Logger::Shutdown();
    REQUIRE(std::filesystem::exists(temporary.path / "rolling.jsonl"));
    REQUIRE(std::filesystem::exists(temporary.path / "rolling.jsonl.1"));
    REQUIRE(std::filesystem::exists(temporary.path / "rolling.jsonl.2"));
    REQUIRE_FALSE(std::filesystem::exists(temporary.path / "rolling.jsonl.3"));
    REQUIRE(std::filesystem::file_size(temporary.path / "rolling.jsonl") <= 512);
    REQUIRE(std::filesystem::file_size(temporary.path / "rolling.jsonl.1") <= 512);
    REQUIRE(std::filesystem::file_size(temporary.path / "rolling.jsonl.2") <= 512);
}

TEST_CASE("Logger removes an interrupted trailing JSONL record before appending", "[foundation][observability][logging][recovery]") {
    LoggerGuard loggerGuard;
    TemporaryDirectory temporary;
    const std::filesystem::path path = temporary.path / "recovery.jsonl";
    std::ofstream(path, std::ios::binary) << "{\"schemaVersion\":1,\"message\":\"complete\"}\n{\"schemaVersion\":1,\"message\":\"partial";

    REQUIRE(Horo::Log::Logger::Init(Horo::Log::LoggerConfiguration{.logDirectory = temporary.path,
                                                                   .baseName = "recovery",
                                                                   .queueCapacity = 16,
                                                                   .maxFileBytes = 1024U * 1024U,
                                                                   .maxRolledFiles = 1,
                                                                   .echoToStderr = false}));
    Horo::Log::Logger::Shutdown();

    const std::string log = ReadText(path);
    REQUIRE(log.starts_with("{\"schemaVersion\":1,\"message\":\"complete\"}\n"));
    REQUIRE(log.find("partial") == std::string::npos);
    REQUIRE(log.ends_with("}\n"));
}

TEST_CASE("Oversized log records are failure-isolated without violating the file limit",
          "[foundation][observability][logging][failure-isolation]") {
    LoggerGuard loggerGuard;
    TemporaryDirectory temporary;
    const auto before = Horo::Telemetry::Runtime::GetStatistics();
    REQUIRE(Horo::Log::Logger::Init(Horo::Log::LoggerConfiguration{.logDirectory = temporary.path,
                                                                   .baseName = "bounded",
                                                                   .queueCapacity = 16,
                                                                   .maxFileBytes = 512,
                                                                   .maxRolledFiles = 1,
                                                                   .echoToStderr = false}));
    const auto acceptedBefore = Horo::Log::Logger::Statistics().acceptedRecords;
    while (Horo::Log::Logger::Statistics().acceptedRecords == acceptedBefore)
        Horo::Log::Logger::Write("test.oversized", Horo::Log::Level::Info, std::string(2048, 'x'));
    REQUIRE(Horo::Log::Logger::Flush());
    Horo::Log::Logger::Shutdown();
    const auto after = Horo::Telemetry::Runtime::GetStatistics();

    REQUIRE(after.sinkFailures > before.sinkFailures);
    REQUIRE(std::filesystem::file_size(temporary.path / "bounded.jsonl") <= 512);
}

TEST_CASE("Unavailable log directories fail initialization without enabling producers",
          "[foundation][observability][logging][storage][failure]") {
    LoggerGuard loggerGuard;
    TemporaryDirectory temporary;
    const std::filesystem::path regularFile = temporary.path / "not-a-directory";
    std::ofstream(regularFile) << "occupied";
    REQUIRE_FALSE(Horo::Log::Logger::Init(
        Horo::Log::LoggerConfiguration{.logDirectory = regularFile, .baseName = "unavailable", .echoToStderr = false}));
    REQUIRE_FALSE(Horo::Telemetry::Runtime::IsEnabled());
}

TEST_CASE("JSONL replacement failures remain observable and isolated from sibling sinks",
          "[foundation][observability][logging][rolling][failure]") {
    LoggerGuard loggerGuard;
    TemporaryDirectory temporary;
    const std::filesystem::path blockedSegment = temporary.path / "replacement.jsonl.1";
    std::filesystem::create_directory(blockedSegment);
    std::ofstream(blockedSegment / "keep") << "prevents directory removal";

    auto store = std::make_shared<Horo::Log::StructuredLogStore>(64);
    Horo::Log::Logger::SetStructuredLogStore(store);
    REQUIRE(Horo::Log::Logger::Init(Horo::Log::LoggerConfiguration{.logDirectory = temporary.path,
                                                                   .baseName = "replacement",
                                                                   .queueCapacity = 128,
                                                                   .maxFileBytes = 512,
                                                                   .maxRolledFiles = 1,
                                                                   .echoToStderr = false}));
    const auto before = Horo::Telemetry::Runtime::GetStatistics();
    for (int index = 0; index < 32; ++index)
        Horo::Log::Logger::Write("test.replacement", Horo::Log::Level::Error,
                                 Horo::Log::FormatArgs("record-%d-abcdefghijklmnopqrstuvwxyz", index));
    REQUIRE(Horo::Log::Logger::Flush());
    const auto after = Horo::Telemetry::Runtime::GetStatistics();
    Horo::Log::Logger::Shutdown();

    REQUIRE(after.sinkFailures > before.sinkFailures);
    const auto snapshot = store->SnapshotIfChanged(0);
    REQUIRE(snapshot.has_value());
    REQUIRE_FALSE(snapshot->records.empty());
    REQUIRE(std::filesystem::is_directory(blockedSegment));
}

#if HORO_ENABLE_TELEMETRY
TEST_CASE("Telemetry exports typed instruments and context-forwarded events asynchronously", "[foundation][observability][telemetry]") {
    Horo::Telemetry::Runtime::Shutdown();
    auto sink = std::make_shared<CollectingSink>();
    REQUIRE(Horo::Telemetry::Runtime::Initialize({.queueCapacity = 256, .enabled = true}, sink));
    const auto counter = Horo::Telemetry::Runtime::RegisterCounter(
        {.name = "jobs.completed", .subsystem = "jobs", .unit = Horo::Telemetry::MetricUnit::Count});
    const auto gauge = Horo::Telemetry::Runtime::RegisterGauge(
        {.name = "render.queue_depth", .subsystem = "render", .unit = Horo::Telemetry::MetricUnit::Count});
    const auto histogram = Horo::Telemetry::Runtime::RegisterHistogram(
        {.name = "frame.duration", .subsystem = "runtime", .unit = Horo::Telemetry::MetricUnit::Seconds});
    REQUIRE(static_cast<bool>(counter));
    REQUIRE(static_cast<bool>(gauge));
    REQUIRE(static_cast<bool>(histogram));
    REQUIRE(RecordTelemetryEventually([&counter] {
        counter.Add(2);
    }));
    REQUIRE(RecordTelemetryEventually([&gauge] {
        gauge.Set(3.0);
    }));
    REQUIRE(RecordTelemetryEventually([&histogram] {
        histogram.Observe(16.5);
    }));
    const auto context = Horo::Log::LogContextSnapshot{{{"correlation.id", "job-9"}}};
    bool eventAccepted = false;
    for (int attempt = 0; attempt < 1000 && !eventAccepted; ++attempt)
        eventAccepted = Horo::Telemetry::Runtime::EmitEvent("jobs", "job.failed", Horo::Log::Level::Error, "cook failed", context);
    REQUIRE(eventAccepted);
    REQUIRE(Horo::Telemetry::Runtime::Flush());
    Horo::Telemetry::Runtime::Shutdown();

    std::lock_guard lock(sink->mutex);
    const auto hasDescriptor = [&sink](const std::string_view name) {
        return std::ranges::any_of(sink->descriptors, [name](const auto &descriptor) {
            return descriptor.name == name;
        });
    };
    REQUIRE(hasDescriptor("jobs.completed"));
    REQUIRE(hasDescriptor("render.queue_depth"));
    REQUIRE(hasDescriptor("frame.duration"));
    const auto event = std::ranges::find_if(sink->records, [](const auto &record) {
        const auto *event = std::get_if<Horo::Telemetry::DiagnosticEvent>(&record.payload);
        return event != nullptr && event->name == "job.failed";
    });
    REQUIRE(event != sink->records.end());
    REQUIRE(event->context.Fields().front().second == "job-9");
    REQUIRE(sink->flushed);
}

TEST_CASE("Metric descriptors bind only allowlisted bounded dimension series", "[foundation][observability][telemetry][dimensions]") {
    static_cast<void>(Horo::Telemetry::Runtime::Shutdown());
    auto sink = std::make_shared<CollectingSink>();
    REQUIRE(Horo::Telemetry::Runtime::Initialize({.queueCapacity = 64, .enabled = true}, sink));
    const auto before = Horo::Telemetry::Runtime::GetStatistics();
    const auto counter = Horo::Telemetry::Runtime::RegisterCounter({
        .name = "asset.import.completed",
        .subsystem = "Assets.Import",
        .unit = Horo::Telemetry::MetricUnit::Count,
        .description = "Completed asset imports by terminal outcome.",
        .dimensions = {{.key = "outcome", .allowedValues = {"success", "failed", "cancelled"}}},
        .maxSeries = 2,
    });
    REQUIRE_FALSE(static_cast<bool>(counter));

    const auto succeeded = counter.WithDimensions(std::array{Horo::Telemetry::DimensionValue{.key = "outcome", .value = "success"}});
    const auto failed = counter.WithDimensions(std::array{Horo::Telemetry::DimensionValue{.key = "outcome", .value = "failed"}});
    REQUIRE(static_cast<bool>(succeeded));
    REQUIRE(static_cast<bool>(failed));
    REQUIRE_FALSE(
        static_cast<bool>(counter.WithDimensions(std::array{Horo::Telemetry::DimensionValue{.key = "outcome", .value = "cancelled"}})));
    REQUIRE_FALSE(static_cast<bool>(
        counter.WithDimensions(std::array{Horo::Telemetry::DimensionValue{.key = "asset.id", .value = "high-cardinality"}})));

    const auto acceptedBefore = Horo::Telemetry::Runtime::GetStatistics().acceptedRecords;
    while (Horo::Telemetry::Runtime::GetStatistics().acceptedRecords == acceptedBefore)
        succeeded.Add(3);
    const auto acceptedAfterSuccess = Horo::Telemetry::Runtime::GetStatistics().acceptedRecords;
    while (Horo::Telemetry::Runtime::GetStatistics().acceptedRecords == acceptedAfterSuccess)
        failed.Add();
    REQUIRE(Horo::Telemetry::Runtime::Flush());
    REQUIRE(Horo::Telemetry::Runtime::Shutdown());
    const auto after = Horo::Telemetry::Runtime::GetStatistics();

    REQUIRE(after.rejectedMetricSeries >= before.rejectedMetricSeries + 2U);
    std::lock_guard lock(sink->mutex);
    REQUIRE(sink->records.size() == 2);
    const auto &first = std::get<Horo::Telemetry::MetricRecord>(sink->records[0].payload);
    const auto &second = std::get<Horo::Telemetry::MetricRecord>(sink->records[1].payload);
    REQUIRE(first.kind == Horo::Telemetry::InstrumentKind::Counter);
    REQUIRE(first.value == 3.0);
    REQUIRE(first.dimensionCount == 1);
    REQUIRE(first.dimensionValueIds[0] != second.dimensionValueIds[0]);
    REQUIRE(sink->descriptors[0].dimensions[0].key == "outcome");
}

TEST_CASE("Metric registration rejects duplicate and unsafe descriptors deterministically",
          "[foundation][observability][telemetry][registration]") {
    static_cast<void>(Horo::Telemetry::Runtime::Shutdown());
    auto sink = std::make_shared<CollectingSink>();
    REQUIRE(Horo::Telemetry::Runtime::Initialize({.queueCapacity = 16, .enabled = true}, sink));
    const auto before = Horo::Telemetry::Runtime::GetStatistics();
    REQUIRE(static_cast<bool>(Horo::Telemetry::Runtime::RegisterGauge(
        {.name = "renderer.queue.depth", .subsystem = "Renderer", .unit = Horo::Telemetry::MetricUnit::Count})));
    REQUIRE_FALSE(static_cast<bool>(Horo::Telemetry::Runtime::RegisterGauge(
        {.name = "renderer.queue.depth", .subsystem = "Renderer", .unit = Horo::Telemetry::MetricUnit::Count})));
    REQUIRE_FALSE(static_cast<bool>(Horo::Telemetry::Runtime::RegisterHistogram(
        {.name = "Renderer Unsafe Id", .subsystem = "Renderer", .unit = Horo::Telemetry::MetricUnit::Seconds})));
    REQUIRE(Horo::Telemetry::Runtime::Shutdown());
    const auto after = Horo::Telemetry::Runtime::GetStatistics();
    REQUIRE(after.invalidInstrumentRegistrations == before.invalidInstrumentRegistrations + 2U);
}

TEST_CASE("Gauge histogram and timing records preserve their instrument semantics", "[foundation][observability][telemetry][instruments]") {
    static_cast<void>(Horo::Telemetry::Runtime::Shutdown());
    auto sink = std::make_shared<CollectingSink>();
    REQUIRE(Horo::Telemetry::Runtime::Initialize({.queueCapacity = 64, .enabled = true}, sink));
    const auto gauge = Horo::Telemetry::Runtime::RegisterGauge(
        {.name = "renderer.queue.depth", .subsystem = "Renderer", .unit = Horo::Telemetry::MetricUnit::Count});
    const auto histogram = Horo::Telemetry::Runtime::RegisterHistogram(
        {.name = "renderer.frame.duration", .subsystem = "Renderer", .unit = Horo::Telemetry::MetricUnit::Seconds});
    const auto timing = Horo::Telemetry::Runtime::RegisterTiming(
        {.name = "asset.import.duration", .subsystem = "Assets", .unit = Horo::Telemetry::MetricUnit::Seconds});
    REQUIRE(static_cast<bool>(gauge));
    REQUIRE(static_cast<bool>(histogram));
    REQUIRE(static_cast<bool>(timing));

    const auto submitUntilAccepted = [](const auto &submit) {
        const auto accepted = Horo::Telemetry::Runtime::GetStatistics().acceptedRecords;
        while (Horo::Telemetry::Runtime::GetStatistics().acceptedRecords == accepted)
            submit();
    };
    submitUntilAccepted([&gauge] {
        gauge.Set(7.0);
    });
    submitUntilAccepted([&histogram] {
        histogram.Observe(0.016);
    });
    submitUntilAccepted([&timing] {
        timing.Record(std::chrono::milliseconds{25});
    });
    REQUIRE(Horo::Telemetry::Runtime::Flush());
    REQUIRE(Horo::Telemetry::Runtime::Shutdown());

    std::lock_guard lock(sink->mutex);
    REQUIRE(sink->records.size() == 3);
    REQUIRE(std::get<Horo::Telemetry::MetricRecord>(sink->records[0].payload).kind == Horo::Telemetry::InstrumentKind::Gauge);
    REQUIRE(std::get<Horo::Telemetry::MetricRecord>(sink->records[1].payload).kind == Horo::Telemetry::InstrumentKind::Histogram);
    const auto &timingRecord = std::get<Horo::Telemetry::MetricRecord>(sink->records[2].payload);
    REQUIRE(timingRecord.kind == Horo::Telemetry::InstrumentKind::Timing);
    REQUIRE(timingRecord.value == 0.025);
}

TEST_CASE("Operation spans emit one explicit terminal state and preserve nesting", "[foundation][observability][operations][lifecycle]") {
    static_cast<void>(Horo::Telemetry::Runtime::Shutdown());
    auto sink = std::make_shared<CollectingSink>();
    REQUIRE(Horo::Telemetry::Runtime::Initialize({.queueCapacity = 128, .enabled = true}, sink));

    const auto completeAccepted = [](const Horo::Telemetry::SpanStatus status, const bool abandon) {
        for (int attempt = 0; attempt < 1000; ++attempt) {
            const auto before = Horo::Telemetry::Runtime::GetStatistics().acceptedRecords;
            Horo::Telemetry::OperationId id{};
            {
                Horo::Telemetry::OperationSpan operation{"Foundation.Tests", "Test.Operation"};
                id = operation.Id();
                if (!abandon) {
                    REQUIRE(operation.Complete(status));
                    REQUIRE_FALSE(operation.Complete(status));
                }
            }
            if (Horo::Telemetry::Runtime::GetStatistics().acceptedRecords > before)
                return id;
        }
        return Horo::Telemetry::OperationId{};
    };

    const auto succeeded = completeAccepted(Horo::Telemetry::SpanStatus::Succeeded, false);
    const auto failed = completeAccepted(Horo::Telemetry::SpanStatus::Failed, false);
    const auto timedOut = completeAccepted(Horo::Telemetry::SpanStatus::TimedOut, false);
    const auto abandoned = completeAccepted(Horo::Telemetry::SpanStatus::Cancelled, true);
    REQUIRE(succeeded != 0);
    REQUIRE(failed != 0);
    REQUIRE(timedOut != 0);
    REQUIRE(abandoned != 0);
    {
        Horo::Telemetry::OperationSpan invalidTransition{"Foundation.Tests", "Test.InvalidTransition"};
        REQUIRE_FALSE(invalidTransition.Complete(Horo::Telemetry::SpanStatus::Unset));
        REQUIRE(invalidTransition.Complete(Horo::Telemetry::SpanStatus::Succeeded));
    }
    REQUIRE(Horo::Telemetry::Runtime::Flush());
    REQUIRE(Horo::Telemetry::Runtime::Shutdown());

    std::lock_guard lock(sink->mutex);
    const auto statusFor = [&sink](const Horo::Telemetry::OperationId id) {
        const auto found = std::ranges::find_if(sink->records, [id](const auto &record) {
            const auto *span = std::get_if<Horo::Telemetry::SpanRecord>(&record.payload);
            return span != nullptr && span->operationId == id;
        });
        REQUIRE(found != sink->records.end());
        return std::get<Horo::Telemetry::SpanRecord>(found->payload).status;
    };
    REQUIRE(statusFor(succeeded) == Horo::Telemetry::SpanStatus::Succeeded);
    REQUIRE(statusFor(failed) == Horo::Telemetry::SpanStatus::Failed);
    REQUIRE(statusFor(timedOut) == Horo::Telemetry::SpanStatus::TimedOut);
    REQUIRE(statusFor(abandoned) == Horo::Telemetry::SpanStatus::Cancelled);
}

TEST_CASE("JobSystem forwards operation and correlation context into nested work", "[foundation][observability][operations][jobs]") {
    static_cast<void>(Horo::Telemetry::Runtime::Shutdown());
    auto sink = std::make_shared<CollectingSink>();
    REQUIRE(Horo::Telemetry::Runtime::Initialize({.queueCapacity = 128, .enabled = true}, sink));
    Horo::JobSystem jobs{{.workerCount = 1, .maxQueuedJobs = 8}};
    Horo::Telemetry::OperationId observedParent{};
    Horo::Telemetry::OperationId childId{};
    std::string observedCorrelation;
    bool eventAccepted{};
    bool childCompleted{};
    Horo::Telemetry::OperationId childParent{};

    Horo::Telemetry::OperationSpan outer{"Assets.Import", "Asset.Import"};
    const Horo::Telemetry::OperationId outerId = outer.Id();
    const auto submitted = jobs.SubmitResult({}, [&](const Horo::CancellationToken &) {
        const auto inherited = Horo::Telemetry::CaptureOperationContext();
        observedParent = inherited.operationId;
        Horo::Telemetry::OperationSpan child{"Assets.Decode", "Asset.Decode"};
        childId = child.Id();
        childParent = child.ParentId();
        const auto active = Horo::Telemetry::CaptureOperationContext();
        const auto correlation =
            std::ranges::find(active.diagnosticContext.Fields(), std::string_view{"correlation.id"}, &Horo::Log::MdcField::first);
        if (correlation != active.diagnosticContext.Fields().end())
            observedCorrelation = correlation->second;
        for (int attempt = 0; attempt < 1000 && !eventAccepted; ++attempt)
            eventAccepted = Horo::Telemetry::Runtime::EmitEvent("Assets.Decode", "asset.decoded", Horo::Log::Level::Info, "decoded");
        childCompleted = child.Complete(Horo::Telemetry::SpanStatus::Succeeded);
        return Horo::Result<void>::Success();
    });
    REQUIRE(submitted.HasValue());
    REQUIRE(submitted.Value().Wait().HasValue());
    REQUIRE(observedParent == outerId);
    REQUIRE(childParent == outerId);
    REQUIRE_FALSE(observedCorrelation.empty());
    REQUIRE(eventAccepted);
    REQUIRE(childCompleted);
    REQUIRE(outer.Complete(Horo::Telemetry::SpanStatus::Succeeded));
    jobs.Shutdown(Horo::ShutdownPolicy::Drain);
    REQUIRE(Horo::Telemetry::Runtime::Flush());
    REQUIRE(Horo::Telemetry::Runtime::Shutdown());

    std::lock_guard lock(sink->mutex);
    const auto event = std::ranges::find_if(sink->records, [childId](const auto &record) {
        const auto *diagnostic = std::get_if<Horo::Telemetry::DiagnosticEvent>(&record.payload);
        if (diagnostic == nullptr || diagnostic->name != "asset.decoded")
            return false;
        return std::ranges::any_of(record.context.Fields(), [childId](const Horo::Log::MdcField &field) {
            return field.first == "operation.id" && field.second == std::to_string(childId);
        });
    });
    REQUIRE(event != sink->records.end());
}

TEST_CASE("Captured operation context remains lifetime safe after its source scope completes",
          "[foundation][observability][operations][context]") {
    Horo::Telemetry::OperationContext captured;
    Horo::Telemetry::OperationId sourceId{};
    {
        Horo::Telemetry::OperationSpan source{"Editor.Project", "Project.Open"};
        sourceId = source.Id();
        captured = source.Context();
        REQUIRE(source.Complete(Horo::Telemetry::SpanStatus::Succeeded));
    }

    Horo::Telemetry::OperationId restored{};
    Horo::Telemetry::OperationId childParent{};
    bool childCompleted{};
    std::thread worker([&] {
        const Horo::Telemetry::ScopedOperationContext binding{captured};
        restored = Horo::Telemetry::CaptureOperationContext().operationId;
        Horo::Telemetry::OperationSpan child{"Editor.Project", "Project.Validate"};
        childParent = child.ParentId();
        childCompleted = child.Complete(Horo::Telemetry::SpanStatus::Cancelled);
    });
    worker.join();
    REQUIRE(restored == sourceId);
    REQUIRE(childParent == sourceId);
    REQUIRE(childCompleted);
}

TEST_CASE("Terminal operation history survives restart with parent status fields and context",
          "[foundation][observability][history][persistence]") {
    TemporaryDirectory temporary;
    auto history = Horo::Diagnostics::OperationHistorySink::Create(
        {.directory = temporary.path, .baseName = "jobs", .maxFileBytes = 4096, .maxRolledFiles = 2, .maxRecoveredRecords = 8});
    REQUIRE(history != nullptr);
    REQUIRE(Horo::Telemetry::Runtime::Initialize({.queueCapacity = 32, .enabled = true}, history));

    const auto emitSpan = [](const Horo::Telemetry::OperationId id, const Horo::Telemetry::OperationId parent,
                             const Horo::Telemetry::SpanStatus status) {
        for (int attempt = 0; attempt < 1000; ++attempt) {
            if (Horo::Telemetry::Runtime::EmitRecord(
                    {.subsystem = "Assets.Import",
                     .context = Horo::Log::LogContextSnapshot{{{"correlation.id", "import-42"}}},
                     .payload = Horo::Telemetry::SpanRecord{.operationId = id,
                                                            .parentOperationId = parent,
                                                            .name = "Asset.Import",
                                                            .status = status,
                                                            .duration = std::chrono::milliseconds{17},
                                                            .fields = {{.key = "asset.type", .value = std::string{"mesh"}}}}}))
                return true;
        }
        return false;
    };
    REQUIRE(emitSpan(10, 0, Horo::Telemetry::SpanStatus::Succeeded));
    REQUIRE(emitSpan(11, 10, Horo::Telemetry::SpanStatus::Failed));
    REQUIRE(emitSpan(12, 10, Horo::Telemetry::SpanStatus::Cancelled));
    REQUIRE(Horo::Telemetry::Runtime::Shutdown());
    history.reset();

    const auto recovered = Horo::Diagnostics::OperationHistorySink::Create(
        {.directory = temporary.path, .baseName = "jobs", .maxFileBytes = 4096, .maxRolledFiles = 2, .maxRecoveredRecords = 8});
    REQUIRE(recovered != nullptr);
    const auto snapshot = recovered->Snapshot();
    REQUIRE(snapshot.size() == 3);
    REQUIRE(snapshot[0].status == Horo::Telemetry::SpanStatus::Succeeded);
    REQUIRE(snapshot[1].parentOperationId == 10);
    REQUIRE(snapshot[1].status == Horo::Telemetry::SpanStatus::Failed);
    REQUIRE(snapshot[2].status == Horo::Telemetry::SpanStatus::Cancelled);
    REQUIRE(snapshot[1].durationNanoseconds == std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::milliseconds{17}).count());
    REQUIRE(std::get<std::string>(snapshot[1].fields[0].value) == "mesh");
    REQUIRE(snapshot[1].context.Fields()[0].second == "import-42");
}

TEST_CASE("Operation history rejects path-bearing base names", "[foundation][observability][history][security]") {
    TemporaryDirectory temporary;

    const auto history = Horo::Diagnostics::OperationHistorySink::Create(
        {.directory = temporary.path, .baseName = "../escaped", .maxFileBytes = 4096, .maxRolledFiles = 2, .maxRecoveredRecords = 8});

    REQUIRE(history == nullptr);
    REQUIRE_FALSE(std::filesystem::exists(temporary.path.parent_path() / "escaped.jsonl"));
}

TEST_CASE("Operation history rolling and partial-record recovery remain bounded", "[foundation][observability][history][recovery]") {
    TemporaryDirectory temporary;
    auto history = Horo::Diagnostics::OperationHistorySink::Create(
        {.directory = temporary.path, .baseName = "bounded", .maxFileBytes = 512, .maxRolledFiles = 2, .maxRecoveredRecords = 4});
    REQUIRE(history != nullptr);
    REQUIRE(Horo::Telemetry::Runtime::Initialize({.queueCapacity = 64, .enabled = true}, history));
    for (Horo::Telemetry::OperationId id = 1; id <= 12; ++id) {
        bool accepted = false;
        for (int attempt = 0; attempt < 1000 && !accepted; ++attempt) {
            accepted = Horo::Telemetry::Runtime::EmitRecord(
                {.subsystem = "Foundation.Jobs",
                 .payload = Horo::Telemetry::SpanRecord{.operationId = id,
                                                        .name = "Job.Run",
                                                        .status = Horo::Telemetry::SpanStatus::Succeeded,
                                                        .duration = std::chrono::milliseconds{static_cast<std::int64_t>(id)}}});
        }
        REQUIRE(accepted);
    }
    REQUIRE(Horo::Telemetry::Runtime::Shutdown());
    history.reset();
    REQUIRE(std::filesystem::exists(temporary.path / "bounded.jsonl.1"));
    REQUIRE_FALSE(std::filesystem::exists(temporary.path / "bounded.jsonl.3"));
    REQUIRE(std::filesystem::file_size(temporary.path / "bounded.jsonl") <= 512);

    std::ofstream(temporary.path / "bounded.jsonl", std::ios::app | std::ios::binary) << "{\"schemaVersion\":1,\"partial\":";
    const auto recovered = Horo::Diagnostics::OperationHistorySink::Create(
        {.directory = temporary.path, .baseName = "bounded", .maxFileBytes = 512, .maxRolledFiles = 2, .maxRecoveredRecords = 4});
    REQUIRE(recovered != nullptr);
    const auto snapshot = recovered->Snapshot();
    REQUIRE(snapshot.size() == 4);
    REQUIRE(snapshot.back().operationId == 12);
}

TEST_CASE("Failing operation history storage cannot block producers or sibling sinks",
          "[foundation][observability][history][failure][concurrency]") {
    static_cast<void>(Horo::Telemetry::Runtime::Shutdown());
    TemporaryDirectory temporary;
    const std::filesystem::path blockedSegment = temporary.path / "failing.jsonl.1";
    std::filesystem::create_directory(blockedSegment);
    std::ofstream(blockedSegment / "keep") << "occupied";

    auto history = Horo::Diagnostics::OperationHistorySink::Create(
        {.directory = temporary.path, .baseName = "failing", .maxFileBytes = 512, .maxRolledFiles = 1, .maxRecoveredRecords = 8});
    auto sibling = std::make_shared<CollectingSink>();
    REQUIRE(history != nullptr);
    REQUIRE(Horo::Telemetry::Runtime::Initialize({.queueCapacity = 64, .enabled = true},
                                                 std::vector<std::shared_ptr<Horo::Telemetry::ISink>>{history, sibling}));
    const auto before = Horo::Telemetry::Runtime::GetStatistics();
    const auto startedAt = std::chrono::steady_clock::now();
    for (Horo::Telemetry::OperationId id = 1; id <= 12; ++id) {
        bool accepted = false;
        for (int attempt = 0; attempt < 1000 && !accepted; ++attempt) {
            accepted = Horo::Telemetry::Runtime::EmitRecord(
                {.subsystem = "Foundation.Jobs",
                 .payload = Horo::Telemetry::SpanRecord{.operationId = id,
                                                        .name = "Job.With.Bounded.Persistent.History",
                                                        .status = Horo::Telemetry::SpanStatus::Succeeded,
                                                        .duration = std::chrono::milliseconds{1}}});
        }
        REQUIRE(accepted);
    }
    const auto producerElapsed = std::chrono::steady_clock::now() - startedAt;
    REQUIRE(Horo::Telemetry::Runtime::Flush());
    const auto after = Horo::Telemetry::Runtime::GetStatistics();
    REQUIRE(Horo::Telemetry::Runtime::Shutdown());

    REQUIRE(producerElapsed < std::chrono::seconds{1});
    REQUIRE(after.sinkFailures > before.sinkFailures);
    std::lock_guard lock(sibling->mutex);
    REQUIRE(sibling->records.size() == 12);
}

TEST_CASE("Unified observability records fan out identically to every sink", "[foundation][observability][telemetry][records][sinks]") {
    static_cast<void>(Horo::Telemetry::Runtime::Shutdown());
    auto firstSink = std::make_shared<CollectingSink>();
    auto secondSink = std::make_shared<CollectingSink>();
    REQUIRE(Horo::Telemetry::Runtime::Initialize({.queueCapacity = 64, .enabled = true},
                                                 std::vector<std::shared_ptr<Horo::Telemetry::ISink>>{firstSink, secondSink}));

    const auto counter = Horo::Telemetry::Runtime::RegisterCounter(
        {.name = "jobs.completed", .subsystem = "Foundation.Jobs", .unit = Horo::Telemetry::MetricUnit::Count});
    const auto acceptedBefore = Horo::Telemetry::Runtime::GetStatistics().acceptedRecords;
    while (Horo::Telemetry::Runtime::GetStatistics().acceptedRecords == acceptedBefore)
        counter.Add();

    const Horo::Log::LogContextSnapshot context{{{"correlation.id", "request-7"}}};
    const auto emitUntilAccepted = [](const auto &makeRecord) {
        for (int attempt = 0; attempt < 1000; ++attempt) {
            if (Horo::Telemetry::Runtime::EmitRecord(makeRecord()))
                return true;
        }
        return false;
    };
    REQUIRE(emitUntilAccepted([&context] {
        return Horo::Telemetry::Record{.subsystem = "Assets.Import",
                                       .context = context,
                                       .payload = Horo::Telemetry::LogRecord{.severity = Horo::Log::Level::Warn,
                                                                             .category = "asset.import",
                                                                             .message = "cache miss",
                                                                             .fields = {{.key = "attempt", .value = std::uint64_t{2}}}}};
    }));
    REQUIRE(emitUntilAccepted([&context] {
        return Horo::Telemetry::Record{.subsystem = "Renderer.Shader",
                                       .context = context,
                                       .payload = Horo::Telemetry::SpanRecord{.operationId = 9,
                                                                              .parentOperationId = 4,
                                                                              .name = "Shader.Compile",
                                                                              .status = Horo::Telemetry::SpanStatus::Succeeded,
                                                                              .duration = std::chrono::milliseconds{8}}};
    }));
    bool eventAccepted = false;
    for (int attempt = 0; attempt < 1000 && !eventAccepted; ++attempt) {
        eventAccepted =
            Horo::Telemetry::Runtime::EmitEvent("Editor.AssetBrowser", "asset.selected", Horo::Log::Level::Info, "asset selected",
                                                std::array{Horo::Telemetry::Field{.key = "asset.type", .value = std::string{"mesh"}}},
                                                context);
    }
    REQUIRE(eventAccepted);

    REQUIRE(Horo::Telemetry::Runtime::Flush());
    REQUIRE(Horo::Telemetry::Runtime::Shutdown());

    std::scoped_lock lock(firstSink->mutex, secondSink->mutex);
    REQUIRE(firstSink->records.size() == 4);
    REQUIRE(secondSink->records.size() == firstSink->records.size());
    for (std::size_t index = 0; index < firstSink->records.size(); ++index) {
        const auto &first = firstSink->records[index];
        const auto &second = secondSink->records[index];
        REQUIRE(first.sequence == index + 1U);
        REQUIRE(second.sequence == first.sequence);
        REQUIRE(first.Kind() == second.Kind());
        REQUIRE(first.timestampUtc.time_since_epoch() != std::chrono::system_clock::duration::zero());
        REQUIRE(first.monotonicTime.time_since_epoch() != std::chrono::steady_clock::duration::zero());
        REQUIRE_FALSE(first.subsystem.empty());
    }
    REQUIRE(std::ranges::any_of(firstSink->records, [](const auto &record) {
        return record.Kind() == Horo::Telemetry::RecordKind::Log;
    }));
    REQUIRE(std::ranges::any_of(firstSink->records, [](const auto &record) {
        return record.Kind() == Horo::Telemetry::RecordKind::Metric;
    }));
    REQUIRE(std::ranges::any_of(firstSink->records, [](const auto &record) {
        return record.Kind() == Horo::Telemetry::RecordKind::Span;
    }));
    REQUIRE(std::ranges::any_of(firstSink->records, [](const auto &record) {
        return record.Kind() == Horo::Telemetry::RecordKind::Event;
    }));
    REQUIRE(firstSink->flushed);
    REQUIRE(secondSink->flushed);
}

TEST_CASE("A throwing telemetry sink is isolated from sibling sinks and producer success",
          "[foundation][observability][telemetry][failure-isolation]") {
    static_cast<void>(Horo::Telemetry::Runtime::Shutdown());
    auto throwingSink = std::make_shared<ThrowingSink>();
    auto collectingSink = std::make_shared<CollectingSink>();
    REQUIRE(Horo::Telemetry::Runtime::Initialize({.queueCapacity = 16, .enabled = true},
                                                 std::vector<std::shared_ptr<Horo::Telemetry::ISink>>{throwingSink, collectingSink}));
    const auto before = Horo::Telemetry::Runtime::GetStatistics();
    bool accepted = false;
    for (int attempt = 0; attempt < 1000 && !accepted; ++attempt)
        accepted = Horo::Telemetry::Runtime::EmitEvent("Foundation.Telemetry", "sink.test", Horo::Log::Level::Info, "failure isolation");
    REQUIRE(accepted);
    REQUIRE(Horo::Telemetry::Runtime::Flush());
    REQUIRE_FALSE(Horo::Telemetry::Runtime::Shutdown());
    const auto after = Horo::Telemetry::Runtime::GetStatistics();

    std::lock_guard lock(collectingSink->mutex);
    REQUIRE(collectingSink->records.size() == 1);
    REQUIRE(collectingSink->flushed);
    REQUIRE(after.sinkFailures >= before.sinkFailures + 2U);
    REQUIRE(after.shutdownTimeouts == before.shutdownTimeouts);
}

TEST_CASE("Telemetry severity and hierarchical subsystem filters run before record construction",
          "[foundation][observability][telemetry][filtering][fast-path]") {
    static_cast<void>(Horo::Telemetry::Runtime::Shutdown());
    auto sink = std::make_shared<CollectingSink>();
    REQUIRE(Horo::Telemetry::Runtime::Initialize({.queueCapacity = 16,
                                                  .minimumEventSeverity = Horo::Log::Level::Warn,
                                                  .subsystemPrefixes = {"Renderer"},
                                                  .enabled = true},
                                                 sink));
    REQUIRE_FALSE(Horo::Telemetry::Runtime::IsEventEnabled("Renderer.OpenGL", Horo::Log::Level::Debug));
    REQUIRE(Horo::Telemetry::Runtime::IsEventEnabled("Renderer.OpenGL", Horo::Log::Level::Warn));
    REQUIRE_FALSE(Horo::Telemetry::Runtime::IsEventEnabled("Editor.Viewport", Horo::Log::Level::Critical));
    REQUIRE_FALSE(static_cast<bool>(Horo::Telemetry::Runtime::RegisterCounter(
        {.name = "editor.frames", .subsystem = "Editor", .unit = Horo::Telemetry::MetricUnit::Count})));
    REQUIRE(static_cast<bool>(Horo::Telemetry::Runtime::RegisterCounter(
        {.name = "renderer.frames", .subsystem = "Renderer", .unit = Horo::Telemetry::MetricUnit::Count})));

    const auto before = Horo::Telemetry::Runtime::GetStatistics();
    REQUIRE_FALSE(
        Horo::Telemetry::Runtime::EmitEvent("Renderer.OpenGL", "debug.filtered", Horo::Log::Level::Debug, "filtered by severity"));
    REQUIRE_FALSE(
        Horo::Telemetry::Runtime::EmitEvent("Editor.Viewport", "subsystem.filtered", Horo::Log::Level::Critical, "filtered by subsystem"));
    const auto afterFiltering = Horo::Telemetry::Runtime::GetStatistics();
    REQUIRE(afterFiltering.filteredRecords == before.filteredRecords + 2U);
    REQUIRE(afterFiltering.droppedRecords == before.droppedRecords);
    bool accepted = false;
    for (int attempt = 0; attempt < 1000 && !accepted; ++attempt) {
        accepted = Horo::Telemetry::Runtime::EmitEvent("Renderer.OpenGL", "backend.degraded", Horo::Log::Level::Warn, "fallback selected");
    }
    REQUIRE(accepted);
    REQUIRE(Horo::Telemetry::Runtime::Flush());
    REQUIRE(Horo::Telemetry::Runtime::Shutdown());
    const auto after = Horo::Telemetry::Runtime::GetStatistics();
    REQUIRE(after.filteredRecords == before.filteredRecords + 2U);
    std::lock_guard lock(sink->mutex);
    REQUIRE(sink->records.size() == 1);
    REQUIRE(sink->records.front().subsystem == "Renderer.OpenGL");
}

TEST_CASE("Typed metric collection levels reject detailed instruments before handles are created",
          "[foundation][observability][telemetry][metrics][configuration]") {
    static_cast<void>(Horo::Telemetry::Runtime::Shutdown());
    auto sink = std::make_shared<CollectingSink>();
    REQUIRE(Horo::Telemetry::Runtime::Initialize({.queueCapacity = 16,
                                                  .metricCollectionLevel = Horo::Telemetry::MetricCollectionLevel::Core,
                                                  .enabled = true},
                                                 sink));
    const auto core = Horo::Telemetry::Runtime::RegisterGauge(
        {.name = "process.thread.count", .subsystem = "Foundation.Process", .unit = Horo::Telemetry::MetricUnit::Count});
    const auto detailed =
        Horo::Telemetry::Runtime::RegisterHistogram({.name = "renderer.command.duration",
                                                     .subsystem = "Renderer.Frontend",
                                                     .unit = Horo::Telemetry::MetricUnit::Seconds,
                                                     .minimumCollectionLevel = Horo::Telemetry::MetricCollectionLevel::Detailed});
    REQUIRE(core);
    REQUIRE_FALSE(detailed);
    REQUIRE(Horo::Telemetry::Runtime::Shutdown());

    REQUIRE(Horo::Telemetry::Runtime::Initialize({.queueCapacity = 16,
                                                  .metricCollectionLevel = Horo::Telemetry::MetricCollectionLevel::Off,
                                                  .enabled = true},
                                                 sink));
    REQUIRE_FALSE(Horo::Telemetry::Runtime::RegisterCounter(
        {.name = "jobs.completed", .subsystem = "Foundation.Jobs", .unit = Horo::Telemetry::MetricUnit::Count}));
    REQUIRE(Horo::Telemetry::Runtime::IsEventEnabled("Foundation.Jobs", Horo::Log::Level::Info));
    REQUIRE(Horo::Telemetry::Runtime::Shutdown());
}

TEST_CASE("Telemetry shutdown reports a bounded timeout for a blocked sink",
          "[foundation][observability][telemetry][shutdown][non-blocking]") {
    static_cast<void>(Horo::Telemetry::Runtime::Shutdown());
    auto sink = std::make_shared<BlockingSink>();
    REQUIRE(Horo::Telemetry::Runtime::Initialize({.queueCapacity = 4, .shutdownTimeout = std::chrono::milliseconds{25}, .enabled = true},
                                                 sink));
    const auto counter = Horo::Telemetry::Runtime::RegisterCounter(
        {.name = "shutdown.blocked", .subsystem = "Foundation.Telemetry", .unit = Horo::Telemetry::MetricUnit::Count});
    const auto acceptedBefore = Horo::Telemetry::Runtime::GetStatistics().acceptedRecords;
    while (Horo::Telemetry::Runtime::GetStatistics().acceptedRecords == acceptedBefore)
        counter.Add();
    sink->WaitUntilEntered();

    const auto before = Horo::Telemetry::Runtime::GetStatistics();
    const auto startedAt = std::chrono::steady_clock::now();
    REQUIRE_FALSE(Horo::Telemetry::Runtime::Shutdown());
    const auto elapsed = std::chrono::steady_clock::now() - startedAt;
    const auto after = Horo::Telemetry::Runtime::GetStatistics();
    REQUIRE(elapsed < std::chrono::milliseconds{250});
    REQUIRE(after.shutdownTimeouts == before.shutdownTimeouts + 1U);

    sink->Release();
    REQUIRE(sink->WaitUntilFlushed(std::chrono::seconds{1}));
}

TEST_CASE("DropOldest overflow retains new records without growing the bounded queue", "[foundation][observability][telemetry][overflow]") {
    static_cast<void>(Horo::Telemetry::Runtime::Shutdown());
    auto sink = std::make_shared<BlockingSink>();
    REQUIRE(Horo::Telemetry::Runtime::Initialize({.queueCapacity = 2,
                                                  .overflowPolicy = Horo::Telemetry::OverflowPolicy::DropOldest,
                                                  .shutdownTimeout = std::chrono::seconds{1},
                                                  .enabled = true},
                                                 sink));
    const auto counter = Horo::Telemetry::Runtime::RegisterCounter(
        {.name = "queue.drop_oldest", .subsystem = "Foundation.Telemetry", .unit = Horo::Telemetry::MetricUnit::Count});
    const auto acceptedBeforeBlocking = Horo::Telemetry::Runtime::GetStatistics().acceptedRecords;
    while (Horo::Telemetry::Runtime::GetStatistics().acceptedRecords == acceptedBeforeBlocking)
        counter.Add();
    sink->WaitUntilEntered();
    const auto before = Horo::Telemetry::Runtime::GetStatistics();
    counter.Add();
    counter.Add();
    counter.Add();
    const auto pressured = Horo::Telemetry::Runtime::GetStatistics();
    REQUIRE(pressured.acceptedRecords == before.acceptedRecords + 3U);
    REQUIRE(pressured.queueFullDrops == before.queueFullDrops + 1U);
    REQUIRE(pressured.droppedRecords == before.droppedRecords + 1U);

    sink->Release();
    REQUIRE(Horo::Telemetry::Runtime::Shutdown());
    const auto after = Horo::Telemetry::Runtime::GetStatistics();
    REQUIRE(after.exportedRecords == before.exportedRecords + 3U);
}

TEST_CASE("Disabled telemetry handles do not enqueue or drop samples", "[foundation][observability][telemetry][fast-path]") {
    Horo::Telemetry::Runtime::Shutdown();
    auto sink = std::make_shared<CollectingSink>();
    REQUIRE(Horo::Telemetry::Runtime::Initialize({.queueCapacity = 16, .enabled = true}, sink));
    const auto counter = Horo::Telemetry::Runtime::RegisterCounter(
        {.name = "disabled.counter", .subsystem = "test", .unit = Horo::Telemetry::MetricUnit::Count});
    Horo::Telemetry::Runtime::SetEnabled(false);
    const Horo::Telemetry::Statistics before = Horo::Telemetry::Runtime::GetStatistics();
    for (int index = 0; index < 10000; ++index)
        counter.Add();
    const Horo::Telemetry::Statistics after = Horo::Telemetry::Runtime::GetStatistics();
    REQUIRE(after.acceptedRecords == before.acceptedRecords);
    REQUIRE(after.droppedRecords == before.droppedRecords);
    Horo::Telemetry::Runtime::Shutdown();
}

TEST_CASE("Telemetry handles cannot alias instruments from a later runtime generation",
          "[foundation][observability][telemetry][lifetime]") {
    Horo::Telemetry::Runtime::Shutdown();
    auto firstSink = std::make_shared<CollectingSink>();
    REQUIRE(Horo::Telemetry::Runtime::Initialize({.queueCapacity = 16, .enabled = true}, firstSink));
    const auto staleCounter = Horo::Telemetry::Runtime::RegisterCounter(
        {.name = "first.counter", .subsystem = "test", .unit = Horo::Telemetry::MetricUnit::Count});
    Horo::Telemetry::Runtime::Shutdown();

    auto secondSink = std::make_shared<CollectingSink>();
    REQUIRE(Horo::Telemetry::Runtime::Initialize({.queueCapacity = 16, .enabled = true}, secondSink));
    const auto currentCounter = Horo::Telemetry::Runtime::RegisterCounter(
        {.name = "second.counter", .subsystem = "test", .unit = Horo::Telemetry::MetricUnit::Count});
    staleCounter.Add();
    for (int attempt = 0; attempt < 100; ++attempt)
        currentCounter.Add();
    REQUIRE(Horo::Telemetry::Runtime::Flush());
    Horo::Telemetry::Runtime::Shutdown();
    std::lock_guard lock(secondSink->mutex);
    REQUIRE_FALSE(secondSink->records.empty());
    REQUIRE(std::ranges::all_of(secondSink->descriptors, [](const auto &descriptor) {
        return descriptor.name == "second.counter";
    }));
}

TEST_CASE("Telemetry producers drop from a full bounded queue without waiting for a blocked sink",
          "[foundation][observability][telemetry][non-blocking]") {
    Horo::Telemetry::Runtime::Shutdown();
    auto sink = std::make_shared<BlockingSink>();
    REQUIRE(Horo::Telemetry::Runtime::Initialize({.queueCapacity = 1, .enabled = true}, sink));
    const auto counter = Horo::Telemetry::Runtime::RegisterCounter(
        {.name = "queue.pressure", .subsystem = "test", .unit = Horo::Telemetry::MetricUnit::Count});
    const auto acceptedBeforeBlocking = Horo::Telemetry::Runtime::GetStatistics().acceptedRecords;
    while (Horo::Telemetry::Runtime::GetStatistics().acceptedRecords == acceptedBeforeBlocking)
        counter.Add();
    sink->WaitUntilEntered();
    const Horo::Telemetry::Statistics before = Horo::Telemetry::Runtime::GetStatistics();
    for (int index = 0; index < 1000; ++index)
        counter.Add();
    const Horo::Telemetry::Statistics after = Horo::Telemetry::Runtime::GetStatistics();
    REQUIRE(after.droppedRecords > before.droppedRecords);
    sink->Release();
    Horo::Telemetry::Runtime::Shutdown();
}

TEST_CASE("Telemetry accepts concurrent metric producers safely", "[foundation][observability][telemetry][concurrency]") {
    Horo::Telemetry::Runtime::Shutdown();
    auto sink = std::make_shared<CollectingSink>();
    REQUIRE(Horo::Telemetry::Runtime::Initialize({.queueCapacity = 4096, .enabled = true}, sink));
    const auto histogram = Horo::Telemetry::Runtime::RegisterHistogram(
        {.name = "job.duration", .subsystem = "jobs", .unit = Horo::Telemetry::MetricUnit::Seconds});
    const Horo::Telemetry::Statistics before = Horo::Telemetry::Runtime::GetStatistics();
    std::vector<std::thread> producers;
    for (int producer = 0; producer < 4; ++producer) {
        producers.emplace_back([histogram, producer] {
            for (int sample = 0; sample < 250; ++sample)
                histogram.Observe(static_cast<double>(producer * 250 + sample));
        });
    }
    for (std::thread &producer : producers)
        producer.join();
    REQUIRE(Horo::Telemetry::Runtime::Flush());
    const Horo::Telemetry::Statistics after = Horo::Telemetry::Runtime::GetStatistics();
    Horo::Telemetry::Runtime::Shutdown();
    std::lock_guard lock(sink->mutex);
    REQUIRE(sink->records.size() == after.acceptedRecords - before.acceptedRecords);
    REQUIRE(after.acceptedRecords + after.droppedRecords > before.acceptedRecords + before.droppedRecords);
}

TEST_CASE("Concurrent producers and shutdown race without blocking or retaining an enabled runtime",
          "[foundation][observability][telemetry][concurrency][shutdown]") {
    static_cast<void>(Horo::Telemetry::Runtime::Shutdown());
    auto sink = std::make_shared<CollectingSink>();
    REQUIRE(
        Horo::Telemetry::Runtime::Initialize({.queueCapacity = 256, .shutdownTimeout = std::chrono::seconds{1}, .enabled = true}, sink));
    const auto counter = Horo::Telemetry::Runtime::RegisterCounter(
        {.name = "shutdown.race", .subsystem = "Foundation.Telemetry", .unit = Horo::Telemetry::MetricUnit::Count});
    REQUIRE(counter);

    std::atomic<bool> start{};
    std::vector<std::thread> producers;
    for (int producer = 0; producer < 8; ++producer) {
        producers.emplace_back([counter, &start] {
            while (!start.load(std::memory_order_acquire))
                std::this_thread::yield();
            for (int sample = 0; sample < 2000; ++sample)
                counter.Add();
        });
    }
    const auto acceptedBefore = Horo::Telemetry::Runtime::GetStatistics().acceptedRecords;
    start.store(true, std::memory_order_release);
    for (int attempt = 0; attempt < 10'000 && Horo::Telemetry::Runtime::GetStatistics().acceptedRecords == acceptedBefore; ++attempt)
        std::this_thread::yield();
    REQUIRE(Horo::Telemetry::Runtime::GetStatistics().acceptedRecords > acceptedBefore);
    const auto startedAt = std::chrono::steady_clock::now();
    REQUIRE(Horo::Telemetry::Runtime::Shutdown());
    for (std::thread &producer : producers)
        producer.join();
    REQUIRE_FALSE(Horo::Telemetry::Runtime::IsEnabled());
    REQUIRE(std::chrono::steady_clock::now() - startedAt < std::chrono::seconds{1});
}
#else
TEST_CASE("Compile-time disabled telemetry remains inert", "[foundation][observability][telemetry][disabled-build]") {
    auto sink = std::make_shared<CollectingSink>();
    const auto before = Horo::Telemetry::Runtime::GetStatistics();
    REQUIRE(Horo::Telemetry::Runtime::Initialize({.queueCapacity = 8, .enabled = true}, sink));
    REQUIRE(Horo::Telemetry::Runtime::IsEnabled());
    const auto counter = Horo::Telemetry::Runtime::RegisterCounter(
        {.name = "disabled.counter", .subsystem = "Foundation.Telemetry", .unit = Horo::Telemetry::MetricUnit::Count});
    REQUIRE_FALSE(static_cast<bool>(counter));
    for (int index = 0; index < 10000; ++index)
        counter.Add();
    REQUIRE_FALSE(Horo::Telemetry::Runtime::EmitEvent("Foundation.Telemetry", "disabled", Horo::Log::Level::Info, "must not enqueue"));
    bool logAccepted = false;
    for (int attempt = 0; attempt < 1000 && !logAccepted; ++attempt) {
        logAccepted = Horo::Telemetry::Runtime::EmitRecord({.subsystem = "Foundation.Logging",
                                                            .payload = Horo::Telemetry::LogRecord{.severity = Horo::Log::Level::Info,
                                                                                                  .category = "foundation.logging",
                                                                                                  .message = "still enabled"}});
    }
    REQUIRE(logAccepted);
    REQUIRE(Horo::Telemetry::Runtime::Flush());
    REQUIRE(Horo::Telemetry::Runtime::Shutdown());
    const auto after = Horo::Telemetry::Runtime::GetStatistics();
    REQUIRE(after.acceptedRecords == before.acceptedRecords + 1U);
    REQUIRE(after.droppedRecords == before.droppedRecords);
    std::lock_guard lock(sink->mutex);
    REQUIRE(sink->records.size() == 1);
    REQUIRE(sink->records.front().Kind() == Horo::Telemetry::RecordKind::Log);
}
#endif

TEST_CASE("Terminal operations forward persistent job history through structured logging", "[foundation][observability][history]") {
    LoggerGuard loggerGuard;
    auto store = std::make_shared<Horo::Log::StructuredLogStore>(8);
    Horo::Log::Logger::SetStructuredLogStore(store);
    Horo::OperationStore operations{4, 4, std::make_shared<Horo::LoggingOperationHistorySink>()};
    const auto id = operations.Begin({.kind = Horo::OperationKind::Cook, .title = "Cook game"});
    REQUIRE(id.has_value());
    REQUIRE(operations.Update(*id, {.state = Horo::OperationState::Succeeded, .message = "complete"}));
    const auto snapshot = store->SnapshotIfChanged(0);
    REQUIRE(snapshot.has_value());
    REQUIRE(snapshot->records.size() == 1);
    REQUIRE(snapshot->records.front()->category == "jobs.history");
    REQUIRE(snapshot->records.front()->context.find("operation.id=") != std::string::npos);
}

TEST_CASE("Diagnostic bundle contains only allowlisted files and a checksum manifest", "[foundation][observability][bundle]") {
    TemporaryDirectory temporary;
    const std::filesystem::path logPath = temporary.path / "engine.jsonl";
    std::ofstream(logPath) << R"({"level":"error","message":"failure"})";
    const std::filesystem::path bundlePath = temporary.path / "diagnostics.zip";
    const auto result = Horo::Diagnostics::GenerateDiagnosticBundle({
        .outputPath = bundlePath,
        .entries = {{.sourcePath = logPath, .archivePath = "logs/engine.jsonl"}},
        .metadata = {{"app", "horo-editor"}, {"reason", "test"}},
        .maxInputBytes = 1024,
    });
    REQUIRE(result.HasValue());
    REQUIRE(result.Value().fileCount == 1);
    const std::string archive = ReadText(bundlePath);
    REQUIRE(archive.starts_with("PK"));
    REQUIRE(archive.find("manifest.json") != std::string::npos);
    REQUIRE(archive.find("logs/engine.jsonl") != std::string::npos);
    REQUIRE(archive.find("sha256:") != std::string::npos);
    REQUIRE_FALSE(std::filesystem::exists(bundlePath.string() + ".tmp"));

    mz_zip_archive reader{};
    REQUIRE(mz_zip_reader_init_file(&reader, bundlePath.string().c_str(), 0));
    REQUIRE(mz_zip_reader_locate_file(&reader, "manifest.json", nullptr, 0) >= 0);
    REQUIRE(mz_zip_reader_locate_file(&reader, "logs/engine.jsonl", nullptr, 0) >= 0);
    std::size_t manifestSize{};
    void *manifestData = mz_zip_reader_extract_file_to_heap(&reader, "manifest.json", &manifestSize, 0);
    REQUIRE(manifestData != nullptr);
    const std::string extractedManifest{static_cast<const char *>(manifestData), manifestSize};
    mz_free(manifestData);
    REQUIRE(extractedManifest.find("sha256:") != std::string::npos);
    REQUIRE(mz_zip_reader_end(&reader));
}

TEST_CASE("Diagnostic bundle rejects path escapes and oversized allowlists", "[foundation][observability][bundle][validation]") {
    TemporaryDirectory temporary;
    const std::filesystem::path logPath = temporary.path / "engine.jsonl";
    std::ofstream(logPath) << "diagnostic-data";
    const auto escaped = Horo::Diagnostics::GenerateDiagnosticBundle({
        .outputPath = temporary.path / "escaped.zip",
        .entries = {{.sourcePath = logPath, .archivePath = "../engine.jsonl"}},
        .maxInputBytes = 1024,
    });
    REQUIRE(escaped.HasError());
    REQUIRE(escaped.ErrorValue().code.Value() == "observability.bundle.invalid_request");

    const auto oversized = Horo::Diagnostics::GenerateDiagnosticBundle({
        .outputPath = temporary.path / "oversized.zip",
        .entries = {{.sourcePath = logPath, .archivePath = "logs/engine.jsonl"}},
        .maxInputBytes = 2,
    });
    REQUIRE(oversized.HasError());
    REQUIRE(oversized.ErrorValue().code.Value() == "observability.bundle.size_exceeded");
}

TEST_CASE("Diagnostic bundles are deterministic and report missing optional inputs", "[foundation][observability][bundle][determinism]") {
    TemporaryDirectory temporary;
    const auto log = temporary.path / "engine.jsonl";
    const auto history = temporary.path / "operations.jsonl";
    std::ofstream(log) << "log-data\n";
    std::ofstream(history) << "history-data\n";
    const auto missing = temporary.path / "crash.json";

    const auto first = Horo::Diagnostics::GenerateDiagnosticBundle({
        .outputPath = temporary.path / "first.zip",
        .entries = {{.sourcePath = history, .archivePath = "history/operations.jsonl"},
                    {.sourcePath = missing, .archivePath = "crash/report.json", .optional = true},
                    {.sourcePath = log, .archivePath = "logs/engine.jsonl"}},
        .metadata = {{"reason", "test"}, {"app", "horo-editor"}},
    });
    const auto second = Horo::Diagnostics::GenerateDiagnosticBundle({
        .outputPath = temporary.path / "second.zip",
        .entries = {{.sourcePath = log, .archivePath = "logs/engine.jsonl"},
                    {.sourcePath = history, .archivePath = "history/operations.jsonl"},
                    {.sourcePath = missing, .archivePath = "crash/report.json", .optional = true}},
        .metadata = {{"app", "horo-editor"}, {"reason", "test"}},
    });
    REQUIRE(first.HasValue());
    REQUIRE(second.HasValue());
    REQUIRE(first.Value().missingOptionalCount == 1);
    REQUIRE(ReadText(first.Value().outputPath) == ReadText(second.Value().outputPath));
}

TEST_CASE("Diagnostic bundle privacy rejects symlinks secrets absolute paths and unrelated content",
          "[foundation][observability][bundle][privacy]") {
    TemporaryDirectory temporary;
    const auto log = temporary.path / "engine.jsonl";
    std::ofstream(log) << "safe-log\n";

    const auto unrelated = Horo::Diagnostics::GenerateDiagnosticBundle({
        .outputPath = temporary.path / "unrelated.zip",
        .entries = {{.sourcePath = log, .archivePath = "project/source.cpp"}},
    });
    REQUIRE(unrelated.HasError());

    const auto secret = Horo::Diagnostics::GenerateDiagnosticBundle({
        .outputPath = temporary.path / "secret.zip",
        .entries = {{.sourcePath = log, .archivePath = "logs/engine.jsonl"}},
        .metadata = {{"api_token", "do-not-export"}},
    });
    REQUIRE(secret.HasError());

    const auto absolutePath = Horo::Diagnostics::GenerateDiagnosticBundle({
        .outputPath = temporary.path / "path.zip",
        .entries = {{.sourcePath = log, .archivePath = "logs/engine.jsonl"}},
        .metadata = {{"project", "/Users/example/private-project"}},
    });
    REQUIRE(absolutePath.HasError());

    const auto embeddedAbsolutePath = Horo::Diagnostics::GenerateDiagnosticBundle({
        .outputPath = temporary.path / "embedded-path.zip",
        .entries = {{.sourcePath = log, .archivePath = "logs/engine.jsonl"}},
        .metadata = {{"diagnostic", "failure at /Users/example/private-project"}},
    });
    REQUIRE(embeddedAbsolutePath.HasError());

    std::error_code error;
    const auto symlink = temporary.path / "linked-log.jsonl";
    std::filesystem::create_symlink(log, symlink, error);
    if (error) {
        SUCCEED("Symlink creation is unavailable on this platform");
        return;
    }
    const auto linked = Horo::Diagnostics::GenerateDiagnosticBundle({
        .outputPath = temporary.path / "linked.zip",
        .entries = {{.sourcePath = symlink, .archivePath = "logs/engine.jsonl"}},
    });
    REQUIRE(linked.HasError());
}

TEST_CASE("Diagnostic bundle redaction recovers a trailing partial record and rejects interior corruption",
          "[foundation][observability][bundle][privacy][recovery]") {
    TemporaryDirectory temporary;
    const auto recoveredLog = temporary.path / "recovered.jsonl";
    std::ofstream(recoveredLog) << R"({"message":"failed at /Users/example/project","auth.token":"secret"})" << '\n'
                                << R"({"message":"interrupted)";

    const auto recovered = Horo::Diagnostics::GenerateDiagnosticBundle({
        .outputPath = temporary.path / "recovered.zip",
        .entries = {{.sourcePath = recoveredLog, .archivePath = "logs/recovered.jsonl", .redactSensitiveText = true}},
        .maxInputBytes = 4096,
    });
    REQUIRE(recovered.HasValue());

    mz_zip_archive reader{};
    REQUIRE(mz_zip_reader_init_file(&reader, recovered.Value().outputPath.string().c_str(), 0));
    std::size_t extractedSize{};
    void *extractedData = mz_zip_reader_extract_file_to_heap(&reader, "logs/recovered.jsonl", &extractedSize, 0);
    REQUIRE(extractedData != nullptr);
    const std::string extracted{static_cast<const char *>(extractedData), extractedSize};
    mz_free(extractedData);
    REQUIRE(mz_zip_reader_end(&reader));
    REQUIRE(extracted.find("[REDACTED_PATH]") != std::string::npos);
    REQUIRE(extracted.find("[REDACTED]") != std::string::npos);
    REQUIRE(extracted.find("/Users/example/project") == std::string::npos);
    REQUIRE(extracted.find("secret") == std::string::npos);
    REQUIRE(extracted.find("interrupted") == std::string::npos);

    const auto corruptLog = temporary.path / "corrupt.jsonl";
    std::ofstream(corruptLog) << R"({"message":"valid"})" << '\n'
                              << "not-json\n"
                              << R"({"message":"also valid"})" << '\n';
    const auto corrupt = Horo::Diagnostics::GenerateDiagnosticBundle({
        .outputPath = temporary.path / "corrupt.zip",
        .entries = {{.sourcePath = corruptLog, .archivePath = "logs/corrupt.jsonl", .redactSensitiveText = true}},
        .maxInputBytes = 4096,
    });
    REQUIRE(corrupt.HasError());
    REQUIRE(corrupt.ErrorValue().code.Value() == "observability.bundle.read_failed");

    const auto expandingLog = temporary.path / "expanding.jsonl";
    std::ofstream(expandingLog) << R"({"paths":["/","/","/","/"]})";
    const auto expandingSize = std::filesystem::file_size(expandingLog);
    const auto expanding = Horo::Diagnostics::GenerateDiagnosticBundle({
        .outputPath = temporary.path / "expanding.zip",
        .entries = {{.sourcePath = expandingLog, .archivePath = "logs/expanding.jsonl", .redactSensitiveText = true}},
        .maxInputBytes = expandingSize,
    });
    REQUIRE(expanding.HasError());
    REQUIRE(expanding.ErrorValue().code.Value() == "observability.bundle.size_exceeded");
}
