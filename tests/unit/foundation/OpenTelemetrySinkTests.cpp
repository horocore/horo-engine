#include "Horo/Foundation/Logging/Logger.h"
#include "Horo/Foundation/Telemetry/OpenTelemetrySink.h"
#include "Horo/Foundation/Telemetry/Telemetry.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {
    class TemporaryDirectory final {
    public:
        TemporaryDirectory() {
            static std::atomic<std::uint64_t> sequence{};
            path = std::filesystem::temp_directory_path() /
                   ("horo-otel-tests-" + std::to_string(sequence.fetch_add(1, std::memory_order_relaxed)) + '-' +
                    std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
            std::filesystem::create_directories(path);
        }

        ~TemporaryDirectory() {
            std::error_code error;
            std::filesystem::remove_all(path, error);
        }

        std::filesystem::path path;
    };

    struct TransportCall {
        Horo::Telemetry::OtlpSignal signal{};
        std::string payload;
        std::thread::id threadId;
    };

    class CapturingTransport final : public Horo::Telemetry::IOtlpTransport {
    public:
        bool Post(const Horo::Telemetry::OtlpSignal signal, const std::string_view payload, const std::chrono::milliseconds) override {
            std::lock_guard lock(mutex);
            calls.push_back({.signal = signal, .payload = std::string{payload}, .threadId = std::this_thread::get_id()});
            if (outcomes.empty())
                return defaultOutcome;
            const bool result = outcomes.front();
            outcomes.erase(outcomes.begin());
            return result;
        }

        [[nodiscard]] std::vector<TransportCall> Snapshot() const {
            std::lock_guard lock(mutex);
            return calls;
        }

        mutable std::mutex mutex;
        std::vector<TransportCall> calls;
        std::vector<bool> outcomes;
        bool defaultOutcome{true};
    };

    class RuntimeGuard final {
    public:
        RuntimeGuard() {
            static_cast<void>(Horo::Telemetry::Runtime::Shutdown());
        }

        ~RuntimeGuard() {
            static_cast<void>(Horo::Telemetry::Runtime::Shutdown());
        }
    };

    [[nodiscard]] bool EmitEventually(Horo::Telemetry::Record record) {
        for (int attempt = 0; attempt < 10'000; ++attempt) {
            if (Horo::Telemetry::Runtime::EmitRecord(record))
                return true;
            std::this_thread::yield();
        }
        return false;
    }

    [[nodiscard]] bool AddEventually(const Horo::Telemetry::Counter &counter, const std::uint64_t value) {
        const std::uint64_t acceptedBefore = Horo::Telemetry::Runtime::GetStatistics().acceptedRecords;
        for (int attempt = 0; attempt < 10'000; ++attempt) {
            counter.Add(value);
            if (Horo::Telemetry::Runtime::GetStatistics().acceptedRecords > acceptedBefore)
                return true;
            std::this_thread::yield();
        }
        return false;
    }

    [[nodiscard]] std::optional<nlohmann::json> FindAttribute(const nlohmann::json &attributes, const std::string_view key) {
        for (const auto &attribute : attributes) {
            if (attribute.value("key", "") == key)
                return attribute.at("value");
        }
        return std::nullopt;
    }

    [[nodiscard]] const TransportCall &FindCall(const std::vector<TransportCall> &calls, const Horo::Telemetry::OtlpSignal signal) {
        const auto selected = std::find_if(calls.begin(), calls.end(), [signal](const TransportCall &call) {
            return call.signal == signal;
        });
        if (selected == calls.end())
            throw std::runtime_error{"missing OTLP signal call"};
        return *selected;
    }
}  // namespace

