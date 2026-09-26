#pragma once

/**
 * @file NetworkMetrics.h
 * @brief Bounded, backend-neutral network measurements and safe-point publication.
 */

#include "Horo/Foundation/Telemetry/Telemetry.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>

namespace Horo::Network {
    class NetworkIoService;
    /** @brief Fixed measurement categories; no protocol, message, or peer identifier becomes a label. */
    enum class NetworkMetricCategory : std::uint8_t {
        Transport,
        Control,
        Replication,
        Rpc,
        Other,
        Count
    };
    /** @brief Fixed traffic directions. */
    enum class NetworkMetricDirection : std::uint8_t {
        Sent,
        Received,
        Count
    };
    /** @brief Fixed queue kinds. */
    enum class NetworkMetricQueue : std::uint8_t {
        Inbound,
        Outbound,
        Completion,
        Count
    };
    /** @brief Fixed drop reasons. */
    enum class NetworkMetricDrop : std::uint8_t {
        Capacity,
        Invalid,
        Stale,
        Cancelled,
        Backend,
        Count
    };
    /** @brief Fixed failure classes, independent of backend error text. */
    enum class NetworkMetricFailure : std::uint8_t {
        Connect,
        Transport,
        Protocol,
        Authentication,
        Session,
        Replication,
        Count
    };
    /** @brief Fixed committed replication mapping operations, not assumed wire messages. */
    enum class NetworkMetricReplication : std::uint8_t {
        ObjectRegistered,
        ObjectRetired,
        Count
    };

    inline constexpr std::size_t NetworkMetricCategoryCount = static_cast<std::size_t>(NetworkMetricCategory::Count);
    inline constexpr std::size_t NetworkMetricDirectionCount = static_cast<std::size_t>(NetworkMetricDirection::Count);
    inline constexpr std::size_t NetworkMetricQueueCount = static_cast<std::size_t>(NetworkMetricQueue::Count);
    inline constexpr std::size_t NetworkMetricDropCount = static_cast<std::size_t>(NetworkMetricDrop::Count);
    inline constexpr std::size_t NetworkMetricFailureCount = static_cast<std::size_t>(NetworkMetricFailure::Count);
    inline constexpr std::size_t NetworkMetricReplicationCount = static_cast<std::size_t>(NetworkMetricReplication::Count);

    /** @brief Complete numeric owner-safe projection shared by editor, headless and telemetry adapters. */
    struct NetworkMetricSnapshot final {
        std::uint32_t schemaVersion{1};  /**< Fixed projection schema. */
        std::uint64_t ownerGeneration{}; /**< Host-issued owner lifetime. */
        std::uint64_t revision{};        /**< Monotonic safe-point publication. */
        std::array<std::array<std::uint64_t, NetworkMetricCategoryCount>, NetworkMetricDirectionCount> bytes{}; /**< Cumulative bytes. */
        std::array<std::array<std::uint64_t, NetworkMetricCategoryCount>, NetworkMetricDirectionCount>
            messages{};                                                         /**< Cumulative messages. */
        std::array<std::uint64_t, NetworkMetricQueueCount> queueDepth{};        /**< Current owner-observed queue depths. */
        std::array<std::uint64_t, NetworkMetricDropCount> drops{};              /**< Cumulative classified drops. */
        std::array<std::uint64_t, NetworkMetricFailureCount> failures{};        /**< Cumulative terminal failures. */
        std::array<std::uint64_t, NetworkMetricReplicationCount> replication{}; /**< Cumulative committed mapping operations. */
        std::uint64_t packetsLost{};         /**< Cumulative observed loss, meaningful only when lossAvailable. */
        std::uint64_t activeConnections{};   /**< Current transport connection gauge. */
        std::uint64_t rttMilliseconds{};     /**< Mean sample RTT in the current publication window. */
        std::uint64_t invalidObservations{}; /**< Cumulative rejected enum observations. */
        bool rttAvailable{};                 /**< Whether the current window had an RTT sample. */
        bool lossAvailable{};                /**< Whether the source qualifies an actual packet-loss count. */
        bool saturated{};                    /**< Whether a cumulative count reached its integer ceiling. */
        bool enabled{};                      /**< Host elected to collect metrics for this owner. */
        bool closed{};                       /**< Owner was retired after this publication. */
    };

    /** @brief Fixed-capacity, owner-thread measurement accumulator; never retains peer or payload data. */
    class NetworkMetrics final {
    public:
        /** @brief Creates one owner generation on the calling thread.
         * @param generation Nonzero host-issued lifetime generation.
         * @param enabled Whether collection is enabled. Zero generation always disables collection. */
        explicit NetworkMetrics(std::uint64_t generation, bool enabled);
        /** @brief Disables late producer admission even if the host omitted explicit Close. */
        ~NetworkMetrics();
        NetworkMetrics(const NetworkMetrics &) = delete;
        NetworkMetrics &operator=(const NetworkMetrics &) = delete;

