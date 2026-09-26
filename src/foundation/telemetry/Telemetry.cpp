#include "Horo/Foundation/Telemetry/Telemetry.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <ranges>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace Horo::Telemetry {
    namespace {
        /** @brief Process-lifetime atomics intentionally retained for detached timeout recovery. */
        struct AtomicStatistics {
            std::atomic<std::uint64_t> acceptedRecords{};
            std::atomic<std::uint64_t> exportedRecords{};
            std::atomic<std::uint64_t> droppedRecords{};
            std::atomic<std::uint64_t> filteredRecords{};
            std::atomic<std::uint64_t> contentionDrops{};
            std::atomic<std::uint64_t> queueFullDrops{};
            std::atomic<std::uint64_t> shutdownDrops{};
            std::atomic<std::uint64_t> staleHandleDrops{};
            std::atomic<std::uint64_t> sinkFailures{};
            std::atomic<std::uint64_t> flushTimeouts{};
            std::atomic<std::uint64_t> shutdownTimeouts{};
            std::atomic<std::uint64_t> invalidInstrumentRegistrations{};
            std::atomic<std::uint64_t> rejectedMetricSeries{};
        };

        /** @brief Returns counters shared with workers that may finish after bounded shutdown. */
        std::shared_ptr<AtomicStatistics> HealthStorage() {
            static const auto statistics = std::make_shared<AtomicStatistics>();
            return statistics;
        }

        /** @brief Returns process health counters. */
        AtomicStatistics &Health() noexcept {
            static const auto statistics = HealthStorage();
            return *statistics;
        }

        struct InstrumentRegistration {
            std::uint32_t id{};
            std::uint8_t dimensionCount{};
        };

        struct RegisteredInstrument {
            InstrumentDescriptor descriptor;
            std::vector<std::array<std::uint16_t, MaximumMetricDimensions>> series;
            MetricAvailabilityState availability{MetricAvailabilityState::Available};
        };

        /** @brief Queue data protected by one admission/consumer mutex. */
        struct TelemetryQueue {
            explicit TelemetryQueue(const std::size_t capacity) : records(capacity) {}

            std::vector<Record> records;
            std::size_t head{};
            std::size_t tail{};
            std::size_t count{};
            std::uint64_t nextSequence{1};
            std::uint64_t lastAccepted{};
            std::mutex mutex;
            std::condition_variable ready;
        };

        /** @brief Export watermark and its waiters. */
        struct TelemetryFlushState {
            std::atomic<std::uint64_t> lastExported{};
            std::mutex mutex;
            std::condition_variable completed;
        };

        /** @brief Worker completion state and its waiters. */
        struct TelemetryExitState {
            std::atomic<bool> writerExited{};
            std::mutex mutex;
            std::condition_variable completed;
        };

        [[nodiscard]] bool IsCanonicalMetricName(const std::string_view value) noexcept {
            if (value.empty() || value.front() == '.' || value.back() == '.')
                return false;
            bool previousDot = false;
            for (const unsigned char character : value) {
                const bool dot = character == '.';
                if (dot && previousDot)
                    return false;
                if (!dot && character != '_' && !std::islower(character) && !std::isdigit(character))
                    return false;
                previousDot = dot;
            }
            return true;
        }

        [[nodiscard]] bool IsValidMetricUnit(const MetricUnit unit) noexcept {
            return unit >= MetricUnit::Count && unit <= MetricUnit::Ratio;
        }

        [[nodiscard]] bool IsValidAvailabilityState(const MetricAvailabilityState state) noexcept {
            return state >= MetricAvailabilityState::Available && state <= MetricAvailabilityState::SamplerFailed;
        }

        [[nodiscard]] bool IsSensitiveKey(std::string_view key) {
            std::string normalized{key};
            std::ranges::transform(normalized, normalized.begin(), [](const unsigned char character) {
                return static_cast<char>(std::tolower(character));
            });
            static constexpr std::array<std::string_view, 7> fragments{"authorization", "cookie", "credential", "password",
                                                                       "private_key",   "secret", "token"};
            return std::ranges::any_of(fragments, [&normalized](const std::string_view fragment) {
                return normalized.find(fragment) != std::string::npos;
            });
        }

        void RedactFields(std::vector<Field> &fields) {
            std::erase_if(fields, [](const Field &field) {
                return field.privacy == FieldPrivacy::Forbidden;
            });
            for (Field &field : fields) {
                if (field.privacy == FieldPrivacy::SensitiveRedacted || IsSensitiveKey(field.key))
                    field.value = std::string{"[REDACTED]"};
            }
        }

        void RedactRecord(Record &record) {
            if (!record.context.Fields().empty()) {
                Log::LogContextSnapshot redacted;
                for (const auto &[key, value] : record.context.Fields())
                    redacted = redacted.With(key, IsSensitiveKey(key) ? "[REDACTED]" : value);
                record.context = std::move(redacted);
            }
            std::visit([]<typename Payload>(Payload &payload) {
                if constexpr (std::is_same_v<Payload, LogRecord> || std::is_same_v<Payload, SpanRecord> ||
                              std::is_same_v<Payload, DiagnosticEvent>)
                    RedactFields(payload.fields);
            }, record.payload);
        }

        [[nodiscard]] bool IsValidDescriptor(const InstrumentDescriptor &descriptor) noexcept {
            if (descriptor.name.size() > MaximumMetricNameBytes || !IsCanonicalMetricName(descriptor.name) ||
                descriptor.subsystem.empty() || descriptor.subsystem.size() > MaximumMetricSubsystemBytes ||
                descriptor.description.size() > MaximumMetricDescriptionBytes || !IsValidMetricUnit(descriptor.unit) ||
                descriptor.maxSeries == 0 || descriptor.maxSeries > MaximumMetricSeries ||
                descriptor.dimensions.size() > MaximumMetricDimensions)
                return false;
            for (std::size_t index = 0; index < descriptor.dimensions.size(); ++index) {
                const DimensionDescriptor &dimension = descriptor.dimensions[index];
                if (dimension.key.size() > MaximumMetricDimensionKeyBytes || !IsCanonicalMetricName(dimension.key) ||
                    dimension.allowedValues.empty() || dimension.allowedValues.size() > MaximumMetricDimensionValues)
                    return false;
                if (std::find_if(descriptor.dimensions.begin(), descriptor.dimensions.begin() + static_cast<std::ptrdiff_t>(index),
                                 [&dimension](const DimensionDescriptor &candidate) {
                    return candidate.key == dimension.key;
                }) != descriptor.dimensions.begin() + static_cast<std::ptrdiff_t>(index))
                    return false;
                for (std::size_t valueIndex = 0; valueIndex < dimension.allowedValues.size(); ++valueIndex) {
                    if (dimension.allowedValues[valueIndex].empty() ||
                        dimension.allowedValues[valueIndex].size() > MaximumMetricDimensionValueBytes ||
                        std::find(dimension.allowedValues.begin(),
                                  dimension.allowedValues.begin() + static_cast<std::ptrdiff_t>(valueIndex),
                                  dimension.allowedValues[valueIndex]) !=
                            dimension.allowedValues.begin() + static_cast<std::ptrdiff_t>(valueIndex))
                        return false;
                }
            }
            return true;
        }

        class TelemetryState final : public std::enable_shared_from_this<TelemetryState> {
        public:
            TelemetryState(const Configuration &configuration, std::vector<std::shared_ptr<ISink>> sinks, const std::uint32_t generation)
                : configuration_(configuration), queue_(configuration.queueCapacity), sinks_(std::move(sinks)), generation_(generation) {}

            ~TelemetryState() {
                if (!writer_.joinable())
                    return;
                stopping_.store(true);
                queue_.ready.notify_all();
                if (writer_.get_id() == std::this_thread::get_id())
                    writer_.detach();  // NOSONAR: self-owned bounded-shutdown worker cannot join itself.
                else
                    writer_.join();
            }

            void Start() {
                writer_ = std::thread([self = shared_from_this()] {  // NOSONAR: jthread is unavailable on the minimum supported libc++.
                    self->WriterLoop();
                });
            }

            [[nodiscard]] std::uint32_t Generation() const noexcept {
                return generation_;
            }

            [[nodiscard]] std::chrono::milliseconds ShutdownTimeout() const noexcept {
                return configuration_.shutdownTimeout;
            }

            [[nodiscard]] bool HasExited() const noexcept {
                return exit_.writerExited.load();
            }

            [[nodiscard]] bool SetAvailability(const std::uint32_t instrumentId, const MetricAvailabilityState state) noexcept {
                if (!IsValidAvailabilityState(state))
                    return false;
                std::lock_guard lock(descriptorMutex_);
                if (instrumentId == 0 || instrumentId > descriptors_.size())
                    return false;
                if (RegisteredInstrument &registered = descriptors_[instrumentId - 1U]; registered.availability != state) {
                    registered.availability = state;
                    ++availabilityRevision_;
                }
                return true;
            }

            void FillDiagnosticSnapshot(DiagnosticSnapshot &snapshot) {
                std::lock_guard lock(descriptorMutex_);
                snapshot.runtimeGeneration = generation_;
                snapshot.availabilityRevision = availabilityRevision_;
                snapshot.availabilityCount = static_cast<std::uint16_t>(descriptors_.size());
                for (std::size_t index = 0; index < descriptors_.size(); ++index) {
                    snapshot.availability[index] = MetricAvailabilityRecord{.instrumentId = static_cast<std::uint32_t>(index + 1U),
                                                                            .state = descriptors_[index].availability};
                }
            }

            [[nodiscard]] std::optional<InstrumentRegistration> Register(InstrumentDescriptor descriptor) {
                std::lock_guard lock(descriptorMutex_);
                if (descriptors_.size() >= MaximumMetricInstruments || !IsValidDescriptor(descriptor) ||
                    std::ranges::any_of(descriptors_, [&descriptor](const RegisteredInstrument &registered) {
                    return registered.descriptor.name == descriptor.name;
                })) {
                    health_->invalidInstrumentRegistrations.fetch_add(1);
                    return std::nullopt;
                }
                RegisteredInstrument registered{.descriptor = std::move(descriptor)};
                registered.series.reserve(registered.descriptor.dimensions.empty() ? 1U : registered.descriptor.maxSeries);
                if (registered.descriptor.dimensions.empty())
                    registered.series.push_back({});
                descriptors_.push_back(std::move(registered));
                ++availabilityRevision_;
                return InstrumentRegistration{.id = static_cast<std::uint32_t>(descriptors_.size()),
                                              .dimensionCount =
                                                  static_cast<std::uint8_t>(descriptors_.back().descriptor.dimensions.size())};
            }

            [[nodiscard]] bool Bind(const std::uint32_t instrumentId, const InstrumentKind kind,
                                    const std::span<const DimensionValue> dimensions,
                                    std::array<std::uint16_t, MaximumMetricDimensions> &valueIds, std::uint8_t &dimensionCount) {
                std::lock_guard lock(descriptorMutex_);
                if (instrumentId == 0 || instrumentId > descriptors_.size())
                    return false;
                RegisteredInstrument &registered = descriptors_[instrumentId - 1U];
                const InstrumentDescriptor &descriptor = registered.descriptor;
                if (descriptor.kind != kind || dimensions.size() != descriptor.dimensions.size())
                    return false;

                valueIds.fill(0);
                for (std::size_t descriptorIndex = 0; descriptorIndex < descriptor.dimensions.size(); ++descriptorIndex) {
                    const DimensionDescriptor &expected = descriptor.dimensions[descriptorIndex];
                    const auto selected = std::ranges::find(dimensions, expected.key, &DimensionValue::key);
                    if (selected == dimensions.end())
                        return false;
                    if (std::ranges::count(dimensions, expected.key, &DimensionValue::key) != 1)
                        return false;
                    const auto allowed = std::ranges::find(expected.allowedValues, selected->value);
                    if (allowed == expected.allowedValues.end())
                        return false;
                    valueIds[descriptorIndex] = static_cast<std::uint16_t>(std::distance(expected.allowedValues.begin(), allowed) + 1);
                }

                dimensionCount = static_cast<std::uint8_t>(descriptor.dimensions.size());
                if (std::ranges::find(registered.series, valueIds) != registered.series.end())
                    return true;
                if (registered.series.size() >= descriptor.maxSeries)
                    return false;
                registered.series.push_back(valueIds);
                return true;
            }

            [[nodiscard]] bool AllowsSubsystem(const std::string_view subsystem) const noexcept {
                if (configuration_.subsystemPrefixes.empty())
                    return true;
                return std::ranges::any_of(configuration_.subsystemPrefixes, [subsystem](const std::string &prefix) {
                    return subsystem == prefix ||
                           (subsystem.size() > prefix.size() && subsystem.starts_with(prefix) && subsystem[prefix.size()] == '.');
                });
            }

            [[nodiscard]] bool AllowsMetric(const InstrumentDescriptor &descriptor) const noexcept {
                return configuration_.metricCollectionLevel != MetricCollectionLevel::Off &&
                       configuration_.metricCollectionLevel >= descriptor.minimumCollectionLevel && AllowsSubsystem(descriptor.subsystem);
            }

            [[nodiscard]] bool AllowsEvent(const std::string_view subsystem, const Log::Level severity) const noexcept {
                return severity >= configuration_.minimumEventSeverity && severity < Log::Level::Off && AllowsSubsystem(subsystem);
            }

            [[nodiscard]] bool Allows(const Record &record) const noexcept {
                if (!AllowsSubsystem(record.subsystem))
                    return false;
                if (const auto *log = std::get_if<LogRecord>(&record.payload); log != nullptr)
                    return AllowsEvent(record.subsystem, log->severity);
                if (const auto *event = std::get_if<DiagnosticEvent>(&record.payload); event != nullptr)
                    return AllowsEvent(record.subsystem, event->severity);
                return true;
            }

            [[nodiscard]] bool TryPush(Record record) noexcept {
                if (std::unique_lock lock(queue_.mutex, std::try_to_lock); lock.owns_lock()) {
                    if (stopping_.load()) {
                        CountDrop(health_->shutdownDrops);
                        return false;
                    }
                    if (queue_.count == queue_.records.size()) {
                        health_->queueFullDrops.fetch_add(1);
                        health_->droppedRecords.fetch_add(1);
                        if (configuration_.overflowPolicy == OverflowPolicy::DropNewest)
                            return false;
                        queue_.records[queue_.head] = Record{};
                        queue_.head = (queue_.head + 1U) % queue_.records.size();
                        --queue_.count;
                    }

                    record.sequence = queue_.nextSequence++;
                    record.timestampUtc = std::chrono::system_clock::now();
                    record.monotonicTime = std::chrono::steady_clock::now();
                    record.threadId = static_cast<std::uint64_t>(std::hash<std::thread::id>{}(std::this_thread::get_id()));
                    queue_.records[queue_.tail] = std::move(record);
                    queue_.tail = (queue_.tail + 1U) % queue_.records.size();
                    ++queue_.count;
                    queue_.lastAccepted = queue_.records[(queue_.tail + queue_.records.size() - 1U) % queue_.records.size()].sequence;
                    health_->acceptedRecords.fetch_add(1);
                } else {
                    CountDrop(health_->contentionDrops);
                    return false;
                }
                queue_.ready.notify_one();
                return true;
            }

            [[nodiscard]] bool Flush(const std::chrono::milliseconds timeout) {
                std::uint64_t watermark{};
                {
                    std::lock_guard lock(queue_.mutex);
                    watermark = queue_.lastAccepted;
                }
                queue_.ready.notify_one();
                std::unique_lock lock(flush_.mutex);
                const bool completed = flush_.completed.wait_for(lock, timeout, [this, watermark] {
                    return flush_.lastExported.load() >= watermark || exit_.writerExited.load();
                });
                if (!completed)
                    health_->flushTimeouts.fetch_add(1);
                return completed;
            }

            [[nodiscard]] bool Stop(const std::chrono::milliseconds timeout) {
                if (bool expected = false; !stopping_.compare_exchange_strong(expected, true))
                    return WaitAndJoin(timeout);
                {
                    // Synchronize the predicate transition with WriterLoop's
                    // condition-variable wait so the stop notification cannot
                    // be lost between its predicate check and sleep.
                    std::lock_guard lock(queue_.mutex);
                }
                queue_.ready.notify_all();
                return WaitAndJoin(timeout);
            }

        private:
            void CountDrop(std::atomic<std::uint64_t> &reason) const noexcept {
                reason.fetch_add(1);
                health_->droppedRecords.fetch_add(1);
            }

            [[nodiscard]] bool WaitAndJoin(const std::chrono::milliseconds timeout) {
                bool exited{};
                {
                    std::unique_lock lock(exit_.mutex);
                    exited = exit_.completed.wait_for(lock, timeout, [this] {
                        return exit_.writerExited.load();
                    });
                }
                if (!exited) {
                    if (writer_.joinable())
                        writer_.detach();  // NOSONAR: bounded shutdown transfers lifetime to the worker's shared self-capture.
                    return false;
                }
                if (writer_.joinable())
                    writer_.join();
                return flushSucceeded_.load();
            }

            void WriterLoop() noexcept {
                while (true) {
                    std::optional<Record> record;
                    bool periodicFlush{};
                    {
                        std::unique_lock lock(queue_.mutex);
                        if (const bool ready = queue_.ready.wait_for(lock, configuration_.sinkFlushInterval,
                                                                     [this] {
                            return queue_.count != 0 || stopping_.load();
                        });
                            !ready) {
                            periodicFlush = true;
                        } else if (queue_.count == 0 && stopping_.load()) {
                            break;
                        } else {
                            record.emplace(std::move(queue_.records[queue_.head]));
                            queue_.records[queue_.head] = Record{};
                            queue_.head = (queue_.head + 1U) % queue_.records.size();
                            --queue_.count;
                        }
                    }
                    if (periodicFlush) {
                        FlushSinks();
                        continue;
                    }
                    if (record.has_value())
                        Dispatch(*record);
                }

                FlushSinks();
                {
                    std::lock_guard lock(exit_.mutex);
                    exit_.writerExited.store(true);
                }
                flush_.completed.notify_all();
                exit_.completed.notify_all();
            }

            void Dispatch(Record &record) noexcept {
                std::optional<InstrumentDescriptor> descriptor;
                if (const auto *metric = std::get_if<MetricRecord>(&record.payload); metric != nullptr) {
                    std::lock_guard lock(descriptorMutex_);
                    if (metric->instrumentId > 0 && metric->instrumentId <= descriptors_.size())
                        descriptor = descriptors_[metric->instrumentId - 1U].descriptor;
                }
                if (descriptor.has_value() && record.subsystem.empty())
                    record.subsystem = descriptor->subsystem;

                for (const auto &sink : sinks_) {
                    try {
                        sink->Export(record, descriptor ? &*descriptor : nullptr);
                    } catch (...) {
                        health_->sinkFailures.fetch_add(1);
                    }
                }

                health_->exportedRecords.fetch_add(1);
                {
                    std::lock_guard lock(flush_.mutex);
                    flush_.lastExported.store(record.sequence);
                }
                flush_.completed.notify_all();
            }

            void FlushSinks() noexcept {
                for (const auto &sink : sinks_) {
                    try {
                        sink->Flush();
                    } catch (...) {
                        flushSucceeded_.store(false);
                        health_->sinkFailures.fetch_add(1);
                    }
                }
            }

            Configuration configuration_;
            TelemetryQueue queue_;
            TelemetryFlushState flush_;
            std::mutex descriptorMutex_;
            std::vector<RegisteredInstrument> descriptors_;
            std::uint64_t availabilityRevision_{};
            std::vector<std::shared_ptr<ISink>> sinks_;
            const std::uint32_t generation_;
            std::atomic<bool> stopping_{};
            std::atomic<bool> flushSucceeded_{true};
            TelemetryExitState exit_;
            std::thread writer_;  // NOSONAR: see Start(); bounded shutdown requires explicit detach support.
            std::shared_ptr<AtomicStatistics> health_{HealthStorage()};
        };

        /** @brief Process runtime controls hidden behind one function-local owner. */
        struct RuntimeGlobals {
            [[nodiscard]] std::shared_ptr<TelemetryState> LoadState() const noexcept {
                return std::atomic_load(&state);
            }

            void StoreState(std::shared_ptr<TelemetryState> replacement) noexcept {
                std::atomic_store(&state, std::move(replacement));
            }

            [[nodiscard]] std::shared_ptr<TelemetryState> ExchangeState() noexcept {
                return std::atomic_exchange(&state, std::shared_ptr<TelemetryState>{});
            }

            std::atomic<bool> enabled{};
            std::mutex lifecycleMutex;
            std::atomic<std::uint32_t> nextGeneration{1};
            std::shared_ptr<TelemetryState> state;
        };

        RuntimeGlobals &Globals() {
            static RuntimeGlobals globals;
            return globals;
        }
    }  // namespace

    /** @copydoc Record::Kind */
    RecordKind Record::Kind() const noexcept {
        using enum RecordKind;
        if (std::holds_alternative<LogRecord>(payload))
            return Log;
        if (std::holds_alternative<MetricRecord>(payload))
            return Metric;
        if (std::holds_alternative<SpanRecord>(payload))
            return Span;
        return Event;
    }

    /** @copydoc Runtime::Initialize */
    bool Runtime::Initialize(const Configuration &configuration, std::shared_ptr<ISink> sink) {
        std::vector<std::shared_ptr<ISink>> sinks;
        if (sink != nullptr)
            sinks.push_back(std::move(sink));
        return Initialize(configuration, std::move(sinks));
    }

    /** @copydoc Runtime::Initialize */
    bool Runtime::Initialize(const Configuration &configuration, std::vector<std::shared_ptr<ISink>> sinks) {
        if (configuration.queueCapacity == 0 || configuration.shutdownTimeout <= std::chrono::milliseconds::zero() ||
            configuration.sinkFlushInterval <= std::chrono::milliseconds::zero())
            return false;

        static_cast<void>(Shutdown());
        std::erase(sinks, nullptr);
        RuntimeGlobals &globals = Globals();
        if (!configuration.enabled || sinks.empty()) {
            globals.enabled.store(false);
            return true;
        }

        const std::uint32_t generation = globals.nextGeneration.fetch_add(1);
        auto state = std::make_shared<TelemetryState>(configuration, std::move(sinks), generation);
        state->Start();
        {
            std::lock_guard lock(globals.lifecycleMutex);
            globals.StoreState(std::move(state));
        }
        globals.enabled.store(true);
        return true;
    }

    /** @copydoc Runtime::Shutdown */
    bool Runtime::Shutdown() {
        RuntimeGlobals &globals = Globals();
        globals.enabled.store(false);
        std::shared_ptr<TelemetryState> state;
        {
            std::lock_guard lock(globals.lifecycleMutex);
            state = globals.ExchangeState();
        }
        if (state == nullptr)
            return true;
        const bool completed = state->Stop(state->ShutdownTimeout());
        if (!completed && !state->HasExited())
            Health().shutdownTimeouts.fetch_add(1);
        return completed;
    }

    /** @copydoc Runtime::SetEnabled */
    void Runtime::SetEnabled(const bool enabled) noexcept {
        RuntimeGlobals &globals = Globals();
        globals.enabled.store(enabled && globals.LoadState() != nullptr);
    }

    /** @copydoc Runtime::IsEnabled */
    bool Runtime::IsEnabled() noexcept {
        return Globals().enabled.load();
    }

    /** @copydoc Runtime::IsEventEnabled */
    bool Runtime::IsEventEnabled(const std::string_view subsystem, const Log::Level severity) noexcept {
#if !HORO_ENABLE_TELEMETRY
        static_cast<void>(subsystem);
        static_cast<void>(severity);
        return false;
#else
        if (!IsEnabled())
            return false;
        const auto state = Globals().LoadState();
        return state != nullptr && state->AllowsEvent(subsystem, severity);
#endif
    }

    /** @copydoc Runtime::Flush */
    bool Runtime::Flush(const std::chrono::milliseconds timeout) {
        const auto state = Globals().LoadState();
        return state == nullptr || state->Flush(timeout);
    }

    /** @copydoc Runtime::EmitRecord */
    bool Runtime::EmitRecord(Record record) noexcept {
        if (!IsEnabled())
            return false;
        const auto state = Globals().LoadState();
        if (state == nullptr) {
            Health().shutdownDrops.fetch_add(1);
            Health().droppedRecords.fetch_add(1);
            return false;
        }
        if (!state->Allows(record)) {
            Health().filteredRecords.fetch_add(1);
            return false;
        }
        try {
            RedactRecord(record);
            return state->TryPush(std::move(record));
        } catch (const std::bad_alloc &) {
            Health().droppedRecords.fetch_add(1);
            return false;
        }
    }

    /** @copydoc Runtime::RegisterCounter */
    Counter Runtime::RegisterCounter(InstrumentDescriptor descriptor) {
#if !HORO_ENABLE_TELEMETRY
        static_cast<void>(descriptor);
        return {};
#else
        descriptor.kind = InstrumentKind::Counter;
        const auto state = Globals().LoadState();
        if (state == nullptr || !state->AllowsMetric(descriptor))
            return {};
        const auto registration = state->Register(std::move(descriptor));
        return registration ? Counter{registration->id, state->Generation(), registration->dimensionCount} : Counter{};
#endif
    }

    /** @copydoc Runtime::RegisterGauge */
    Gauge Runtime::RegisterGauge(InstrumentDescriptor descriptor) {
#if !HORO_ENABLE_TELEMETRY
        static_cast<void>(descriptor);
        return {};
#else
        descriptor.kind = InstrumentKind::Gauge;
        const auto state = Globals().LoadState();
        if (state == nullptr || !state->AllowsMetric(descriptor))
            return {};
        const auto registration = state->Register(std::move(descriptor));
        return registration ? Gauge{registration->id, state->Generation(), registration->dimensionCount} : Gauge{};
#endif
    }

    /** @copydoc Runtime::RegisterHistogram */
    Histogram Runtime::RegisterHistogram(InstrumentDescriptor descriptor) {
#if !HORO_ENABLE_TELEMETRY
        static_cast<void>(descriptor);
        return {};
#else
        descriptor.kind = InstrumentKind::Histogram;
        const auto state = Globals().LoadState();
        if (state == nullptr || !state->AllowsMetric(descriptor))
            return {};
        const auto registration = state->Register(std::move(descriptor));
        return registration ? Histogram{registration->id, state->Generation(), registration->dimensionCount} : Histogram{};
#endif
    }

    /** @copydoc Runtime::RegisterTiming */
    Timing Runtime::RegisterTiming(InstrumentDescriptor descriptor) {
#if !HORO_ENABLE_TELEMETRY
        static_cast<void>(descriptor);
        return {};
#else
        const auto state = Globals().LoadState();
        if (state == nullptr || !state->AllowsMetric(descriptor))
            return {};
        if (!IsValidMetricUnit(descriptor.unit)) {
            Health().invalidInstrumentRegistrations.fetch_add(1);
            return {};
        }
        descriptor.kind = InstrumentKind::Timing;
        descriptor.unit = MetricUnit::Seconds;
        const auto registration = state->Register(std::move(descriptor));
        return registration ? Timing{registration->id, state->Generation(), registration->dimensionCount} : Timing{};
#endif
    }

    /** @copydoc Runtime::EmitEvent */
    bool Runtime::EmitEvent(const std::string_view subsystem, const std::string_view eventName, const Log::Level severity,
                            const std::string_view message) {
        if (!IsEnabled())
            return false;
        if (!IsEventEnabled(subsystem, severity)) {
            Health().filteredRecords.fetch_add(1);
            return false;
        }
        return EmitEvent(subsystem, eventName, severity, message, {}, Log::CaptureLogContext());
    }

    /** @copydoc Runtime::EmitEvent */
    bool Runtime::EmitEvent(const std::string_view subsystem, const std::string_view eventName, const Log::Level severity,
                            const std::string_view message, const Log::LogContextSnapshot &context) {
        return EmitEvent(subsystem, eventName, severity, message, {}, context);
    }

    /** @copydoc Runtime::EmitEvent */
    bool Runtime::EmitEvent(const std::string_view subsystem, const std::string_view eventName, const Log::Level severity,
                            const std::string_view message, const std::span<const Field> fields, const Log::LogContextSnapshot &context) {
        if (!IsEnabled())
            return false;
        const auto state = Globals().LoadState();
        if (state == nullptr)
            return false;
        if (!state->AllowsEvent(subsystem, severity)) {
            Health().filteredRecords.fetch_add(1);
            return false;
        }
        Record record{.subsystem = std::string{subsystem},
                      .context = context,
                      .payload = DiagnosticEvent{.severity = severity,
                                                 .name = std::string{eventName},
                                                 .message = std::string{message},
                                                 .fields = std::vector<Field>{fields.begin(), fields.end()}}};
        try {
            RedactRecord(record);
            return state->TryPush(std::move(record));
        } catch (const std::bad_alloc &) {
            Health().droppedRecords.fetch_add(1);
            return false;
        }
    }

    /** @copydoc Runtime::GetStatistics */
    Statistics Runtime::GetStatistics() noexcept {
        const AtomicStatistics &health = Health();
        return {.acceptedRecords = health.acceptedRecords.load(),
                .exportedRecords = health.exportedRecords.load(),
                .droppedRecords = health.droppedRecords.load(),
                .filteredRecords = health.filteredRecords.load(),
                .contentionDrops = health.contentionDrops.load(),
                .queueFullDrops = health.queueFullDrops.load(),
                .shutdownDrops = health.shutdownDrops.load(),
                .staleHandleDrops = health.staleHandleDrops.load(),
                .sinkFailures = health.sinkFailures.load(),
                .flushTimeouts = health.flushTimeouts.load(),
                .shutdownTimeouts = health.shutdownTimeouts.load(),
                .invalidInstrumentRegistrations = health.invalidInstrumentRegistrations.load(),
                .rejectedMetricSeries = health.rejectedMetricSeries.load()};
    }

    /** @copydoc Runtime::GetDiagnosticSnapshot */
    DiagnosticSnapshot Runtime::GetDiagnosticSnapshot() noexcept {
        DiagnosticSnapshot snapshot{.statistics = GetStatistics()};
        if (const auto state = Globals().LoadState(); state != nullptr) {
            snapshot.runtimeEnabled = IsEnabled();
            state->FillDiagnosticSnapshot(snapshot);
        }
        return snapshot;
    }

    /** @copydoc Runtime::SetAvailability */
    bool Runtime::SetAvailability(const Counter &instrument, const MetricAvailabilityState state) noexcept {
        return SetAvailability(instrument.instrumentId_, instrument.generation_, state);
    }

    /** @copydoc Runtime::SetAvailability */
    bool Runtime::SetAvailability(const Gauge &instrument, const MetricAvailabilityState state) noexcept {
        return SetAvailability(instrument.instrumentId_, instrument.generation_, state);
    }

    /** @copydoc Runtime::SetAvailability */
    bool Runtime::SetAvailability(const Histogram &instrument, const MetricAvailabilityState state) noexcept {
        return SetAvailability(instrument.instrumentId_, instrument.generation_, state);
    }

    /** @copydoc Runtime::SetAvailability */
    bool Runtime::SetAvailability(const Timing &instrument, const MetricAvailabilityState state) noexcept {
        return SetAvailability(instrument.instrumentId_, instrument.generation_, state);
    }

    bool Runtime::SetAvailability(const std::uint32_t instrumentId, const std::uint32_t generation,
                                  const MetricAvailabilityState state) noexcept {
        const auto runtime = Globals().LoadState();
        return runtime != nullptr && runtime->Generation() == generation && runtime->SetAvailability(instrumentId, state);
    }

    bool Runtime::BindMetric(const std::uint32_t instrumentId, const std::uint32_t generation, const InstrumentKind kind,
                             const std::span<const DimensionValue> dimensions, std::array<std::uint16_t, MaximumMetricDimensions> &valueIds,
                             std::uint8_t &dimensionCount) noexcept {
#if !HORO_ENABLE_TELEMETRY
        static_cast<void>(instrumentId);
        static_cast<void>(generation);
        static_cast<void>(kind);
        static_cast<void>(dimensions);
        static_cast<void>(valueIds);
        static_cast<void>(dimensionCount);
        return false;
#else
        const auto state = Globals().LoadState();
        if (state == nullptr || state->Generation() != generation)
            return false;
        try {
            if (const bool bound = state->Bind(instrumentId, kind, dimensions, valueIds, dimensionCount); !bound) {
                Health().rejectedMetricSeries.fetch_add(1);
                return false;
            }
            return true;
        } catch (const std::bad_alloc &) {
            Health().rejectedMetricSeries.fetch_add(1);
            return false;
        }
#endif
    }

    bool Runtime::RecordMetric(const std::uint32_t instrumentId, const std::uint32_t generation, const InstrumentKind kind,
                               const double value, const std::array<std::uint16_t, MaximumMetricDimensions> &dimensionValueIds,
                               const std::uint8_t dimensionCount) noexcept {
        if (instrumentId == 0 || !IsEnabled())
            return false;
        const auto state = Globals().LoadState();
        if (state == nullptr || state->Generation() != generation) {
            Health().staleHandleDrops.fetch_add(1);
            Health().droppedRecords.fetch_add(1);
            return false;
        }
        return state->TryPush(Record{.payload = MetricRecord{.kind = kind,
                                                             .instrumentId = instrumentId,
                                                             .value = value,
                                                             .dimensionValueIds = dimensionValueIds,
                                                             .dimensionCount = dimensionCount}});
    }

    Counter Counter::WithDimensions(const std::span<const DimensionValue> dimensions) const {
        Counter bound = *this;
        return Runtime::BindMetric(instrumentId_, generation_, InstrumentKind::Counter, dimensions, bound.dimensionValueIds_,
                                   bound.dimensionCount_)
                   ? bound
                   : Counter{};
    }

    Gauge Gauge::WithDimensions(const std::span<const DimensionValue> dimensions) const {
        Gauge bound = *this;
        return Runtime::BindMetric(instrumentId_, generation_, InstrumentKind::Gauge, dimensions, bound.dimensionValueIds_,
                                   bound.dimensionCount_)
                   ? bound
                   : Gauge{};
    }

    Histogram Histogram::WithDimensions(const std::span<const DimensionValue> dimensions) const {
        Histogram bound = *this;
        return Runtime::BindMetric(instrumentId_, generation_, InstrumentKind::Histogram, dimensions, bound.dimensionValueIds_,
                                   bound.dimensionCount_)
                   ? bound
                   : Histogram{};
    }

    Timing Timing::WithDimensions(const std::span<const DimensionValue> dimensions) const {
        Timing bound = *this;
        return Runtime::BindMetric(instrumentId_, generation_, InstrumentKind::Timing, dimensions, bound.dimensionValueIds_,
                                   bound.dimensionCount_)
                   ? bound
                   : Timing{};
    }

