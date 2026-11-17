#pragma once
/** @file PrefabMetrics.h @brief Explicit low-cardinality prefab measurements over process-owned Foundation Telemetry. */
#include "Horo/Foundation/Result.h"
#include "Horo/Foundation/Telemetry/Telemetry.h"

#include <array>
#include <optional>

namespace Horo::Prefab {
    /** @brief Closed canonical pipeline boundaries; identities and user data never become dimensions. */
    enum class PrefabMetricPhase : std::uint8_t {
        Parse,
        Graph,
        Resolve,
        Expand,
        Cache,
        Cook,
        Rebase,
        Spawn,
        Count
    };
    /** @brief Mutually exclusive terminal attempt outcomes. */
    enum class PrefabMetricOutcome : std::uint8_t {
        Success,
        Failed,
        Cancelled,
        Overflow,
        Count
    };
    /** @brief Actual external cache lookup/storage outcomes; internal deduplication is excluded. */
    enum class PrefabMetricCacheEvent : std::uint8_t {
        Hit,
        Miss,
        Evicted,
        Rejected,
        Count
    };
    /** @brief Bounded measurement kinds; byte instruments remain distinct from count instruments. */
    enum class PrefabMetricItem : std::uint8_t {
        SourceObjects,
        ExpandedObjects,
        Components,
        GraphNodes,
        GraphEdges,
        Overrides,
        RuntimeEntities,
        SourceBytes,
        CookedBytes,
        CacheBytes,
        Count
    };

    inline constexpr std::size_t PrefabMetricPhaseCount = static_cast<std::size_t>(PrefabMetricPhase::Count);
    inline constexpr std::size_t PrefabMetricOutcomeCount = static_cast<std::size_t>(PrefabMetricOutcome::Count);
    inline constexpr std::size_t PrefabMetricItemCount = static_cast<std::size_t>(PrefabMetricItem::Count);

    /** @brief Owned numeric prebound handles, safe to copy into immutable work and retire after host shutdown.
     * @details Registration generation is observation evidence only, excluded from source/cook/cache identity.
     * Stale handles are dropped by Telemetry. No registry, logger, sink, callback or host pointer is retained.
     */
    struct PrefabMetricHandles final {
        std::array<std::array<Telemetry::Counter, PrefabMetricOutcomeCount>, PrefabMetricPhaseCount> attempts;
        std::array<Telemetry::Timing, PrefabMetricPhaseCount> durations;
        std::array<Telemetry::Histogram, PrefabMetricItemCount> items;
        std::array<Telemetry::Counter, static_cast<std::size_t>(PrefabMetricCacheEvent::Count)> cache;
        bool logicalSpans{};
    };

    /** @brief Registers finite descriptors and binds every allowed series explicitly at host composition.
     * @param level Maximum requested collection level; process Telemetry policy remains authoritative.
     * @param logicalSpans Whether Detailed policy may emit logical terminal SpanRecord events.
     * @return Numeric owned handles, or empty configuration when disabled/unavailable. Never selects a sink/backend.
     */
    [[nodiscard]] PrefabMetricHandles RegisterPrefabMetricHandles(Telemetry::MetricCollectionLevel level,
                                                                  bool logicalSpans = false) noexcept;
    /** @brief Records a genuine external cache outcome without lookup, allocation or exception.
     * @param handles Host-prebound configuration. @param event Closed actual outcome. @param count Actual occurrences.
     */
    void RecordPrefabCacheEvent(const PrefabMetricHandles &handles, PrefabMetricCacheEvent event, std::uint64_t count = 1) noexcept;

    /** @brief One bounded attempt, prepared before work and completed exactly once without changing its authoritative result.
     * @details Disabled construction performs no clock/span/string/MDC work. Enabled construction may prepare an owned
     * empty-context logical span; completion moves it to Telemetry without building fields or copying MDC.
     * Move ownership explicitly into asynchronous requests; no borrowed configuration survives construction.
     */
    class PrefabMetricOperation final {
    public:
        /** @brief Constructs an inert disabled observer without clock or allocation work. */
        PrefabMetricOperation() noexcept = default;
        /** @brief Captures numeric handles and optionally prepares a bounded logical span.
         * @param handles Explicit configuration borrowed only during construction. @param phase Canonical boundary.
         */
        PrefabMetricOperation(const PrefabMetricHandles &handles, PrefabMetricPhase phase) noexcept;
        PrefabMetricOperation(const PrefabMetricOperation &) = delete;
        PrefabMetricOperation &operator=(const PrefabMetricOperation &) = delete;
        /** @brief Transfers sole completion ownership. @param other Owned prepared observation. */
        PrefabMetricOperation(PrefabMetricOperation &&other) noexcept;
        /** @brief Retires an incomplete replaced attempt and transfers ownership.
         * @param other Owned prepared observation. @return This observer. */
        PrefabMetricOperation &operator=(PrefabMetricOperation &&other) noexcept;
        /** @brief Records an uncompleted attempt as failed; never throws or changes engine publication. */
        ~PrefabMetricOperation();

        /** @brief Reports active numeric observation without clock reads. @return Whether this attempt was enabled. */
        [[nodiscard]] bool Enabled() const noexcept {
            return active_ && !completed_;
        }

        /** @brief Records one existing bounded model count/byte size without traversing or retaining that model.
         * @param item Closed item/unit kind. @param value Actual admitted or attempted size.
         */
        void Observe(PrefabMetricItem item, std::uint64_t value) const noexcept;
        /** @brief Completes one attempt once; later calls are no-ops.
         * @param outcome Explicit authoritative outcome. */
        void Complete(PrefabMetricOutcome outcome = PrefabMetricOutcome::Success) noexcept;
        /** @brief Preserves typed failure classification; raw errors never become metric dimensions or span fields.
         * @param error Existing authoritative typed failure. */
        void Fail(const Error &error) noexcept;

    private:
        std::array<Telemetry::Counter, PrefabMetricOutcomeCount> outcomes_;
        Telemetry::Timing duration_;
        std::array<Telemetry::Histogram, PrefabMetricItemCount> items_;
        std::chrono::steady_clock::time_point started_{};
        std::optional<Telemetry::Record> span_;
        std::uint32_t generation_{};
        bool active_{};
        bool timed_{};
        bool completed_{};
    };
}  // namespace Horo::Prefab