TEST_CASE("Logger host composition fans records to local JSONL and optional OTLP sinks",
          "[foundation][observability][opentelemetry][host]") {
    RuntimeGuard runtimeGuard;
    TemporaryDirectory temporary;
    auto transport = std::make_shared<CapturingTransport>();
    Horo::Telemetry::OpenTelemetryConfiguration exporterConfiguration;
    exporterConfiguration.maxBatchRecords = 8;
    exporterConfiguration.maxBufferedRecords = 8;
    exporterConfiguration.maxAttempts = 1;
    exporterConfiguration.exportApproved = true;
    const auto exporter = Horo::Telemetry::OpenTelemetrySink::Create(exporterConfiguration, transport);
    REQUIRE(exporter != nullptr);

    Horo::Log::LoggerConfiguration loggerConfiguration{.logDirectory = temporary.path,
                                                       .baseName = "composed",
                                                       .hostName = "HoroTestHost",
                                                       .additionalSinks = {exporter},
                                                       .echoToStderr = false};
    REQUIRE(Horo::Log::Logger::Init(loggerConfiguration));
    const auto acceptedBefore = Horo::Log::Logger::Statistics().acceptedRecords;
    for (int attempt = 0; attempt < 10'000 && Horo::Log::Logger::Statistics().acceptedRecords == acceptedBefore; ++attempt) {
        Horo::Log::Logger::Write("game.physics", Horo::Log::Level::Info, "simulation started");
        std::this_thread::yield();
    }
    REQUIRE(Horo::Log::Logger::Statistics().acceptedRecords > acceptedBefore);
    Horo::Log::Logger::Shutdown();

    std::ifstream input(temporary.path / "composed.jsonl", std::ios::binary);
    const std::string localLog{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
    REQUIRE(localLog.find("simulation started") != std::string::npos);
    const std::vector<TransportCall> calls = transport->Snapshot();
    REQUIRE(calls.size() == 1);
    const nlohmann::json payload = nlohmann::json::parse(calls.front().payload);
    const auto &records = payload.at("resourceLogs").at(0).at("scopeLogs").at(0).at("logRecords");
    REQUIRE(std::ranges::any_of(records, [](const nlohmann::json &record) {
        return record.at("body").at("stringValue") == "simulation started";
    }));
}

TEST_CASE("OpenTelemetry sink maps unified records to OTLP off the producer thread",
          "[foundation][observability][opentelemetry][mapping]") {
    RuntimeGuard runtimeGuard;
    auto transport = std::make_shared<CapturingTransport>();
    Horo::Telemetry::OpenTelemetryConfiguration configuration;
    configuration.serviceName = "horo-tests";
    configuration.maxBatchRecords = 4;
    configuration.maxBufferedRecords = 8;
    configuration.maxAttempts = 1;
    configuration.exportApproved = true;
    const auto sink = Horo::Telemetry::OpenTelemetrySink::Create(configuration, transport);
    REQUIRE(sink != nullptr);
    REQUIRE(Horo::Telemetry::Runtime::Initialize({.queueCapacity = 64, .enabled = true}, sink));

    const auto unboundCounter =
        Horo::Telemetry::Runtime::RegisterCounter({.name = "jobs.completed",
                                                   .subsystem = "foundation.jobs",
                                                   .unit = Horo::Telemetry::MetricUnit::Count,
                                                   .dimensions = {{.key = "result", .allowedValues = {"ok", "error"}}},
                                                   .maxSeries = 2});
    const std::array dimensions{Horo::Telemetry::DimensionValue{.key = "result", .value = "ok"}};
    const auto counter = unboundCounter.WithDimensions(dimensions);
    REQUIRE(static_cast<bool>(counter));

    const auto context = Horo::Log::LogContextSnapshot{}.With("correlation.id", "request-42").With("auth.token", "must-not-leave-process");
    const std::array logFields{Horo::Telemetry::Field{.key = "asset.id", .value = std::string{"mesh-7"}}};
    const std::thread::id producerThread = std::this_thread::get_id();
    REQUIRE(EmitEventually({.subsystem = "assets.import",
                            .context = context,
                            .payload = Horo::Telemetry::LogRecord{.severity = Horo::Log::Level::Error,
                                                                  .category = "assets.import",
                                                                  .message = "import failed",
                                                                  .fields = {logFields.begin(), logFields.end()}}}));
    REQUIRE(EmitEventually(
        {.subsystem = "assets.import",
         .context = context,
         .payload = Horo::Telemetry::DiagnosticEvent{.severity = Horo::Log::Level::Warn, .name = "asset.retry", .message = "retrying"}}));
    REQUIRE(AddEventually(counter, 3));
    REQUIRE(EmitEventually({.subsystem = "assets.import",
                            .context = context,
                            .payload = Horo::Telemetry::SpanRecord{.operationId = 12,
                                                                   .parentOperationId = 7,
                                                                   .name = "asset.import",
                                                                   .status = Horo::Telemetry::SpanStatus::Failed,
                                                                   .duration = std::chrono::milliseconds{4}}}));
    REQUIRE(Horo::Telemetry::Runtime::Shutdown());

    const std::vector<TransportCall> calls = transport->Snapshot();
    REQUIRE(calls.size() == 3);
    for (const TransportCall &call : calls)
        REQUIRE(call.threadId != producerThread);

    const nlohmann::json logs = nlohmann::json::parse(FindCall(calls, Horo::Telemetry::OtlpSignal::Logs).payload);
    const auto &logRecords = logs.at("resourceLogs").at(0).at("scopeLogs").at(0).at("logRecords");
    REQUIRE(logRecords.size() == 2);
    REQUIRE(logRecords.at(0).at("severityText") == "error");
    REQUIRE(logRecords.at(0).at("body").at("stringValue") == "import failed");
    const auto redacted = FindAttribute(logRecords.at(0).at("attributes"), "auth.token");
    REQUIRE(redacted.has_value());
    REQUIRE(redacted->at("stringValue") == "[REDACTED]");
    const auto threadId = FindAttribute(logRecords.at(0).at("attributes"), "thread.id");
    REQUIRE(threadId.has_value());
    REQUIRE(threadId->contains("stringValue"));
    REQUIRE_FALSE(threadId->contains("intValue"));
    REQUIRE(FindAttribute(logRecords.at(0).at("attributes"), "asset.id")->at("stringValue") == "mesh-7");

    const nlohmann::json metrics = nlohmann::json::parse(FindCall(calls, Horo::Telemetry::OtlpSignal::Metrics).payload);
    const auto &metric = metrics.at("resourceMetrics").at(0).at("scopeMetrics").at(0).at("metrics").at(0);
    REQUIRE(metric.at("name") == "jobs.completed");
    REQUIRE(metric.at("sum").at("isMonotonic") == true);
    REQUIRE(metric.at("sum").at("dataPoints").at(0).at("asDouble") == 3.0);
    REQUIRE(FindAttribute(metric.at("sum").at("dataPoints").at(0).at("attributes"), "result")->at("stringValue") == "ok");

    const nlohmann::json traces = nlohmann::json::parse(FindCall(calls, Horo::Telemetry::OtlpSignal::Traces).payload);
    const auto &span = traces.at("resourceSpans").at(0).at("scopeSpans").at(0).at("spans").at(0);
    REQUIRE(span.at("traceId").get<std::string>().size() == 32);
    REQUIRE(span.at("spanId") == "000000000000000c");
    REQUIRE(span.at("parentSpanId") == "0000000000000007");
    REQUIRE(span.at("status").at("code") == 2);

    const auto statistics = sink->Statistics();
    REQUIRE(statistics.acceptedRecords == 4);
    REQUIRE(statistics.exportedRecords == 4);
    REQUIRE(statistics.droppedRecords == 0);
    REQUIRE(statistics.redactedAttributes >= 3);
}

TEST_CASE("OpenTelemetry flushes sparse metrics without filling a batch", "[foundation][observability][opentelemetry][metrics][flush]") {
    RuntimeGuard runtimeGuard;
    auto transport = std::make_shared<CapturingTransport>();
    Horo::Telemetry::OpenTelemetryConfiguration configuration;
    configuration.maxBatchRecords = 32;
    configuration.maxBufferedRecords = 32;
    configuration.maxAttempts = 1;
    configuration.exportApproved = true;
    const auto sink = Horo::Telemetry::OpenTelemetrySink::Create(configuration, transport);
    REQUIRE(sink != nullptr);
    REQUIRE(Horo::Telemetry::Runtime::Initialize({.queueCapacity = 16, .sinkFlushInterval = std::chrono::milliseconds{5}, .enabled = true},
                                                 sink));

    const auto gauge = Horo::Telemetry::Runtime::RegisterGauge(
        {.name = "horo.test.sparse", .subsystem = "foundation.tests", .unit = Horo::Telemetry::MetricUnit::Count});
    REQUIRE(gauge);
    const std::uint64_t acceptedBefore = Horo::Telemetry::Runtime::GetStatistics().acceptedRecords;
    for (int attempt = 0; attempt < 10'000 && Horo::Telemetry::Runtime::GetStatistics().acceptedRecords == acceptedBefore; ++attempt) {
        gauge.Set(1.0);
        std::this_thread::yield();
    }
    REQUIRE(Horo::Telemetry::Runtime::GetStatistics().acceptedRecords > acceptedBefore);

    bool exported = false;
    for (int attempt = 0; attempt < 100 && !exported; ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds{2});
        exported = !transport->Snapshot().empty();
    }
    REQUIRE(exported);
    REQUIRE(Horo::Telemetry::Runtime::Shutdown());

    const auto calls = transport->Snapshot();
    REQUIRE(calls.size() == 1);
    REQUIRE(calls.front().signal == Horo::Telemetry::OtlpSignal::Metrics);
}

