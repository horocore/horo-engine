#include "Horo/Network/NetworkMetrics.h"

#include <algorithm>
#include <limits>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>

namespace Horo::Network {
    namespace {
        constexpr std::array<std::string_view, NetworkMetricDirectionCount> Directions{"sent", "received"};
        constexpr std::array<std::string_view, NetworkMetricCategoryCount> Categories{"transport", "control", "replication", "rpc",
                                                                                      "other"};
        constexpr std::array<std::string_view, NetworkMetricQueueCount> Queues{"inbound", "outbound", "completion"};
        constexpr std::array<std::string_view, NetworkMetricDropCount> Drops{"capacity", "invalid", "stale", "cancelled", "backend"};
        constexpr std::array<std::string_view, NetworkMetricFailureCount> Failures{"connect",        "transport", "protocol",
                                                                                   "authentication", "session",   "replication"};
        constexpr std::array<std::string_view, NetworkMetricReplicationCount> Replication{"object_registered", "object_retired"};

        [[nodiscard]] Telemetry::InstrumentDescriptor Descriptor(std::string name, const Telemetry::MetricUnit unit,
                                                                 const Telemetry::InstrumentKind kind) {
            return {.kind = kind,
                    .name = std::move(name),
                    .subsystem = "network",
                    .unit = unit,
                    .description = "Bounded aggregate network measurement",
                    .maxSeries = 1,
                    .minimumCollectionLevel = Telemetry::MetricCollectionLevel::Core};
        }

        [[nodiscard]] Telemetry::Counter Counter(const std::string &name, const Telemetry::MetricUnit unit) {
            return Telemetry::Runtime::RegisterCounter(Descriptor(name, unit, Telemetry::InstrumentKind::Counter));
        }

        [[nodiscard]] Telemetry::Gauge Gauge(const std::string &name, const Telemetry::MetricUnit unit) {
            return Telemetry::Runtime::RegisterGauge(Descriptor(name, unit, Telemetry::InstrumentKind::Gauge));
        }

        void AddDelta(const Telemetry::Counter &counter, const std::uint64_t delta) noexcept {
            if (delta != 0)
                counter.Add(delta);
        }

        template <std::size_t N>
        [[nodiscard]] bool Monotonic(const std::array<std::uint64_t, N> &newer, const std::array<std::uint64_t, N> &older) noexcept {
            for (std::size_t index = 0; index < N; ++index) {
                if (newer[index] < older[index])
                    return false;
            }
            return true;
        }

        [[nodiscard]] bool ValidSuccessor(const NetworkMetricSnapshot &next, const NetworkMetricSnapshot &previous,
                                          const std::uint64_t generation) noexcept {
            if (next.schemaVersion != 1 || next.ownerGeneration != generation || generation == 0 || next.revision <= previous.revision ||
                next.revision == 0 || (previous.revision != 0 && next.enabled != previous.enabled) ||
                (previous.lossAvailable && !next.lossAvailable) || (!next.lossAvailable && next.packetsLost != 0) ||
                (!next.rttAvailable && next.rttMilliseconds != 0) || (previous.saturated && !next.saturated))
                return false;
            if (next.packetsLost < previous.packetsLost || !Monotonic(next.drops, previous.drops) ||
                !Monotonic(next.failures, previous.failures) || !Monotonic(next.replication, previous.replication) ||
                next.invalidObservations < previous.invalidObservations)
                return false;
            for (std::size_t direction = 0; direction < NetworkMetricDirectionCount; ++direction) {
                if (!Monotonic(next.bytes[direction], previous.bytes[direction]) ||
                    !Monotonic(next.messages[direction], previous.messages[direction]))
                    return false;
            }
            if (previous.closed || (next.closed && (next.activeConnections != 0 || next.rttAvailable ||
                                                    std::ranges::any_of(next.queueDepth, [](const auto depth) {
                return depth != 0;
            }))))
                return false;
            return true;
        }
    }  // namespace