#if HORO_ENABLE_TELEMETRY
    /** @copydoc Counter::Add */
    void Counter::Add(const std::uint64_t delta) const noexcept {
        if (!static_cast<bool>(*this))
            return;
        static_cast<void>(Runtime::RecordMetric(instrumentId_, generation_, InstrumentKind::Counter, static_cast<double>(delta),
                                                dimensionValueIds_, dimensionCount_));
    }

    /** @copydoc Gauge::Set */
    void Gauge::Set(const double value) const noexcept {
        if (!static_cast<bool>(*this))
            return;
        static_cast<void>(
            Runtime::RecordMetric(instrumentId_, generation_, InstrumentKind::Gauge, value, dimensionValueIds_, dimensionCount_));
    }

    /** @copydoc Histogram::Observe */
    void Histogram::Observe(const double value) const noexcept {
        if (!static_cast<bool>(*this))
            return;
        static_cast<void>(
            Runtime::RecordMetric(instrumentId_, generation_, InstrumentKind::Histogram, value, dimensionValueIds_, dimensionCount_));
    }

    /** @copydoc Timing::Record */
    void Timing::Record(const std::chrono::nanoseconds duration) const noexcept {
        if (!static_cast<bool>(*this))
            return;
        static_cast<void>(Runtime::RecordMetric(instrumentId_, generation_, InstrumentKind::Timing,
                                                std::chrono::duration<double>(duration).count(), dimensionValueIds_, dimensionCount_));
    }

    /** @copydoc ScopedTimer::ScopedTimer */
    ScopedTimer::ScopedTimer(const Histogram histogram) noexcept : histogram_(histogram) {}

    /** @copydoc ScopedTimer::ScopedTimer */
    ScopedTimer::ScopedTimer(const Timing timing) noexcept : timing_(timing) {}

    /** @copydoc ScopedTimer::~ScopedTimer */
    ScopedTimer::~ScopedTimer() {
        const auto elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - startedAt_);
        if (histogram_)
            histogram_.Observe(elapsed.count());
        if (timing_)
            timing_.Record(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - startedAt_));
    }
#endif
}  // namespace Horo::Telemetry