TEST_CASE("OpenTelemetry export requires explicit approval and safe endpoint policy",
          "[foundation][observability][opentelemetry][policy]") {
    auto transport = std::make_shared<CapturingTransport>();
    Horo::Telemetry::OpenTelemetryConfiguration configuration;
    REQUIRE(Horo::Telemetry::OpenTelemetrySink::Create(configuration, transport) == nullptr);

    configuration.exportApproved = true;
    REQUIRE(Horo::Telemetry::OpenTelemetrySink::Create(configuration, transport) != nullptr);

    configuration.endpoint = "http://collector.example.com:4318";
    REQUIRE(Horo::Telemetry::OpenTelemetrySink::Create(configuration, transport) == nullptr);

    configuration.endpoint = "http://127.0.0.1:4318";
    REQUIRE(Horo::Telemetry::OpenTelemetrySink::Create(configuration, transport) == nullptr);
    configuration.allowInsecureLocalhost = true;
    REQUIRE(Horo::Telemetry::OpenTelemetrySink::Create(configuration, transport) != nullptr);

    configuration.allowInsecureLocalhost = false;
    configuration.endpoint = "https://collector.example.com";
    REQUIRE(Horo::Telemetry::OpenTelemetrySink::Create(configuration, transport) != nullptr);

    configuration.headers = {{.name = "Authorization\r\nInjected", .value = "bad"}};
    REQUIRE(Horo::Telemetry::OpenTelemetrySink::Create(configuration, transport) == nullptr);

    configuration.headers.clear();
    configuration.maxAttempts = 9;
    REQUIRE(Horo::Telemetry::OpenTelemetrySink::Create(configuration, transport) == nullptr);
}

