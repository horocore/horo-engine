#pragma once

/**
 * @file Telemetry.h
 * @brief Unified non-blocking observability records, sinks, and metric handles.
 */

#include "Horo/Foundation/Logging/LogContext.h"
#include "Horo/Foundation/Logging/LogLevel.h"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#ifndef HORO_ENABLE_TELEMETRY
#define HORO_ENABLE_TELEMETRY 1
#endif

namespace Horo::Telemetry {
    /** @brief Stable metric instrument kinds. */
    enum class InstrumentKind : std::uint8_t {
        Counter,
        Gauge,
        Histogram,
        Timing
    };

    /** @brief Canonical unit carried by a metric descriptor. */
    enum class MetricUnit : std::uint8_t {
        Count,
        Bytes,
        Seconds,
        Ratio
    };

    /** @brief Host-selected amount of metric instrumentation admitted at registration. */
    enum class MetricCollectionLevel : std::uint8_t {
        Off,
        Core,
        Detailed
    };

    /** @brief Stable kinds carried by the common observability envelope. */
    enum class RecordKind : std::uint8_t {
        Log,
        Metric,
        Span,
        Event
    };

    /** @brief Terminal state of a meaningful operation/span. */
    enum class SpanStatus : std::uint8_t {
        Unset,
        Succeeded,
        Failed,
        Cancelled,
        TimedOut
    };

    /** @brief Value types accepted by record-local structured fields. */
    using FieldValue = std::variant<bool, std::int64_t, std::uint64_t, double, std::string>;

    /** @brief Privacy policy applied before a field reaches any sink or in-memory store. */
    enum class FieldPrivacy : std::uint8_t {
        Public,
        SensitiveRedacted,
        Forbidden
    };

    /** @brief One record-local structured field that is not inherited as context. */
    struct Field {
        std::string key;
        FieldValue value;
        FieldPrivacy privacy{FieldPrivacy::Public};
    };

    /** @brief One allowlisted low-cardinality metric dimension. */
    struct DimensionDescriptor {
        std::string key;
        std::vector<std::string> allowedValues;
    };

    /** @brief Dimension key/value selected while binding a metric series. */
    struct DimensionValue {
        std::string_view key;
        std::string_view value;
    };

    /** @brief Hard host admission limits for descriptor text, dimensions, series, and registered instruments. */
    inline constexpr std::size_t MaximumMetricDimensions = 4;
    inline constexpr std::size_t MaximumMetricNameBytes = 96;
    inline constexpr std::size_t MaximumMetricSubsystemBytes = 64;
    inline constexpr std::size_t MaximumMetricDescriptionBytes = 256;
    inline constexpr std::size_t MaximumMetricDimensionKeyBytes = 48;
    inline constexpr std::size_t MaximumMetricDimensionValueBytes = 32;
    inline constexpr std::size_t MaximumMetricDimensionValues = 16;
    inline constexpr std::uint32_t MaximumMetricSeries = 256;
    inline constexpr std::size_t MaximumMetricInstruments = 256;

    /** @brief Stable pre-registered metric identity and unit. */
    struct InstrumentDescriptor {
        InstrumentKind kind{InstrumentKind::Counter};
        std::string name;
        std::string subsystem;
        MetricUnit unit{MetricUnit::Count}; /**< Canonical unit; arbitrary unit strings are not accepted. */
        std::string description;
        std::vector<DimensionDescriptor> dimensions;
        std::uint32_t maxSeries{1};
        MetricCollectionLevel minimumCollectionLevel{MetricCollectionLevel::Core}; /**< Minimum host metric policy required. */
    };

    /** @brief Structured log payload carried by a common record. */
    struct LogRecord {
        Log::Level severity{Log::Level::Info};
        std::string category;
        std::string message;
        std::vector<Field> fields;
    };

    /** @brief Metric observation payload carried by a common record. */
    struct MetricRecord {
        InstrumentKind kind{InstrumentKind::Counter};
        std::uint32_t instrumentId{};
        double value{};
        std::array<std::uint16_t, MaximumMetricDimensions> dimensionValueIds{};
        std::uint8_t dimensionCount{};
    };

