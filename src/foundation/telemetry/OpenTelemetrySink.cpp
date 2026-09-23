#include "Horo/Foundation/Telemetry/OpenTelemetrySink.h"

#include <algorithm>
#include <atomic>
#include <curl/curl.h>
#include <format>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace Horo::Telemetry {
    namespace {
        [[nodiscard]] std::string_view MetricUnitSymbol(const MetricUnit unit) noexcept {
            switch (unit) {
                case MetricUnit::Count:
                case MetricUnit::Ratio:
                    return "1";
                case MetricUnit::Bytes:
                    return "By";
                case MetricUnit::Seconds:
                    return "s";
            }
            return {};
        }

        using Json = nlohmann::json;

        template <typename... Visitors> struct Overloaded : Visitors... {
            using Visitors::operator()...;
        };

        class CurlOtlpTransport final : public IOtlpTransport {
        public:
            explicit CurlOtlpTransport(std::string endpoint, std::vector<OtlpHttpHeader> headers)
                : endpoint_(std::move(endpoint)), headers_(std::move(headers)) {
                while (!endpoint_.empty() && endpoint_.back() == '/')
                    endpoint_.pop_back();
            }

            bool Post(const OtlpSignal signal, const std::string_view jsonPayload, const std::chrono::milliseconds timeout) override {
                static std::once_flag curlInitialization;
                static bool curlReady{};
                std::call_once(curlInitialization, [] {
                    // Global setup does not negotiate TLS; the request below requires TLS 1.3.
                    curlReady = curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK;  // NOSONAR(cpp:S4423)
                });
                if (!curlReady)
                    return false;
                CURL *curl = curl_easy_init();  // NOSONAR(cpp:S4423) TLS 1.3 is required before curl_easy_perform().
                if (curl == nullptr)
                    return false;
                const char *suffix{};
                switch (signal) {
                    case OtlpSignal::Logs:
                        suffix = "/v1/logs";
                        break;
                    case OtlpSignal::Metrics:
                        suffix = "/v1/metrics";
                        break;
                    case OtlpSignal::Traces:
                        suffix = "/v1/traces";
                        break;
                }
                const std::string url = endpoint_ + suffix;
                curl_slist *headers = curl_slist_append(nullptr, "Content-Type: application/json");
                if (headers == nullptr) {
                    curl_easy_cleanup(curl);
                    return false;
                }
                for (const OtlpHttpHeader &header : headers_) {
                    const std::string line = header.name + ": " + header.value;
                    curl_slist *appended = curl_slist_append(headers, line.c_str());
                    if (appended == nullptr) {
                        curl_slist_free_all(headers);
                        curl_easy_cleanup(curl);
                        return false;
                    }
                    headers = appended;
                }
                curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
                curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
                curl_easy_setopt(curl, CURLOPT_POST, 1L);
                curl_easy_setopt(curl, CURLOPT_POSTFIELDS, jsonPayload.data());
                curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(jsonPayload.size()));
                curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, static_cast<long>(timeout.count()));
                curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
                curl_easy_setopt(curl, CURLOPT_SSLVERSION, CURL_SSLVERSION_TLSv1_3);  // NOSONAR(cpp:S4423) Require modern TLS.
                const CURLcode result = curl_easy_perform(curl);

                long responseCode{};
                curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &responseCode);
                curl_slist_free_all(headers);
                curl_easy_cleanup(curl);
                return result == CURLE_OK && responseCode >= 200 && responseCode < 300;
            }

        private:
            std::string endpoint_;
            std::vector<OtlpHttpHeader> headers_;
        };

        [[nodiscard]] bool HasForbiddenHeaderCharacter(const std::string_view value) noexcept {
            return value.find_first_of("\r\n") != std::string_view::npos;
        }

        [[nodiscard]] bool MatchesAuthority(const std::string_view endpoint, const std::string_view prefix) noexcept {
            if (!endpoint.starts_with(prefix))
                return false;
            if (endpoint.size() == prefix.size())
                return true;
            const char delimiter = endpoint[prefix.size()];
            return delimiter == ':' || delimiter == '/';
        }

        [[nodiscard]] bool IsAllowedEndpoint(const OpenTelemetryConfiguration &configuration) noexcept {
            constexpr std::string_view kInsecureIpv4Loopback{"http://127.0.0.1"};  // NOSONAR: Explicit opt-in loopback transport.
            constexpr std::string_view kInsecureLocalhost{"http://localhost"};     // NOSONAR: Explicit opt-in loopback transport.
            constexpr std::string_view kInsecureIpv6Loopback{"http://[::1]"};      // NOSONAR: Explicit opt-in loopback transport.
            if (configuration.endpoint.starts_with("https://"))
                return configuration.endpoint.size() > std::string_view{"https://"}.size();
            if (!configuration.allowInsecureLocalhost)
                return false;
            return MatchesAuthority(configuration.endpoint, kInsecureIpv4Loopback) ||
                   MatchesAuthority(configuration.endpoint, kInsecureLocalhost) ||
                   MatchesAuthority(configuration.endpoint, kInsecureIpv6Loopback);
        }

        [[nodiscard]] bool IsValidConfiguration(const OpenTelemetryConfiguration &configuration) noexcept {
            constexpr std::size_t kMaximumBatchRecords = 4096;
            constexpr std::size_t kMaximumBufferedRecords = 65'536;
            constexpr std::size_t kMaximumPayloadBytes = 16U * 1024U * 1024U;
            constexpr std::size_t kMaximumHeaders = 32;
            constexpr std::size_t kMaximumHeaderBytes = 8U * 1024U;
            if (!configuration.exportApproved || configuration.serviceName.empty() || !IsAllowedEndpoint(configuration) ||
                configuration.endpoint.size() > 2048 || configuration.serviceName.size() > 256 || configuration.maxBatchRecords == 0 ||
                configuration.maxBatchRecords > kMaximumBatchRecords || configuration.maxBufferedRecords < configuration.maxBatchRecords ||
                configuration.maxBufferedRecords > kMaximumBufferedRecords || configuration.maxPayloadBytes == 0 ||
                configuration.maxPayloadBytes > kMaximumPayloadBytes || configuration.maxAttempts == 0 || configuration.maxAttempts > 8 ||
                configuration.requestTimeout <= std::chrono::milliseconds::zero() ||
                configuration.requestTimeout > std::chrono::seconds{30} || configuration.retryDelay < std::chrono::milliseconds::zero() ||
                configuration.retryDelay > std::chrono::seconds{1} || configuration.headers.size() > kMaximumHeaders)
                return false;
            if (constexpr std::size_t kMaximumRedactionFragments = 32;
                configuration.redactedAttributeKeyFragments.size() > kMaximumRedactionFragments)
                return false;
            const bool headersAreValid = std::ranges::all_of(configuration.headers, [](const OtlpHttpHeader &header) {
                return !header.name.empty() && !HasForbiddenHeaderCharacter(header.name) && !HasForbiddenHeaderCharacter(header.value) &&
                       header.name.find(':') == std::string::npos && header.name.size() + header.value.size() <= kMaximumHeaderBytes;
            });
            const bool redactionFragmentsAreValid =
                std::ranges::all_of(configuration.redactedAttributeKeyFragments, [](const std::string_view fragment) {
                return !fragment.empty() && fragment.size() <= 128;
            });
            return headersAreValid && redactionFragmentsAreValid;
        }

        template <typename Duration>
        [[nodiscard]] std::string TimeUnixNanos(const std::chrono::time_point<std::chrono::system_clock, Duration> timestamp) {
            return std::to_string(std::chrono::duration_cast<std::chrono::nanoseconds>(timestamp.time_since_epoch()).count());
        }

        [[nodiscard]] std::string HexId(const std::uint64_t value, const bool trace) {
            if (trace)
                return std::format("{:016x}{:016x}", value, value ^ 0x9e3779b97f4a7c15ULL);
            return std::format("{:016x}", value);
        }

        [[nodiscard]] std::uint64_t HashId(const std::string_view value) noexcept {
            std::uint64_t result = 1469598103934665603ULL;
            for (const unsigned char character : value) {
                result ^= character;
                result *= 1099511628211ULL;
            }
            return result == 0 ? 1 : result;
        }

        [[nodiscard]] std::optional<std::string_view> ContextValue(const Record &record, const std::string_view key) noexcept {
            for (const auto &[candidate, value] : record.context.Fields()) {
                if (candidate == key)
                    return value;
            }
            return std::nullopt;
        }

        [[nodiscard]] std::string TraceId(const Record &record, const std::uint64_t fallback) {
            const auto correlation = ContextValue(record, "correlation.id");
            return HexId(correlation ? HashId(*correlation) : fallback, true);
        }

        [[nodiscard]] Json OtlpValue(const FieldValue &value) {
            return std::visit([]<typename Value>(const Value &typed) -> Json {
                if constexpr (std::is_same_v<Value, bool>)
                    return {{"boolValue", typed}};
                else if constexpr (std::is_same_v<Value, std::string>)
                    return {{"stringValue", typed}};
                else if constexpr (std::is_floating_point_v<Value>)
                    return {{"doubleValue", typed}};
                else
                    return {{"intValue", std::to_string(typed)}};
            }, value);
        }

        void AppendAttribute(Json &attributes, const std::string_view key, Json value) {
            attributes.push_back({{"key", key}, {"value", std::move(value)}});
        }

        [[nodiscard]] std::string Lowercase(std::string_view value) {
            std::string result{value};
            for (char &character : result) {
                if (character >= 'A' && character <= 'Z')
                    character = character - 'A' + 'a';
            }
            return result;
        }

        [[nodiscard]] bool IsSensitiveKey(const std::string_view key, const OpenTelemetryConfiguration &configuration) {
            const std::string normalized = Lowercase(key);
            return std::ranges::any_of(configuration.redactedAttributeKeyFragments, [&](const std::string_view fragment) {
                return normalized.find(Lowercase(fragment)) != std::string::npos;
            });
        }

        void AppendConfiguredAttribute(Json &attributes, const std::string_view key, Json value,
                                       const OpenTelemetryConfiguration &configuration, std::atomic<std::uint64_t> &redactedAttributes) {
            if (IsSensitiveKey(key, configuration)) {
                value = {{"stringValue", "[REDACTED]"}};
                redactedAttributes.fetch_add(1);
            }
            AppendAttribute(attributes, key, std::move(value));
        }

        [[nodiscard]] Json CommonAttributes(const Record &record, const OpenTelemetryConfiguration &configuration,
                                            std::atomic<std::uint64_t> &redactedAttributes) {
            Json attributes = Json::array();
            AppendAttribute(attributes, "horo.subsystem", {{"stringValue", record.subsystem}});
            AppendAttribute(attributes, "thread.id", {{"stringValue", std::to_string(record.threadId)}});
            for (const auto &[key, value] : record.context.Fields())
                AppendConfiguredAttribute(attributes, key, {{"stringValue", value}}, configuration, redactedAttributes);
            return attributes;
        }

        void AppendFields(Json &attributes, const std::span<const Field> fields, const OpenTelemetryConfiguration &configuration,
                          std::atomic<std::uint64_t> &redactedAttributes) {
            for (const Field &field : fields)
                AppendConfiguredAttribute(attributes, field.key, OtlpValue(field.value), configuration, redactedAttributes);
        }

        [[nodiscard]] int SeverityNumber(const Log::Level level) noexcept {
            using enum Log::Level;
            switch (level) {
                case Trace:
                    return 1;
                case Debug:
                    return 5;
                case Info:
                    return 9;
                case Warn:
                    return 13;
                case Error:
                    return 17;
                case Critical:
                    return 21;
                case Off:
                    return 0;
            }
            return 0;
        }

        [[nodiscard]] Json Resource(const std::string &serviceName) {
            return {{"attributes", Json::array({{{"key", "service.name"}, {"value", {{"stringValue", serviceName}}}}})}};
        }
    }  // namespace

    class OpenTelemetrySink::Impl {
    public:
        Impl(OpenTelemetryConfiguration value, std::shared_ptr<IOtlpTransport> selectedTransport)
            : configuration_(std::move(value)), transport_(std::move(selectedTransport)) {
            pending_.reserve(configuration_.maxBufferedRecords);
        }

        void Export(const Record &record, const InstrumentDescriptor *descriptor) {
            std::lock_guard lock(mutex_);
            if (pending_.size() >= configuration_.maxBufferedRecords) {
                droppedRecords_.fetch_add(1);
                return;
            }
            std::optional<InstrumentDescriptor> ownedDescriptor;
            if (descriptor != nullptr)
                ownedDescriptor = *descriptor;
            pending_.push_back({.record = record, .descriptor = std::move(ownedDescriptor)});
            acceptedRecords_.fetch_add(1);
            if (pending_.size() >= configuration_.maxBatchRecords && !ExportBatch())
                throw OpenTelemetryExportError{"OTLP batch export failed"};
        }

        void Flush() {
            std::lock_guard lock(mutex_);
            if (!ExportBatch())
                throw OpenTelemetryExportError{"OTLP flush failed"};
        }

        [[nodiscard]] OpenTelemetryStatistics Statistics() const noexcept {
            return {.acceptedRecords = acceptedRecords_.load(),
                    .exportedRecords = exportedRecords_.load(),
                    .droppedRecords = droppedRecords_.load(),
                    .failedBatches = failedBatches_.load(),
                    .retryAttempts = retryAttempts_.load(),
                    .redactedAttributes = redactedAttributes_.load()};
        }

    private:
        struct Pending {
            Record record;
            std::optional<InstrumentDescriptor> descriptor;
        };

        struct Batch {
            Json logs = Json::array();
            Json metrics = Json::array();
            Json spans = Json::array();
            std::size_t logCount{};
            std::size_t metricCount{};
            std::size_t spanCount{};
            std::size_t unmappedCount{};
        };

        [[nodiscard]] bool PostPayload(const OtlpSignal signal, const Json &payload, const std::uint64_t recordCount) {
            const std::string serialized = payload.dump();
            if (serialized.size() > configuration_.maxPayloadBytes) {
                droppedRecords_.fetch_add(recordCount);
                return false;
            }
            for (std::uint32_t attempt = 0; attempt < configuration_.maxAttempts; ++attempt) {
                if (transport_->Post(signal, serialized, configuration_.requestTimeout)) {
                    exportedRecords_.fetch_add(recordCount);
                    return true;
                }
                if (attempt + 1U >= configuration_.maxAttempts)
                    continue;
                retryAttempts_.fetch_add(1);
                if (configuration_.retryDelay > std::chrono::milliseconds::zero())
                    std::this_thread::sleep_for(configuration_.retryDelay);
            }
            droppedRecords_.fetch_add(recordCount);
            return false;
        }

        void MapLog(const Pending &item, const LogRecord &log, Json attributes, Batch &batch) {
            AppendAttribute(attributes, "log.category", {{"stringValue", log.category}});
            AppendFields(attributes, log.fields, configuration_, redactedAttributes_);
            batch.logs.push_back({{"timeUnixNano", TimeUnixNanos(item.record.timestampUtc)},
                                  {"severityNumber", SeverityNumber(log.severity)},
                                  {"severityText", Log::ToString(log.severity)},
                                  {"body", {{"stringValue", log.message}}},
                                  {"attributes", std::move(attributes)}});
            ++batch.logCount;
        }

        void MapEvent(const Pending &item, const DiagnosticEvent &event, Json attributes, Batch &batch) {
            AppendAttribute(attributes, "event.name", {{"stringValue", event.name}});
            AppendFields(attributes, event.fields, configuration_, redactedAttributes_);
            batch.logs.push_back({{"timeUnixNano", TimeUnixNanos(item.record.timestampUtc)},
                                  {"severityNumber", SeverityNumber(event.severity)},
                                  {"severityText", Log::ToString(event.severity)},
                                  {"body", {{"stringValue", event.message}}},
                                  {"attributes", std::move(attributes)}});
            ++batch.logCount;
        }

        void AppendMetricDimensions(Json &attributes, const MetricRecord &metric, const InstrumentDescriptor &descriptor) {
            const std::size_t dimensionCount = std::min<std::size_t>(metric.dimensionCount, descriptor.dimensions.size());
            for (std::size_t index = 0; index < dimensionCount; ++index) {
                const std::uint16_t valueId = metric.dimensionValueIds[index];
                if (valueId == 0 || valueId > descriptor.dimensions[index].allowedValues.size())
                    continue;
                AppendConfiguredAttribute(attributes, descriptor.dimensions[index].key,
                                          {{"stringValue", descriptor.dimensions[index].allowedValues[valueId - 1U]}}, configuration_,
                                          redactedAttributes_);
            }
        }

        [[nodiscard]] static const char *MetricSignalKey(const InstrumentKind kind) noexcept {
            using enum InstrumentKind;
            switch (kind) {
                case Counter:
                    return "sum";
                case Gauge:
                    return "gauge";
                case Histogram:
                case Timing:
                    return "histogram";
            }
            return "histogram";
        }

        [[nodiscard]] static Json NewMetric(const MetricRecord &metric, const InstrumentDescriptor &descriptor, const char *signalKey,
                                            Json point) {
            using enum InstrumentKind;
            Json data;
            switch (metric.kind) {
                case Counter:
                    data = {{signalKey,
                             {{"aggregationTemporality", 1}, {"isMonotonic", true}, {"dataPoints", Json::array({std::move(point)})}}}};
                    break;
                case Gauge:
                    data = {{signalKey, {{"dataPoints", Json::array({std::move(point)})}}}};
                    break;
                case Histogram:
                case Timing:
                    data = {{signalKey, {{"aggregationTemporality", 1}, {"dataPoints", Json::array({std::move(point)})}}}};
                    break;
            }
            data["name"] = descriptor.name;
            data["unit"] = MetricUnitSymbol(descriptor.unit);
            return data;
        }

        void MapMetric(const Pending &item, const MetricRecord &metric, Json attributes, Batch &batch) {
            using enum InstrumentKind;
            if (!item.descriptor) {
                ++batch.unmappedCount;
                return;
            }
            const InstrumentDescriptor &descriptor = *item.descriptor;
            AppendMetricDimensions(attributes, metric, descriptor);
            Json point{{"timeUnixNano", TimeUnixNanos(item.record.timestampUtc)},
                       {"asDouble", metric.value},
                       {"attributes", std::move(attributes)}};
            const char *signalKey = MetricSignalKey(metric.kind);
            if (metric.kind == Histogram || metric.kind == Timing) {
                point.erase("asDouble");
                point["count"] = "1";
                point["sum"] = metric.value;
            }
            if (const auto existing = std::ranges::find_if(batch.metrics,
                                                           [&](const Json &candidate) {
                return candidate.value("name", "") == descriptor.name && candidate.contains(signalKey);
            });
                existing != batch.metrics.end()) {
                (*existing)[signalKey]["dataPoints"].push_back(std::move(point));
            } else {
                batch.metrics.push_back(NewMetric(metric, descriptor, signalKey, std::move(point)));
            }
            ++batch.metricCount;
        }

        void MapSpan(const Pending &item, const SpanRecord &span, Json attributes, Batch &batch) {
            AppendFields(attributes, span.fields, configuration_, redactedAttributes_);
            const auto started = item.record.timestampUtc - span.duration;
            int statusCode = 2;
            if (span.status == SpanStatus::Succeeded)
                statusCode = 1;
            else if (span.status == SpanStatus::Unset)
                statusCode = 0;
            std::string parentSpanId;
            if (span.parentOperationId != 0)
                parentSpanId = HexId(span.parentOperationId, false);
            batch.spans.push_back({{"traceId", TraceId(item.record, span.operationId)},
                                   {"spanId", HexId(span.operationId, false)},
                                   {"parentSpanId", std::move(parentSpanId)},
                                   {"name", span.name},
                                   {"kind", 1},
                                   {"startTimeUnixNano", TimeUnixNanos(started)},
                                   {"endTimeUnixNano", TimeUnixNanos(item.record.timestampUtc)},
                                   {"attributes", std::move(attributes)},
                                   {"status", {{"code", statusCode}}}});
            ++batch.spanCount;
        }

        void MapPending(const Pending &item, Batch &batch) {
            Json attributes = CommonAttributes(item.record, configuration_, redactedAttributes_);
            std::visit(Overloaded{[this, &item, &attributes, &batch](const LogRecord &log) {
                MapLog(item, log, std::move(attributes), batch);
            },
                                  [this, &item, &attributes, &batch](const MetricRecord &metric) {
                MapMetric(item, metric, std::move(attributes), batch);
            },
                                  [this, &item, &attributes, &batch](const SpanRecord &span) {
                MapSpan(item, span, std::move(attributes), batch);
            },
                                  [this, &item, &attributes, &batch](const DiagnosticEvent &event) {
                MapEvent(item, event, std::move(attributes), batch);
            }},
                       item.record.payload);
        }

        [[nodiscard]] bool ExportLogs(Batch &batch) {
            if (batch.logs.empty())
                return true;
            const Json payload{
                {"resourceLogs",
                 Json::array({{{"resource", Resource(configuration_.serviceName)},
                               {"scopeLogs", Json::array({{{"scope", {{"name", "horo"}}}, {"logRecords", std::move(batch.logs)}}})}}})}};
            return PostPayload(OtlpSignal::Logs, payload, batch.logCount);
        }

        [[nodiscard]] bool ExportMetrics(Batch &batch) {
            if (batch.metrics.empty())
                return true;
            const Json payload{
                {"resourceMetrics",
                 Json::array({{{"resource", Resource(configuration_.serviceName)},
                               {"scopeMetrics", Json::array({{{"scope", {{"name", "horo"}}}, {"metrics", std::move(batch.metrics)}}})}}})}};
            return PostPayload(OtlpSignal::Metrics, payload, batch.metricCount);
        }

        [[nodiscard]] bool ExportSpans(Batch &batch) {
            if (batch.spans.empty())
                return true;
            const Json payload{
                {"resourceSpans",
                 Json::array({{{"resource", Resource(configuration_.serviceName)},
                               {"scopeSpans", Json::array({{{"scope", {{"name", "horo"}}}, {"spans", std::move(batch.spans)}}})}}})}};
            return PostPayload(OtlpSignal::Traces, payload, batch.spanCount);
        }

        [[nodiscard]] bool ExportBatch() {
            if (pending_.empty())
                return true;
            Batch batch;
            for (const Pending &item : pending_)
                MapPending(item, batch);

            bool success = true;
            success = ExportLogs(batch) && success;
            success = ExportMetrics(batch) && success;
            success = ExportSpans(batch) && success;
            if (batch.unmappedCount != 0) {
                droppedRecords_.fetch_add(batch.unmappedCount);
                success = false;
            }
            if (!success)
                failedBatches_.fetch_add(1);
            pending_.clear();
            return success;
        }

        OpenTelemetryConfiguration configuration_;
        std::shared_ptr<IOtlpTransport> transport_;
        mutable std::mutex mutex_;
        std::vector<Pending> pending_;
        std::atomic<std::uint64_t> acceptedRecords_{};
        std::atomic<std::uint64_t> exportedRecords_{};
        std::atomic<std::uint64_t> droppedRecords_{};
        std::atomic<std::uint64_t> failedBatches_{};
        std::atomic<std::uint64_t> retryAttempts_{};
        std::atomic<std::uint64_t> redactedAttributes_{};
    };

    /** @copydoc OpenTelemetrySink::Create */
    std::shared_ptr<OpenTelemetrySink> OpenTelemetrySink::Create(const OpenTelemetryConfiguration &configuration) noexcept {
        if (!IsValidConfiguration(configuration))
            return nullptr;
        try {
            return Create(configuration, std::make_shared<CurlOtlpTransport>(configuration.endpoint, configuration.headers));
        } catch (...) {
            return nullptr;
        }
    }

    /** @copydoc OpenTelemetrySink::Create */
    std::shared_ptr<OpenTelemetrySink> OpenTelemetrySink::Create(const OpenTelemetryConfiguration &configuration,
                                                                 std::shared_ptr<IOtlpTransport> transport) noexcept {
        if (!IsValidConfiguration(configuration) || transport == nullptr)
            return nullptr;
        try {
            return std::make_shared<OpenTelemetrySink>(ConstructionKey{}, configuration, std::move(transport));
        } catch (...) {
            return nullptr;
        }
    }

    /** @copydoc OpenTelemetrySink::OpenTelemetrySink */
    OpenTelemetrySink::OpenTelemetrySink(ConstructionKey, OpenTelemetryConfiguration configuration,
                                         std::shared_ptr<IOtlpTransport> transport)
        : impl_(std::make_unique<Impl>(std::move(configuration), std::move(transport))) {}

    /** @copydoc OpenTelemetrySink::~OpenTelemetrySink */
    OpenTelemetrySink::~OpenTelemetrySink() = default;

    /** @copydoc OpenTelemetrySink::Export */
    void OpenTelemetrySink::Export(const Record &record, const InstrumentDescriptor *descriptor) {
        impl_->Export(record, descriptor);
    }

    /** @copydoc OpenTelemetrySink::Flush */
    void OpenTelemetrySink::Flush() {
        impl_->Flush();
    }

    /** @copydoc OpenTelemetrySink::Statistics */
    OpenTelemetryStatistics OpenTelemetrySink::Statistics() const noexcept {
        return impl_->Statistics();
    }
}  // namespace Horo::Telemetry