TEST_CASE("OpenTelemetry preserves counter gauge histogram and timing signal shapes",
          "[foundation][observability][opentelemetry][metrics]") {
    RuntimeGuard runtimeGuard;
    auto transport = std::make_shared<CapturingTransport>();
    Horo::Telemetry::OpenTelemetryConfiguration configuration;
    configuration.maxBatchRecords = 5;
    configuration.maxBufferedRecords = 5;
    configuration.maxAttempts = 1;
    configuration.exportApproved = true;
    const auto sink = Horo::Telemetry::OpenTelemetrySink::Create(configuration, transport);
    REQUIRE(sink != nullptr);
    REQUIRE(Horo::Telemetry::Runtime::Initialize({.queueCapacity = 16, .enabled = true}, sink));

    const auto counter = Horo::Telemetry::Runtime::RegisterCounter(
        {.name = "test.counter", .subsystem = "Foundation.Tests", .unit = Horo::Telemetry::MetricUnit::Count});
    const auto gauge = Horo::Telemetry::Runtime::RegisterGauge(
        {.name = "test.gauge", .subsystem = "Foundation.Tests", .unit = Horo::Telemetry::MetricUnit::Count});
    const auto histogram = Horo::Telemetry::Runtime::RegisterHistogram(
        {.name = "test.histogram", .subsystem = "Foundation.Tests", .unit = Horo::Telemetry::MetricUnit::Seconds});
    const auto timing = Horo::Telemetry::Runtime::RegisterTiming(
        {.name = "test.timing", .subsystem = "Foundation.Tests", .unit = Horo::Telemetry::MetricUnit::Count});
    REQUIRE(counter);
    REQUIRE(gauge);
    REQUIRE(histogram);
    REQUIRE(timing);
    REQUIRE(AddEventually(counter, 2));
    REQUIRE(AddEventually(counter, 3));

    auto recordInstrument = [](const auto &instrument, const auto update) {
        const auto acceptedBefore = Horo::Telemetry::Runtime::GetStatistics().acceptedRecords;
        for (int attempt = 0; attempt < 10'000; ++attempt) {
            update(instrument);
            if (Horo::Telemetry::Runtime::GetStatistics().acceptedRecords > acceptedBefore)
                return true;
            std::this_thread::yield();
        }
        return false;
    };
    REQUIRE(recordInstrument(gauge, [](const auto &value) {
        value.Set(4.5);
    }));
    REQUIRE(recordInstrument(histogram, [](const auto &value) {
        value.Observe(7.5);
    }));
    REQUIRE(recordInstrument(timing, [](const auto &value) {
        value.Record(std::chrono::milliseconds{12});
    }));
    REQUIRE(Horo::Telemetry::Runtime::Shutdown());

    const auto calls = transport->Snapshot();
    REQUIRE(calls.size() == 1);
    const nlohmann::json payload = nlohmann::json::parse(calls.front().payload);
    const auto &metrics = payload.at("resourceMetrics").at(0).at("scopeMetrics").at(0).at("metrics");
    REQUIRE(metrics.size() == 4);
    REQUIRE(metrics.at(0).contains("sum"));
    REQUIRE(metrics.at(0).at("sum").at("dataPoints").size() == 2);
    REQUIRE(metrics.at(1).contains("gauge"));
    REQUIRE(metrics.at(2).contains("histogram"));
    REQUIRE(metrics.at(2).at("histogram").at("dataPoints").at(0).at("count") == "1");
    REQUIRE(metrics.at(3).contains("histogram"));
    REQUIRE(metrics.at(3).at("unit") == "s");
    REQUIRE(metrics.at(3).at("histogram").at("dataPoints").at(0).at("sum") == 0.012);
}