    /** @brief Completed or state-bearing meaningful operation payload. */
    struct SpanRecord {
        std::uint64_t operationId{};
        std::uint64_t parentOperationId{};
        std::string name;
        SpanStatus status{SpanStatus::Unset};
        std::chrono::nanoseconds duration{};
        std::vector<Field> fields;
    };

    /** @brief Structured diagnostic event payload carried by a common record. */
    struct DiagnosticEvent {
        Log::Level severity{Log::Level::Info};
        std::string name;
        std::string message;
        std::vector<Field> fields;
    };

    /** @brief Type-safe payload shared by the unified observability envelope. */
    using RecordPayload = std::variant<LogRecord, MetricRecord, SpanRecord, DiagnosticEvent>;

    /**
     * @brief Immutable-at-delivery common envelope for every observability signal.
     *
     * Producers fill the payload, subsystem, and inherited context. The runtime
     * assigns sequence, wall/monotonic timestamps, and a stable process-local
     * thread identifier before the record becomes visible to sinks.
     */
    struct Record {
        std::uint64_t sequence{};
        std::chrono::system_clock::time_point timestampUtc;
        std::chrono::steady_clock::time_point monotonicTime;
        std::uint64_t threadId{};
        std::string subsystem;
        Log::LogContextSnapshot context;
        RecordPayload payload{MetricRecord{}};

        /**
         * @brief Returns the signal kind represented by the active payload.
         * @return Stable record kind without string inspection.
         */
        [[nodiscard]] RecordKind Kind() const noexcept;
    };

    /** @brief Backend-neutral consumer invoked only by the dispatcher thread. */
    class ISink {
    public:
        virtual ~ISink() = default;

        /**
         * @brief Consumes one immutable record on the dispatcher thread.
         * @param record Record valid for the duration of the call.
         * @param descriptor Metric descriptor, or null for non-metric records.
         */
        virtual void Export(const Record &record, const InstrumentDescriptor *descriptor) = 0;

        /** @brief Flushes backend-owned buffers on the dispatcher thread. */
        virtual void Flush() = 0;
    };

    /** @brief Explicit policy applied when the bounded queue is full. */
    enum class OverflowPolicy : std::uint8_t {
        DropNewest,
        DropOldest
    };

    /** @brief Telemetry queue, overflow, gating, and shutdown configuration. */
    struct Configuration {
        std::size_t queueCapacity{4096};
        OverflowPolicy overflowPolicy{OverflowPolicy::DropNewest};
        std::chrono::milliseconds shutdownTimeout{std::chrono::seconds{5}};
        std::chrono::milliseconds sinkFlushInterval{std::chrono::seconds{1}};
        Log::Level minimumEventSeverity{Log::Level::Trace};
        std::vector<std::string> subsystemPrefixes;
        MetricCollectionLevel metricCollectionLevel{MetricCollectionLevel::Core};
        bool enabled{true};
    };

    /** @brief Monotonic ingestion, delivery, failure, and lifecycle counters. */
    struct Statistics {
        std::uint64_t acceptedRecords{};
        std::uint64_t exportedRecords{};
        std::uint64_t droppedRecords{};
        std::uint64_t filteredRecords{};
        std::uint64_t contentionDrops{};
        std::uint64_t queueFullDrops{};
        std::uint64_t shutdownDrops{};
        std::uint64_t staleHandleDrops{};
        std::uint64_t sinkFailures{};
        std::uint64_t flushTimeouts{};
        std::uint64_t shutdownTimeouts{};
        std::uint64_t invalidInstrumentRegistrations{};
        std::uint64_t rejectedMetricSeries{};
    };

    /** @brief Runtime state for one registered metric, identified only by a process-local instrument index. */
    enum class MetricAvailabilityState : std::uint8_t {
        Available,
        TemporarilyUnavailable,
        UnsupportedByPlatform,
        UnsupportedByBackend,
        DisabledByProfile,
        PermissionDenied,
        SamplerFailed
    };

