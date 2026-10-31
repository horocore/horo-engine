#include "Horo/Prefab/PrefabMetrics.h"

#include "Horo/Foundation/Telemetry/Operation.h"
#include "Horo/Prefab/PrefabErrors.h"

#include <algorithm>
#include <utility>

namespace Horo::Prefab {
    namespace {
        constexpr std::array<std::string_view, PrefabMetricPhaseCount> Phases{"parse", "graph", "resolve", "expand",
                                                                              "cache", "cook",  "rebase",  "spawn"};
        constexpr std::array<std::string_view, PrefabMetricOutcomeCount> Outcomes{"success", "failed", "cancelled", "overflow"};
        constexpr std::array<std::string_view, 7> Counts{"source_objects", "expanded_objects", "components",      "graph_nodes",
                                                         "graph_edges",    "overrides",        "runtime_entities"};
        constexpr std::array<std::string_view, 3> Bytes{"source", "cooked", "cache"};
        constexpr std::array<std::string_view, 4> Cache{"hit", "miss", "evicted", "rejected"};

        /** @brief Builds descriptor vocabulary only during explicit host registration. */
        template <std::size_t Size>
        Telemetry::DimensionDescriptor Dimension(const char *name, const std::array<std::string_view, Size> &values) {
            Telemetry::DimensionDescriptor dimension{.key = name};
            for (const auto value : values)
                dimension.allowedValues.emplace_back(value);
            return dimension;
        }

        /** @brief Constructs an immutable descriptor with a declared finite series budget. */
        Telemetry::InstrumentDescriptor Descriptor(const char *name, Telemetry::MetricUnit unit, std::uint32_t series,
                                                   std::vector<Telemetry::DimensionDescriptor> dimensions,
                                                   Telemetry::MetricCollectionLevel level = Telemetry::MetricCollectionLevel::Core) {
            return {.name = name,
                    .subsystem = "prefab",
                    .unit = unit,
                    .description = "Bounded canonical prefab pipeline observation.",
                    .dimensions = std::move(dimensions),
                    .maxSeries = series,
                    .minimumCollectionLevel = level};
        }

        /** @brief Recognizes only existing precise prefab resource denials as overflow. */
        bool Overflow(const Error &error) noexcept {
            const std::array descriptors{&PrefabErrors::WorkBudgetExceeded,
                                         &PrefabErrors::ObjectCountExceeded,
                                         &PrefabErrors::ComponentCountExceeded,
                                         &PrefabErrors::PayloadTooLarge,
                                         &PrefabErrors::ReferenceCountExceeded,
                                         &PrefabErrors::NestedPlacementCountExceeded,
                                         &PrefabErrors::HierarchyDepthExceeded,
                                         &PrefabErrors::CookPayloadTooLarge,
                                         &PrefabErrors::ExpansionCacheCapacityExceeded,
                                         &PrefabErrors::EntityAllocationExhausted,
                                         &PrefabErrors::SpawnDepthExceeded,
                                         &PrefabErrors::VariantDepthExceeded,
                                         &PrefabErrors::CompositionDepthExceeded,
                                         &PrefabErrors::CompositionEdgeLimitExceeded};
            return std::ranges::any_of(descriptors, [&](const auto *descriptor) {
                return error.domain.Value() == descriptor->domain.Value() && error.code.Value() == descriptor->code.Value();
            });
        }
    }  // namespace