        /** @brief Records one admitted message without retaining its payload or identifiers.
         * @param direction Fixed traffic direction.
         * @param category Fixed semantic category; transport is the wire-total category.
         * @param bytes Admitted payload byte count.
         * @return False if disabled, invalid, wrong-thread or closed. */
        [[nodiscard]] bool RecordMessage(NetworkMetricDirection direction, NetworkMetricCategory category, std::uint64_t bytes) noexcept;
        /** @brief Qualifies actual packet-loss measurement, including an observed zero.
         * @param count Number of packets known lost by the owning transport.
         * @return False if disabled, wrong-thread or closed. */
        [[nodiscard]] bool RecordLoss(std::uint64_t count = 1) noexcept;
        /** @brief Records classified dropped work.
         * @param reason Fixed drop reason.
         * @param count Number of dropped packets or completions.
         * @return False if invalid, disabled, wrong-thread or closed. */
        [[nodiscard]] bool RecordDrop(NetworkMetricDrop reason, std::uint64_t count = 1) noexcept;
        /** @brief Records a classified terminal failure observation.
         * @param reason Fixed failure class.
         * @param count Number of terminal failures.
         * @return False if invalid, disabled, wrong-thread or closed. */
        [[nodiscard]] bool RecordFailure(NetworkMetricFailure reason, std::uint64_t count = 1) noexcept;
        /** @brief Records committed replication mapping operations, not assumed wire traffic.
         * @param kind Fixed operation kind.
         * @param count Number of committed operations.
         * @return False if invalid, disabled, wrong-thread or closed. */
        [[nodiscard]] bool RecordReplication(NetworkMetricReplication kind, std::uint64_t count = 1) noexcept;
        /** @brief Sets a queue depth from its owning service at an owner safe point.
         * @param queue Fixed queue kind.
         * @param depth Current queued-item count.
         * @return False if invalid, disabled, wrong-thread or closed. */
        [[nodiscard]] bool SetQueueDepth(NetworkMetricQueue queue, std::uint64_t depth) noexcept;
        /** @brief Sets aggregate live connection count, never retaining handle identities.
         * @param count Current active connections.
         * @return False if disabled, wrong-thread or closed. */
        [[nodiscard]] bool SetActiveConnections(std::uint64_t count) noexcept;
        /** @brief Includes one finite RTT in the current aggregate sample window.
         * @param value Measured round-trip time in milliseconds.
         * @return False if disabled, wrong-thread or closed. */
        [[nodiscard]] bool RecordRttMilliseconds(std::uint64_t value) noexcept;

        /** @brief Publishes one coherent copy to any reader. @return False on wrong thread, closed owner, or exhausted revision. */
        [[nodiscard]] bool Publish() noexcept;
        /** @brief Copies the last published safe-point projection; never reads in-flight counters.
         * @return Coherent fixed-size snapshot, including owner generation and revision. */
        [[nodiscard]] NetworkMetricSnapshot Snapshot() const noexcept;
        /** @brief Cheap owner-thread collection gate for optional adapters. @return False when disabled or closed. */
        [[nodiscard]] bool IsCollecting() const noexcept;
        /** @brief Closes collection and publishes final zero gauges; idempotent on the owner thread.
         * @return False on wrong thread or exhausted revision. */
        [[nodiscard]] bool Close() noexcept;

    private:
        friend class NetworkIoService;
        [[nodiscard]] bool CanRecord() const noexcept;
        [[nodiscard]] bool Invalid() noexcept;
        static void AddSaturating(std::uint64_t &target, std::uint64_t delta, bool &saturated) noexcept;

        const std::thread::id ownerThread_;
        std::shared_ptr<std::atomic<bool>> admission_;
        NetworkMetricSnapshot current_;
        std::uint64_t rttSum_{};
        std::uint64_t rttSamples_{};
        mutable std::mutex publishedMutex_;
        NetworkMetricSnapshot published_;
    };

    /** @brief Pre-bound fixed-series handles registered only by host composition. */
    struct NetworkMetricHandles final {
        std::array<std::array<Telemetry::Counter, NetworkMetricCategoryCount>, NetworkMetricDirectionCount> bytes;
        std::array<std::array<Telemetry::Counter, NetworkMetricCategoryCount>, NetworkMetricDirectionCount> messages;
        std::array<Telemetry::Gauge, NetworkMetricQueueCount> queues;
        std::array<Telemetry::Counter, NetworkMetricDropCount> drops;
        Telemetry::Counter totalDrops;
        std::array<Telemetry::Counter, NetworkMetricFailureCount> failures;
        std::array<Telemetry::Counter, NetworkMetricReplicationCount> replication;
        Telemetry::Counter lost;
        Telemetry::Gauge connections;
        Telemetry::Gauge rtt;
    };

    /** @brief Registers the closed series vocabulary outside transport and replication hot paths.
     * @param level Host-selected collection level; Off leaves handles inert.
     * @return Pre-bound fixed-series handles for safe-point publication. */
    [[nodiscard]] NetworkMetricHandles RegisterNetworkMetricHandles(Telemetry::MetricCollectionLevel level);

    /** @brief Safe-point adapter of cumulative snapshots to pre-bound, backend-neutral telemetry handles. */
    class NetworkMetricPublisher final {
    public:
        /** @brief Binds one owner generation and pre-registered handles on the caller's thread.
         * @param generation Nonzero host-issued metrics owner generation.
         * @param handles Pre-registered fixed-series handles. */
        NetworkMetricPublisher(std::uint64_t generation, NetworkMetricHandles handles) noexcept;
        /** @brief Emits only deltas and gauges for a newer same-owner snapshot.
         * @param snapshot Coherent owner-safe-point projection.
         * @return False for stale, invalid, wrong-thread or retired-owner input. */
        [[nodiscard]] bool Publish(const NetworkMetricSnapshot &snapshot) noexcept;
        /** @brief Stops publication; idempotent on the owner thread. */
        void Close() noexcept;

    private:
        const std::thread::id ownerThread_;
        const std::uint64_t generation_;
        NetworkMetricHandles handles_;
        NetworkMetricSnapshot previous_;
        bool closed_{};
    };
}  // namespace Horo::Network