    /** @brief Privacy-safe availability row containing no descriptor text or producer payload. */
    struct MetricAvailabilityRecord {
        std::uint32_t instrumentId{}; /**< Process-local registration index; never an account or user identity. */
        MetricAvailabilityState state{MetricAvailabilityState::Available}; /**< Current host-reported state. */
    };

    /** @brief Fixed-capacity telemetry health and metric availability view for host diagnostics. */
    struct DiagnosticSnapshot {
        Statistics statistics; /**< Process-lifetime scalar telemetry health counters. */
        std::array<MetricAvailabilityRecord, MaximumMetricInstruments> availability{}; /**< Active metric state rows. */
        std::uint64_t availabilityRevision{}; /**< Advances when registration or availability changes. */
        std::uint32_t runtimeGeneration{};    /**< Identifies this process-local telemetry runtime. */
        std::uint16_t availabilityCount{};    /**< Number of populated availability rows. */
        bool runtimeEnabled{};                /**< Whether the runtime currently accepts records. */
    };

    class Counter;
    class Gauge;
    class Histogram;
    class Timing;

    /** @brief Process observability registry and bounded asynchronous dispatcher facade. */
    class Runtime final {
    public:
        Runtime(const Runtime &) = delete;
        Runtime &operator=(const Runtime &) = delete;

        /**
         * @brief Starts the process dispatcher with one optional sink.
         * @param configuration Queue, overflow, and lifecycle policy.
         * @param sink Process-owned sink; null installs a disabled runtime.
         * @return True when the requested configuration was installed.
         */
        [[nodiscard]] static bool Initialize(const Configuration &configuration, std::shared_ptr<ISink> sink);

        /**
         * @brief Starts the process dispatcher with multiple independent sinks.
         * @param configuration Queue, overflow, and lifecycle policy.
         * @param sinks Process-owned sinks invoked in registration order.
         * @return True when the requested configuration was installed.
         */
        [[nodiscard]] static bool Initialize(const Configuration &configuration, std::vector<std::shared_ptr<ISink>> sinks);

        /**
         * @brief Stops admission, drains within the configured timeout, and disables collection.
         * @return True when the dispatcher and all sink flushes completed before the timeout.
         */
        static bool Shutdown();

        /**
         * @brief Changes runtime collection gating without invalidating handles.
         * @param enabled Whether registered producers may enqueue records.
         */
        static void SetEnabled(bool enabled) noexcept;

        /**
         * @brief Returns whether observability producers are currently enabled.
         * @return True only when at least one sink exists and collection is enabled.
         */
        [[nodiscard]] static bool IsEnabled() noexcept;

        /**
         * @brief Checks event severity and subsystem policy before formatting or allocation.
         * @param subsystem Candidate hierarchical subsystem.
         * @param severity Candidate event severity.
         * @return True when the event may be constructed and submitted.
         */
        [[nodiscard]] static bool IsEventEnabled(std::string_view subsystem, Log::Level severity) noexcept;

        /**
         * @brief Waits for records accepted before this call to be dispatched.
         * @param timeout Maximum time to wait.
         * @return True when the dispatch watermark was reached.
         */
        [[nodiscard]] static bool Flush(std::chrono::milliseconds timeout = std::chrono::seconds{5});

        /**
         * @brief Enqueues a pre-built Horo record for asynchronous sink delivery.
         * @param record Backend-neutral record whose common runtime fields are assigned on admission.
         * @return False when disabled, stopping, stale, or rejected by the bounded queue.
         */
        [[nodiscard]] static bool EmitRecord(Record record) noexcept;

        /**
         * @brief Registers a monotonic counter outside latency-sensitive paths.
         * @param descriptor Stable metric identity; its kind is normalized to Counter.
         * @return Handle valid for the current runtime generation, or an empty handle when uninitialized.
         */
        [[nodiscard]] static Counter RegisterCounter(InstrumentDescriptor descriptor);