    /** @copydoc RegisterPrefabMetricHandles */
    PrefabMetricHandles RegisterPrefabMetricHandles(const Telemetry::MetricCollectionLevel level, const bool logicalSpans) noexcept {
        try {
            PrefabMetricHandles handles;
#if HORO_ENABLE_TELEMETRY
            if (level == Telemetry::MetricCollectionLevel::Off || level > Telemetry::MetricCollectionLevel::Detailed ||
                !Telemetry::Runtime::IsEnabled())
                return handles;
            const auto attempts =
                Telemetry::Runtime::RegisterCounter(Descriptor("horo.prefab.operation", Telemetry::MetricUnit::Count, 32,
                                                               {Dimension("phase", Phases), Dimension("outcome", Outcomes)}));
            for (std::size_t phase = 0; phase < Phases.size(); ++phase) {
                for (std::size_t outcome = 0; outcome < Outcomes.size(); ++outcome) {
                    const std::array dimensions{Telemetry::DimensionValue{"phase", Phases[phase]},
                                                Telemetry::DimensionValue{"outcome", Outcomes[outcome]}};
                    handles.attempts[phase][outcome] = attempts.WithDimensions(dimensions);
                }
            }
            if (level == Telemetry::MetricCollectionLevel::Detailed) {
                const auto timing = Telemetry::Runtime::RegisterTiming(Descriptor("horo.prefab.duration", Telemetry::MetricUnit::Seconds, 8,
                                                                                  {Dimension("phase", Phases)},
                                                                                  Telemetry::MetricCollectionLevel::Detailed));
                for (std::size_t phase = 0; phase < Phases.size(); ++phase) {
                    const std::array dimensions{Telemetry::DimensionValue{"phase", Phases[phase]}};
                    handles.durations[phase] = timing.WithDimensions(dimensions);
                }
            }
            const auto counts = Telemetry::Runtime::RegisterHistogram(
                Descriptor("horo.prefab.items", Telemetry::MetricUnit::Count, 7, {Dimension("kind", Counts)}));
            const auto bytes = Telemetry::Runtime::RegisterHistogram(
                Descriptor("horo.prefab.bytes", Telemetry::MetricUnit::Bytes, 3, {Dimension("kind", Bytes)}));
            for (std::size_t item = 0; item < handles.items.size(); ++item) {
                const bool isBytes = item >= Counts.size();
                const std::array dimensions{Telemetry::DimensionValue{"kind", isBytes ? Bytes[item - Counts.size()] : Counts[item]}};
                handles.items[item] = (isBytes ? bytes : counts).WithDimensions(dimensions);
            }
            const auto cache = Telemetry::Runtime::RegisterCounter(
                Descriptor("horo.prefab.cache", Telemetry::MetricUnit::Count, 4, {Dimension("outcome", Cache)}));
            for (std::size_t index = 0; index < Cache.size(); ++index) {
                const std::array dimensions{Telemetry::DimensionValue{"outcome", Cache[index]}};
                handles.cache[index] = cache.WithDimensions(dimensions);
            }
            handles.logicalSpans = logicalSpans && static_cast<bool>(handles.durations[0]);
#else
            static_cast<void>(level);
            static_cast<void>(logicalSpans);
#endif
            return handles;
        } catch (...) {
            return {};
        }
    }

    /** @copydoc RecordPrefabCacheEvent */
    void RecordPrefabCacheEvent(const PrefabMetricHandles &handles, const PrefabMetricCacheEvent event,
                                const std::uint64_t count) noexcept {
        if (static_cast<std::size_t>(event) < handles.cache.size())
            handles.cache[static_cast<std::size_t>(event)].Add(count);
    }

    /** @copydoc PrefabMetricOperation::PrefabMetricOperation */
    PrefabMetricOperation::PrefabMetricOperation(const PrefabMetricHandles &handles, const PrefabMetricPhase phase) noexcept {
#if HORO_ENABLE_TELEMETRY
        const auto index = static_cast<std::size_t>(phase);
        if (index >= Phases.size() || !handles.attempts[index][0] || !Telemetry::Runtime::IsEnabled() ||
            handles.attempts[index][0].RuntimeGeneration() != Telemetry::Runtime::Generation())
            return;
        active_ = true;
        generation_ = handles.attempts[index][0].RuntimeGeneration();
        outcomes_ = handles.attempts[index];
        duration_ = handles.durations[index];
        items_ = handles.items;
        const bool spans = handles.logicalSpans && Telemetry::Runtime::IsEventEnabled("prefab", Log::Level::Trace);
        timed_ = static_cast<bool>(duration_) || spans;
        if (timed_)
            started_ = std::chrono::steady_clock::now();
        if (spans) {
            try {
                const auto parent = Telemetry::CaptureOperationIdentity();
                span_.emplace(Telemetry::Record{.subsystem = "prefab",
                                                .payload = Telemetry::SpanRecord{.operationId = Telemetry::AllocateOperationId(),
                                                                                 .parentOperationId = parent.operationId,
                                                                                 .name = "prefab." + std::string{Phases[index]}}});
            } catch (...) { /* Observation preparation cannot change authoritative work. */
            }
        }
#else
        static_cast<void>(handles);
        static_cast<void>(phase);
#endif
    }

