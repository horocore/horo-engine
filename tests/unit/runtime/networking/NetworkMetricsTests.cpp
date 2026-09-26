#include "Horo/Network/NetworkErrors.h"
#include "Horo/Network/NetworkMetricTransport.h"
#include "Horo/Network/NetworkMetrics.h"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace Horo::Network {
    namespace {
        constexpr auto Sent = static_cast<std::size_t>(NetworkMetricDirection::Sent);
        constexpr auto Received = static_cast<std::size_t>(NetworkMetricDirection::Received);
        constexpr auto Wire = static_cast<std::size_t>(NetworkMetricCategory::Transport);

        class FakeTransport final : public INetworkTransport {
        public:
            Result<void> Initialize(const NetworkTransportConfig &) override {
                return Result<void>::Success();
            }

            Result<ListenerHandle> Listen(const NetworkListenRequest &) override {
                return Result<ListenerHandle>::Failure(MakeError(NetworkErrors::TransportCapabilityUnavailable));
            }

            Result<ConnectionHandle> Connect(const NetworkConnectRequest &) override {
                return Result<ConnectionHandle>::Success(ConnectionHandle::Create(1, 1).Value());
            }

            Result<void> Send(ConnectionHandle, ChannelId, std::span<const std::byte>, DeliveryPolicy) override {
                if (backpressure)
                    return Result<void>::Failure(MakeError(NetworkErrors::TransportReliableBackpressure));
                return failSend ? Result<void>::Failure(MakeError(NetworkErrors::TransportCapabilityUnavailable)) : Result<void>::Success();
            }

            Result<void> Close(ConnectionHandle) override {
                return Result<void>::Success();
            }

            Result<void> CloseListener(ListenerHandle) override {
                return Result<void>::Success();
            }

            Result<std::size_t> PollEvents(INetworkTransportEventConsumer &consumer) override {
                NetworkTransportEvent event;
                event.kind = NetworkTransportEventKind::PacketReceived;
                event.payload = {std::byte{1}, std::byte{2}, std::byte{3}};
                consumer.Consume(std::move(event));
                return Result<std::size_t>::Success(1);
            }

            NetworkTransportStats Stats() const noexcept override {
                ++statsCalls;
                return {.activeConnections = 1};
            }

            Result<NetworkConnectionStats> ConnectionStats(ConnectionHandle) const override {
                return Result<NetworkConnectionStats>::Success({.pingMilliseconds = 25});
            }

            TransportCapabilities Capabilities() const noexcept override {
                return {};
            }

            void Shutdown() noexcept override {
                stopped = true;
            }

            bool failSend{};
            bool backpressure{};
            bool stopped{};
            mutable std::size_t statsCalls{};
        };

        class CountingConsumer final : public INetworkTransportEventConsumer {
        public:
            void Consume(NetworkTransportEvent) noexcept override {
                ++count;
            }

            std::size_t count{};
        };

        class MetricSink final : public Telemetry::ISink {
        public:
            void Export(const Telemetry::Record &record, const Telemetry::InstrumentDescriptor *descriptor) override {
                if (record.Kind() != Telemetry::RecordKind::Metric || !descriptor)
                    return;
                std::scoped_lock lock{mutex};
                names.push_back(descriptor->name);
                bounded = bounded && descriptor->dimensions.empty() && descriptor->maxSeries == 1;
            }

            void Flush() override {}

            std::mutex mutex;
            std::vector<std::string> names;
            bool bounded{true};
        };
    }  // namespace

    TEST_CASE("network metric categories remain fixed under hostile enum values", "[network][metrics]") {
        NetworkMetrics metrics{7, true};
        for (std::size_t index = 0; index < 10'000; ++index) {
            REQUIRE_FALSE(metrics.RecordMessage(NetworkMetricDirection::Received, static_cast<NetworkMetricCategory>(255), 8));
            REQUIRE_FALSE(metrics.RecordDrop(static_cast<NetworkMetricDrop>(255)));
        }
        REQUIRE(metrics.RecordMessage(NetworkMetricDirection::Received, NetworkMetricCategory::Other, 4));
        REQUIRE(metrics.Publish());
        const auto snapshot = metrics.Snapshot();
        REQUIRE(snapshot.invalidObservations == 20'000);
        REQUIRE(snapshot.messages[Received][static_cast<std::size_t>(NetworkMetricCategory::Other)] == 1);
        REQUIRE(snapshot.bytes[Received][static_cast<std::size_t>(NetworkMetricCategory::Other)] == 4);
    }

    TEST_CASE("network metrics saturate without wrapping or allocating series", "[network][metrics]") {
        NetworkMetrics metrics{8, true};
        REQUIRE(metrics.RecordLoss(std::numeric_limits<std::uint64_t>::max()));
        REQUIRE(metrics.RecordLoss());
        REQUIRE(metrics.RecordDrop(NetworkMetricDrop::Capacity, std::numeric_limits<std::uint64_t>::max()));
        REQUIRE(metrics.RecordDrop(NetworkMetricDrop::Capacity));
        REQUIRE(metrics.Publish());
        const auto snapshot = metrics.Snapshot();
        REQUIRE(snapshot.saturated);
        REQUIRE(snapshot.packetsLost == std::numeric_limits<std::uint64_t>::max());
        REQUIRE(snapshot.drops[static_cast<std::size_t>(NetworkMetricDrop::Capacity)] == std::numeric_limits<std::uint64_t>::max());
    }

    TEST_CASE("published network snapshots fence owner lifetime and concurrent readers", "[network][metrics]") {
        NetworkMetrics metrics{9, true};
        NetworkMetricPublisher publisher{9, {}};
        REQUIRE(metrics.RecordReplication(NetworkMetricReplication::ObjectRegistered, 3));
        REQUIRE(metrics.SetQueueDepth(NetworkMetricQueue::Completion, 4));
        REQUIRE(metrics.Publish());
        const auto first = metrics.Snapshot();
        REQUIRE(publisher.Publish(first));
        REQUIRE_FALSE(publisher.Publish(first));
        auto wrongGeneration = first;
        wrongGeneration.ownerGeneration = 10;
        wrongGeneration.revision = 2;
        REQUIRE_FALSE(publisher.Publish(wrongGeneration));
        auto rolledBack = first;
        rolledBack.revision = 2;
        rolledBack.replication[static_cast<std::size_t>(NetworkMetricReplication::ObjectRegistered)] = 0;
        REQUIRE_FALSE(publisher.Publish(rolledBack));
        auto fabricatedLoss = first;
        fabricatedLoss.revision = 2;
        fabricatedLoss.packetsLost = 1;
        REQUIRE_FALSE(publisher.Publish(fabricatedLoss));

        bool wrongThreadRecord = true;
        NetworkMetricSnapshot readerSnapshot;
        std::thread reader{[&] {
            wrongThreadRecord = metrics.RecordLoss();
            readerSnapshot = metrics.Snapshot();
        }};
        reader.join();
        REQUIRE_FALSE(wrongThreadRecord);
        REQUIRE(readerSnapshot.revision == first.revision);
        REQUIRE(readerSnapshot.queueDepth[static_cast<std::size_t>(NetworkMetricQueue::Completion)] == 4);

        REQUIRE(metrics.Close());
        REQUIRE(metrics.Close());
        const auto final = metrics.Snapshot();
        REQUIRE(final.closed);
        REQUIRE(final.queueDepth[static_cast<std::size_t>(NetworkMetricQueue::Completion)] == 0);
        REQUIRE(publisher.Publish(final));
        REQUIRE_FALSE(publisher.Publish(final));
        REQUIRE_FALSE(metrics.RecordLoss());
        REQUIRE_FALSE(metrics.Publish());
    }

    TEST_CASE("disabled network metrics do no measurement work", "[network][metrics]") {
        NetworkMetrics metrics{10, false};
        REQUIRE_FALSE(metrics.RecordMessage(NetworkMetricDirection::Sent, NetworkMetricCategory::Transport, 42));
        REQUIRE_FALSE(metrics.RecordFailure(NetworkMetricFailure::Transport));
        REQUIRE(metrics.Publish());
        const auto snapshot = metrics.Snapshot();
        REQUIRE_FALSE(snapshot.enabled);
        REQUIRE(snapshot.invalidObservations == 0);
        REQUIRE(snapshot.messages[Sent][Wire] == 0);
        NetworkMetricPublisher publisher{10, {}};
        REQUIRE(publisher.Publish(snapshot));
    }

    TEST_CASE("composed transport measures admitted wire traffic without peer labels", "[network][metrics]") {
        NetworkMetrics metrics{11, true};
        auto fake = std::make_unique<FakeTransport>();
        auto *backend = fake.get();
        NetworkMetricTransport transport{std::move(fake), metrics};
        const auto connection = ConnectionHandle::Create(1, 1).Value();
        const std::array payload{std::byte{1}, std::byte{2}};
        REQUIRE(transport.Send(connection, ChannelId{}, payload, DeliveryPolicy::UnreliableUnordered).HasValue());
        backend->failSend = true;
        REQUIRE(transport.Send(connection, ChannelId{}, payload, DeliveryPolicy::UnreliableUnordered).HasError());
        backend->backpressure = true;
        REQUIRE(transport.Send(connection, ChannelId{}, payload, DeliveryPolicy::ReliableOrdered).HasError());
        CountingConsumer consumer;
        REQUIRE(transport.PollEvents(consumer).Value() == 1);
        REQUIRE(consumer.count == 1);
        REQUIRE(transport.ConnectionStats(connection).Value().pingMilliseconds == 25);
        REQUIRE(metrics.Publish());
        const auto snapshot = metrics.Snapshot();
        REQUIRE(snapshot.messages[Sent][Wire] == 1);
        REQUIRE(snapshot.bytes[Sent][Wire] == 2);
        REQUIRE(snapshot.messages[Received][Wire] == 1);
        REQUIRE(snapshot.bytes[Received][Wire] == 3);
        REQUIRE(snapshot.drops[static_cast<std::size_t>(NetworkMetricDrop::Capacity)] == 1);
        REQUIRE(snapshot.failures[static_cast<std::size_t>(NetworkMetricFailure::Transport)] == 1);
        REQUIRE(snapshot.activeConnections == 1);
        REQUIRE(snapshot.rttAvailable);
        REQUIRE(snapshot.rttMilliseconds == 25);
        transport.Shutdown();
        REQUIRE(backend->stopped);
    }

    TEST_CASE("disabled transport instrumentation avoids extra statistics sampling", "[network][metrics]") {
        NetworkMetrics metrics{12, false};
        auto fake = std::make_unique<FakeTransport>();
        auto *backend = fake.get();
        NetworkMetricTransport transport{std::move(fake), metrics};
        CountingConsumer consumer;
        REQUIRE(transport.PollEvents(consumer).Value() == 1);
        REQUIRE(backend->statsCalls == 0);
        const auto connection = ConnectionHandle::Create(1, 1).Value();
        REQUIRE(transport.ConnectionStats(connection).HasValue());
        REQUIRE_FALSE(metrics.IsCollecting());
        REQUIRE(metrics.Publish());
        REQUIRE(metrics.Snapshot().messages[Received][Wire] == 0);
    }

    TEST_CASE("network snapshots publish through pre-bound backend-neutral telemetry", "[network][metrics]") {
        Telemetry::Runtime::Shutdown();
        auto sink = std::make_shared<MetricSink>();
        REQUIRE(
            Telemetry::Runtime::Initialize({.queueCapacity = 256, .metricCollectionLevel = Telemetry::MetricCollectionLevel::Core}, sink));
        auto handles = RegisterNetworkMetricHandles(Telemetry::MetricCollectionLevel::Core);
        REQUIRE(static_cast<bool>(handles.bytes[Sent][Wire]));
        REQUIRE(static_cast<bool>(handles.totalDrops));
        REQUIRE(static_cast<bool>(handles.lost));
        REQUIRE(Telemetry::Runtime::GetStatistics().invalidInstrumentRegistrations == 0);
        NetworkMetricPublisher publisher{13, std::move(handles)};
        NetworkMetrics metrics{13, true};
        const auto hasRequiredNames = [&sink] {
            std::scoped_lock lock{sink->mutex};
            for (const auto name : {"net.bytes_sent", "net.packets_lost", "net.packets_dropped", "net.active_connections", "net.rtt_ms"}) {
                if (std::ranges::find(sink->names, name) == sink->names.end())
                    return false;
            }
            return true;
        };
        // The runtime queue is intentionally best-effort under writer contention. Retry
        // bounded safe-point samples instead of assuming every single enqueue survives.
        std::size_t publications{};
        for (; publications < 16 && !hasRequiredNames(); ++publications) {
            REQUIRE(metrics.RecordMessage(NetworkMetricDirection::Sent, NetworkMetricCategory::Transport, 17));
            REQUIRE(metrics.RecordDrop(NetworkMetricDrop::Capacity));
            REQUIRE(metrics.RecordLoss(2));
            REQUIRE(metrics.SetActiveConnections(1));
            REQUIRE(metrics.RecordRttMilliseconds(30));
            REQUIRE(metrics.Publish());
            const auto snapshot = metrics.Snapshot();
            REQUIRE(snapshot.revision == publications + 1);
            REQUIRE(snapshot.messages[Sent][Wire] == publications + 1);
            REQUIRE(snapshot.packetsLost == 2 * (publications + 1));
            REQUIRE(publisher.Publish(snapshot));
            REQUIRE(Telemetry::Runtime::Flush());
        }
        const auto statistics = Telemetry::Runtime::GetStatistics();
        CAPTURE(publications, statistics.acceptedRecords, statistics.exportedRecords, statistics.droppedRecords, statistics.contentionDrops,
                statistics.queueFullDrops, statistics.invalidInstrumentRegistrations);
        std::vector<std::string> observedNames;
        {
            std::scoped_lock lock{sink->mutex};
            observedNames = sink->names;
        }
        CAPTURE(observedNames);
        REQUIRE(hasRequiredNames());
        {
            std::scoped_lock lock{sink->mutex};
            REQUIRE(sink->bounded);
        }
        REQUIRE(metrics.Close());
        REQUIRE(publisher.Publish(metrics.Snapshot()));
        REQUIRE(Telemetry::Runtime::Shutdown());
    }
}  // namespace Horo::Network