        /**
         * @brief Registers a last-value gauge outside latency-sensitive paths.
         * @param descriptor Stable metric identity; its kind is normalized to Gauge.
         * @return Handle valid for the current runtime generation, or an empty handle when uninitialized.
         */
        [[nodiscard]] static Gauge RegisterGauge(InstrumentDescriptor descriptor);

        /**
         * @brief Registers a timing or value histogram outside latency-sensitive paths.
         * @param descriptor Stable metric identity; its kind is normalized to Histogram.
         * @return Handle valid for the current runtime generation, or an empty handle when uninitialized.
         */
        [[nodiscard]] static Histogram RegisterHistogram(InstrumentDescriptor descriptor);

        /**
         * @brief Registers a duration instrument outside latency-sensitive paths.
         * @param descriptor Stable metric identity; its kind is normalized to Timing.
         * @return Handle valid for the current runtime generation, or an empty handle on invalid registration.
         */
        [[nodiscard]] static Timing RegisterTiming(InstrumentDescriptor descriptor);

        /**
         * @brief Emits a structured diagnostic event using the active inherited context.
         * @param subsystem Stable hierarchical subsystem tag.
         * @param eventName Stable low-cardinality event identity.
         * @param severity Diagnostic severity.
         * @param message Human-readable diagnostic detail.
         * @return False when disabled or rejected by the bounded queue.
         */
        [[nodiscard]] static bool EmitEvent(std::string_view subsystem, std::string_view eventName, Log::Level severity,
                                            std::string_view message);

        /**
         * @brief Emits a structured event using explicit inherited context.
         * @param subsystem Stable hierarchical subsystem tag.
         * @param eventName Stable low-cardinality event identity.
         * @param severity Diagnostic severity.
         * @param message Human-readable diagnostic detail.
         * @param context Immutable correlation context captured by the caller.
         * @return False when disabled or rejected by the bounded queue.
         */
        [[nodiscard]] static bool EmitEvent(std::string_view subsystem, std::string_view eventName, Log::Level severity,
                                            std::string_view message, const Log::LogContextSnapshot &context);

        /**
         * @brief Emits a structured event with record-local fields and explicit context.
         * @param subsystem Stable hierarchical subsystem tag.
         * @param eventName Stable low-cardinality event identity.
         * @param severity Diagnostic severity.
         * @param message Human-readable diagnostic detail.
         * @param fields Record-local structured fields copied into the record.
         * @param context Immutable correlation context captured by the caller.
         * @return False when disabled or rejected by the bounded queue.
         */
        [[nodiscard]] static bool EmitEvent(std::string_view subsystem, std::string_view eventName, Log::Level severity,
                                            std::string_view message, std::span<const Field> fields,
                                            const Log::LogContextSnapshot &context);

        /**
         * @brief Returns process-lifetime dispatcher health counters.
         * @return Monotonic ingestion, delivery, drop, failure, and timeout counters.
         */
        [[nodiscard]] static Statistics GetStatistics() noexcept;

        /**
         * @brief Copies bounded telemetry health and availability without descriptor text or record payloads.
         * @return Fixed-capacity snapshot containing only counters, process-local instrument indices, and enum states.
         */
        [[nodiscard]] static DiagnosticSnapshot GetDiagnosticSnapshot() noexcept;

        /**
         * @brief Updates a registered counter's typed availability state outside the metric fast path.
         * @param instrument Registered counter handle.
         * @param state Current availability state.
         * @return True when the handle belongs to the active runtime and the state is valid.
         */
        [[nodiscard]] static bool SetAvailability(const Counter &instrument, MetricAvailabilityState state) noexcept;
        /**
         * @brief Updates a registered gauge's typed availability state outside the metric fast path.
         * @param instrument Registered gauge handle.
         * @param state Current availability state.
         * @return True when the handle belongs to the active runtime and the state is valid.
         */
        [[nodiscard]] static bool SetAvailability(const Gauge &instrument, MetricAvailabilityState state) noexcept;
        /**
         * @brief Updates a registered histogram's typed availability state outside the metric fast path.
         * @param instrument Registered histogram handle.
         * @param state Current availability state.
         * @return True when the handle belongs to the active runtime and the state is valid.
         */
        [[nodiscard]] static bool SetAvailability(const Histogram &instrument, MetricAvailabilityState state) noexcept;
        /**
         * @brief Updates a registered timing instrument's typed availability state outside the metric fast path.
         * @param instrument Registered timing handle.
         * @param state Current availability state.
         * @return True when the handle belongs to the active runtime and the state is valid.
         */
        [[nodiscard]] static bool SetAvailability(const Timing &instrument, MetricAvailabilityState state) noexcept;