    /** @copydoc RegisterNetworkMetricHandles */
    NetworkMetricHandles RegisterNetworkMetricHandles(const Telemetry::MetricCollectionLevel level) {
        NetworkMetricHandles handles{};
        if (level == Telemetry::MetricCollectionLevel::Off)
            return handles;
        for (std::size_t direction = 0; direction < NetworkMetricDirectionCount; ++direction) {
            for (std::size_t category = 0; category < NetworkMetricCategoryCount; ++category) {
                const std::string suffix = category == static_cast<std::size_t>(NetworkMetricCategory::Transport)
                                               ? std::string{}
                                               : "." + std::string{Categories[category]};
                handles.bytes[direction][category] =
                    Counter("net.bytes_" + std::string{Directions[direction]} + suffix, Telemetry::MetricUnit::Bytes);
                handles.messages[direction][category] =
                    Counter("net.messages_" + std::string{Directions[direction]} + suffix, Telemetry::MetricUnit::Count);
            }
        }
        for (std::size_t index = 0; index < NetworkMetricQueueCount; ++index)
            handles.queues[index] = Gauge("net." + std::string{Queues[index]} + "_queue_depth", Telemetry::MetricUnit::Count);
        for (std::size_t index = 0; index < NetworkMetricDropCount; ++index)
            handles.drops[index] = Counter("net.packets_dropped." + std::string{Drops[index]}, Telemetry::MetricUnit::Count);
        handles.totalDrops = Counter("net.packets_dropped", Telemetry::MetricUnit::Count);
        for (std::size_t index = 0; index < NetworkMetricFailureCount; ++index)
            handles.failures[index] = Counter("net.failures." + std::string{Failures[index]}, Telemetry::MetricUnit::Count);
        for (std::size_t index = 0; index < NetworkMetricReplicationCount; ++index)
            handles.replication[index] = Counter("net.replication." + std::string{Replication[index]}, Telemetry::MetricUnit::Count);
        handles.lost = Counter("net.packets_lost", Telemetry::MetricUnit::Count);
        handles.connections = Gauge("net.active_connections", Telemetry::MetricUnit::Count);
        handles.rtt = Gauge("net.rtt_ms", Telemetry::MetricUnit::Count);
        return handles;
    }

    /** @copydoc NetworkMetricPublisher::NetworkMetricPublisher */
    NetworkMetricPublisher::NetworkMetricPublisher(const std::uint64_t generation, NetworkMetricHandles handles) noexcept
        : ownerThread_(std::this_thread::get_id()), generation_(generation), handles_(std::move(handles)) {
        previous_.ownerGeneration = generation;
    }

    /** @copydoc NetworkMetricPublisher::Publish */
    bool NetworkMetricPublisher::Publish(const NetworkMetricSnapshot &snapshot) noexcept {
        if (std::this_thread::get_id() != ownerThread_ || closed_ || !ValidSuccessor(snapshot, previous_, generation_))
            return false;
        if (snapshot.enabled) {
            std::uint64_t totalDropDelta{};
            for (std::size_t direction = 0; direction < NetworkMetricDirectionCount; ++direction) {
                for (std::size_t category = 0; category < NetworkMetricCategoryCount; ++category) {
                    AddDelta(handles_.bytes[direction][category],
                             snapshot.bytes[direction][category] - previous_.bytes[direction][category]);
                    AddDelta(handles_.messages[direction][category],
                             snapshot.messages[direction][category] - previous_.messages[direction][category]);
                }
            }
            for (std::size_t index = 0; index < NetworkMetricQueueCount; ++index)
                handles_.queues[index].Set(static_cast<double>(snapshot.queueDepth[index]));
            for (std::size_t index = 0; index < NetworkMetricDropCount; ++index) {
                const auto delta = snapshot.drops[index] - previous_.drops[index];
                AddDelta(handles_.drops[index], delta);
                totalDropDelta = delta > std::numeric_limits<std::uint64_t>::max() - totalDropDelta
                                     ? std::numeric_limits<std::uint64_t>::max()
                                     : totalDropDelta + delta;
            }
            AddDelta(handles_.totalDrops, totalDropDelta);
            for (std::size_t index = 0; index < NetworkMetricFailureCount; ++index)
                AddDelta(handles_.failures[index], snapshot.failures[index] - previous_.failures[index]);
            for (std::size_t index = 0; index < NetworkMetricReplicationCount; ++index)
                AddDelta(handles_.replication[index], snapshot.replication[index] - previous_.replication[index]);
            if (snapshot.lossAvailable)
                AddDelta(handles_.lost, snapshot.packetsLost - previous_.packetsLost);
            handles_.connections.Set(static_cast<double>(snapshot.activeConnections));
            if (snapshot.rttAvailable)
                handles_.rtt.Set(static_cast<double>(snapshot.rttMilliseconds));
            else if (snapshot.closed)
                handles_.rtt.Set(0);
        }
        previous_ = snapshot;
        if (snapshot.closed)
            closed_ = true;
        return true;
    }

    /** @copydoc NetworkMetricPublisher::Close */
    void NetworkMetricPublisher::Close() noexcept {
        if (std::this_thread::get_id() == ownerThread_)
            closed_ = true;
    }
}  // namespace Horo::Network