    /** @copydoc PrefabMetricOperation::PrefabMetricOperation */
    PrefabMetricOperation::PrefabMetricOperation(PrefabMetricOperation &&other) noexcept {
        *this = std::move(other);
    }

    /** @copydoc PrefabMetricOperation::operator= */
    PrefabMetricOperation &PrefabMetricOperation::operator=(PrefabMetricOperation &&other) noexcept {
        if (this != &other) {
            if (active_ && !completed_)
                Complete(PrefabMetricOutcome::Failed);
            outcomes_ = other.outcomes_;
            duration_ = other.duration_;
            items_ = other.items_;
            generation_ = other.generation_;
            started_ = other.started_;
            span_ = std::move(other.span_);
            active_ = other.active_;
            timed_ = other.timed_;
            completed_ = other.completed_;
            other.active_ = false;
        }
        return *this;
    }

    /** @copydoc PrefabMetricOperation::~PrefabMetricOperation */
    PrefabMetricOperation::~PrefabMetricOperation() {
        if (active_ && !completed_)
            Complete(PrefabMetricOutcome::Failed);
    }

    /** @copydoc PrefabMetricOperation::Observe */
    void PrefabMetricOperation::Observe(const PrefabMetricItem item, const std::uint64_t value) const noexcept {
        if (Enabled() && static_cast<std::size_t>(item) < items_.size())
            items_[static_cast<std::size_t>(item)].Observe(static_cast<double>(value));
    }

    /** @copydoc PrefabMetricOperation::Complete */
    void PrefabMetricOperation::Complete(const PrefabMetricOutcome outcome) noexcept {
        if (!Enabled() || static_cast<std::size_t>(outcome) >= outcomes_.size())
            return;
        completed_ = true;
        outcomes_[static_cast<std::size_t>(outcome)].Add();
        if (!Telemetry::Runtime::IsEnabled() || generation_ != Telemetry::Runtime::Generation()) {
            span_.reset();
            return;
        }
        const auto elapsed = timed_ ? std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - started_)
                                    : std::chrono::nanoseconds{};
        if (duration_)
            duration_.Record(elapsed);
        if (span_ && generation_ == Telemetry::Runtime::Generation()) {
            auto &span = std::get<Telemetry::SpanRecord>(span_->payload);
            span.duration = elapsed;
            span.status = outcome == PrefabMetricOutcome::Success     ? Telemetry::SpanStatus::Succeeded
                          : outcome == PrefabMetricOutcome::Cancelled ? Telemetry::SpanStatus::Cancelled
                                                                      : Telemetry::SpanStatus::Failed;
            static_cast<void>(Telemetry::Runtime::EmitRecord(std::move(*span_), generation_));
        }
        span_.reset();
    }

    /** @copydoc PrefabMetricOperation::Fail */
    void PrefabMetricOperation::Fail(const Error &error) noexcept {
        Complete(error.domain.Value() == PrefabErrors::Cancelled.domain.Value() &&
                         error.code.Value() == PrefabErrors::Cancelled.code.Value()
                     ? PrefabMetricOutcome::Cancelled
                 : Overflow(error) ? PrefabMetricOutcome::Overflow
                                   : PrefabMetricOutcome::Failed);
    }
}  // namespace Horo::Prefab