    private:
        Runtime() = delete;
        friend class Counter;
        friend class Gauge;
        friend class Histogram;
        friend class Timing;
        static bool SetAvailability(std::uint32_t instrumentId, std::uint32_t generation, MetricAvailabilityState state) noexcept;
        static bool BindMetric(std::uint32_t instrumentId, std::uint32_t generation, InstrumentKind kind,
                               std::span<const DimensionValue> dimensions, std::array<std::uint16_t, MaximumMetricDimensions> &valueIds,
                               std::uint8_t &dimensionCount) noexcept;
        static bool RecordMetric(std::uint32_t instrumentId, std::uint32_t generation, InstrumentKind kind, double value,
                                 const std::array<std::uint16_t, MaximumMetricDimensions> &dimensionValueIds,
                                 std::uint8_t dimensionCount) noexcept;
    };

    /** @brief Cheap pre-registered monotonic counter handle. */
    class Counter final {
    public:
        Counter() = default;

        /**
         * @brief Adds a non-negative delta when telemetry is enabled.
         * @param delta Amount added to the counter.
         */
#if HORO_ENABLE_TELEMETRY
        void Add(std::uint64_t delta = 1) const noexcept;
#else
        void Add(std::uint64_t = 1) const noexcept {}
#endif

        /** @brief Returns whether this handle references a registered instrument. */
        [[nodiscard]] explicit operator bool() const noexcept {
            return instrumentId_ != 0 && dimensionCount_ == requiredDimensionCount_;
        }

        /**
         * @brief Resolves one bounded low-cardinality series outside the update fast path.
         * @param dimensions Exact descriptor-declared dimension keys and allowlisted values.
         * @return Bound handle, or an empty handle when validation or the series budget fails.
         */
        [[nodiscard]] Counter WithDimensions(std::span<const DimensionValue> dimensions) const;

    private:
        friend class Runtime;

        Counter(std::uint32_t instrumentId, std::uint32_t generation, std::uint8_t requiredDimensionCount)
            : instrumentId_(instrumentId), generation_(generation), requiredDimensionCount_(requiredDimensionCount) {}

        std::uint32_t instrumentId_{};
        std::uint32_t generation_{};
        std::array<std::uint16_t, MaximumMetricDimensions> dimensionValueIds_{};
        std::uint8_t dimensionCount_{};
        std::uint8_t requiredDimensionCount_{};
    };

    /** @brief Cheap pre-registered last-value gauge handle. */
    class Gauge final {
    public:
        Gauge() = default;

        /**
         * @brief Records the current gauge value when telemetry is enabled.
         * @param value Current gauge value.
         */
#if HORO_ENABLE_TELEMETRY
        void Set(double value) const noexcept;
#else
        void Set(double) const noexcept {}
#endif

        /** @brief Returns whether this handle references a registered instrument. */
        [[nodiscard]] explicit operator bool() const noexcept {
            return instrumentId_ != 0 && dimensionCount_ == requiredDimensionCount_;
        }

        /** @brief Resolves one bounded low-cardinality series outside the update fast path. */
        [[nodiscard]] Gauge WithDimensions(std::span<const DimensionValue> dimensions) const;

    private:
        friend class Runtime;

        Gauge(std::uint32_t instrumentId, std::uint32_t generation, std::uint8_t requiredDimensionCount)
            : instrumentId_(instrumentId), generation_(generation), requiredDimensionCount_(requiredDimensionCount) {}