TEST_CASE("OpenTelemetry retries are bounded and exporter failure remains isolated from producers",
          "[foundation][observability][opentelemetry][retry][failure]") {
    RuntimeGuard runtimeGuard;
    auto transport = std::make_shared<CapturingTransport>();
    transport->outcomes = {false, true};
    Horo::Telemetry::OpenTelemetryConfiguration configuration;
    configuration.maxBatchRecords = 1;
    configuration.maxBufferedRecords = 1;
    configuration.maxAttempts = 2;
    configuration.retryDelay = std::chrono::milliseconds::zero();
    configuration.exportApproved = true;
    const auto sink = Horo::Telemetry::OpenTelemetrySink::Create(configuration, transport);
    REQUIRE(sink != nullptr);
    REQUIRE(Horo::Telemetry::Runtime::Initialize({.queueCapacity = 8, .enabled = true}, sink));
    REQUIRE(EmitEventually(
        {.subsystem = "foundation.tests",
         .payload = Horo::Telemetry::DiagnosticEvent{.severity = Horo::Log::Level::Info, .name = "retry.test", .message = "retry"}}));
    REQUIRE(Horo::Telemetry::Runtime::Shutdown());
    REQUIRE(transport->Snapshot().size() == 2);
    REQUIRE(sink->Statistics().exportedRecords == 1);
    REQUIRE(sink->Statistics().retryAttempts == 1);

    transport = std::make_shared<CapturingTransport>();
    transport->defaultOutcome = false;
    configuration.maxBatchRecords = 8;
    configuration.maxBufferedRecords = 8;
    configuration.maxAttempts = 1;
    const auto failingSink = Horo::Telemetry::OpenTelemetrySink::Create(configuration, transport);
    REQUIRE(failingSink != nullptr);
    REQUIRE(Horo::Telemetry::Runtime::Initialize({.queueCapacity = 8, .enabled = true}, failingSink));
    REQUIRE(EmitEventually(
        {.subsystem = "foundation.tests",
         .payload = Horo::Telemetry::DiagnosticEvent{.severity = Horo::Log::Level::Error, .name = "offline.test", .message = "offline"}}));
    REQUIRE_FALSE(Horo::Telemetry::Runtime::Shutdown());
    REQUIRE(failingSink->Statistics().failedBatches == 1);
    REQUIRE(failingSink->Statistics().droppedRecords == 1);
}

TEST_CASE("OpenTelemetry reports direct export failures with its dedicated exception type",
          "[foundation][observability][opentelemetry][failure][exception]") {
    auto transport = std::make_shared<CapturingTransport>();
    transport->defaultOutcome = false;
    Horo::Telemetry::OpenTelemetryConfiguration configuration;
    configuration.maxBatchRecords = 1;
    configuration.maxBufferedRecords = 1;
    configuration.maxAttempts = 1;
    configuration.exportApproved = true;
    const auto sink = Horo::Telemetry::OpenTelemetrySink::Create(configuration, transport);
    REQUIRE(sink != nullptr);

    const Horo::Telemetry::Record record{.timestampUtc = std::chrono::system_clock::now(),
                                         .subsystem = "foundation.tests",
                                         .payload = Horo::Telemetry::DiagnosticEvent{.severity = Horo::Log::Level::Error,
                                                                                     .name = "offline.test",
                                                                                     .message = "offline"}};
    REQUIRE_THROWS_AS(sink->Export(record, nullptr), Horo::Telemetry::OpenTelemetryExportError);
}