        std::uint32_t instrumentId_{};
        std::uint32_t generation_{};
        std::array<std::uint16_t, MaximumMetricDimensions> dimensionValueIds_{};
        std::uint8_t dimensionCount_{};
        std::uint8_t requiredDimensionCount_{};
    };

    /** @brief Cheap pre-registered histogram observation handle. */
    class Histogram final {
    public:
        Histogram() = default;

        /**
         * @brief Records one timing or value observation when telemetry is enabled.
         * @param value Timing or distribution observation.
         */
#if HORO_ENABLE_TELEMETRY
        void Observe(double value) const noexcept;
#else
        void Observe(double) const noexcept {}
#endif

        /** @brief Returns whether this handle references a registered instrument. */
        [[nodiscard]] explicit operator bool() const noexcept {
            return instrumentId_ != 0 && dimensionCount_ == requiredDimensionCount_;
        }

        /** @brief Resolves one bounded low-cardinality series outside the update fast path. */
        [[nodiscard]] Histogram WithDimensions(std::span<const DimensionValue> dimensions) const;

    private:
        friend class Runtime;

        Histogram(std::uint32_t instrumentId, std::uint32_t generation, std::uint8_t requiredDimensionCount)
            : instrumentId_(instrumentId), generation_(generation), requiredDimensionCount_(requiredDimensionCount) {}

        std::uint32_t instrumentId_{};
        std::uint32_t generation_{};
        std::array<std::uint16_t, MaximumMetricDimensions> dimensionValueIds_{};
        std::uint8_t dimensionCount_{};
        std::uint8_t requiredDimensionCount_{};
    };

    /** @brief Cheap pre-registered duration observation handle. */
    class Timing final {
    public:
        Timing() = default;
#if HORO_ENABLE_TELEMETRY
        void Record(std::chrono::nanoseconds duration) const noexcept;
#else
        void Record(std::chrono::nanoseconds) const noexcept {}
#endif
        [[nodiscard]] explicit operator bool() const noexcept {
            return instrumentId_ != 0 && dimensionCount_ == requiredDimensionCount_;
        }

        /** @brief Resolves one bounded low-cardinality series outside the update fast path. */
        [[nodiscard]] Timing WithDimensions(std::span<const DimensionValue> dimensions) const;

    private:
        friend class Runtime;

        Timing(std::uint32_t instrumentId, std::uint32_t generation, std::uint8_t requiredDimensionCount)
            : instrumentId_(instrumentId), generation_(generation), requiredDimensionCount_(requiredDimensionCount) {}

        std::uint32_t instrumentId_{};
        std::uint32_t generation_{};
        std::array<std::uint16_t, MaximumMetricDimensions> dimensionValueIds_{};
        std::uint8_t dimensionCount_{};
        std::uint8_t requiredDimensionCount_{};
    };

    /** @brief RAII steady-clock timing observation for a histogram handle. */
    class ScopedTimer final {
    public:
        /**
         * @brief Starts a timer whose elapsed milliseconds are observed at destruction.
         * @param histogram Histogram receiving the elapsed milliseconds.
         */
#if HORO_ENABLE_TELEMETRY
        explicit ScopedTimer(Histogram histogram) noexcept;
#else
        explicit ScopedTimer(Histogram) noexcept {}
#endif
        ScopedTimer(const ScopedTimer &) = delete;
        ScopedTimer &operator=(const ScopedTimer &) = delete;
#if HORO_ENABLE_TELEMETRY
        ~ScopedTimer();
#else
        ~ScopedTimer() = default;
#endif

        /** @brief Starts a timer backed by an explicit duration instrument. */
#if HORO_ENABLE_TELEMETRY
        explicit ScopedTimer(Timing timing) noexcept;
#else
        explicit ScopedTimer(Timing) noexcept {}
#endif

    private:
        Histogram histogram_;
        Timing timing_;
        std::chrono::steady_clock::time_point startedAt_{std::chrono::steady_clock::now()};
    };
}  // namespace Horo::Telemetry
