#pragma once
/** @file ChaosScenarioTransport.h @brief Test-only paired normalization over the sole deterministic impairment owner. */
#include "Horo/Network/NetworkChaosScenario.h"
#include "NetworkAdmissionAuthorityTestSupport.h"

#include <algorithm>

namespace Horo::Network::ChaosTests {
    /** @brief One bounded owned wire and provider-evidence state; no second impairment scheduler. */
    struct Wire final {
        std::shared_ptr<AdmissionTests::Link> evidence{std::make_shared<AdmissionTests::Link>()};
        NetworkChaosScope scope{TestSupport::Session(100), TestSupport::Session(101)};
        NetworkChaosScenario scenario;
        std::array<bool, 2> reconnect{};
        std::uint64_t rejectedSends{};

        explicit Wire(const NetworkChaosScenarioDescriptor &descriptor) : scope(descriptor.scope), scenario(Create(descriptor)) {}

        static NetworkChaosScenario Create(const NetworkChaosScenarioDescriptor &descriptor) {
            auto value = NetworkChaosScenario::Create(descriptor);
            if (value.HasError())
                UNSCOPED_INFO(value.ErrorValue().domain.Value()
                              << ":" << value.ErrorValue().code.Value() << ":" << value.ErrorValue().message);
            REQUIRE(value.HasValue());
            return std::move(value).Value();
        }

        void Connected(const std::size_t side, const ConnectionHandle connection) {
            auto &endpoint = evidence->ends[side];
            endpoint.connection = connection;
            endpoint.open = true;
            REQUIRE(evidence
                        ->Queue(side, {.kind = side == 0 ? NetworkTransportEventKind::Accepted : NetworkTransportEventKind::Connected,
                                       .listener = side == 0 ? ListenerHandle::Create(0, 1).Value() : ListenerHandle{},
                                       .connection = connection})
                        .HasValue());
        }

        void Start() {
            REQUIRE(scenario.Descriptor().topology.size() == 2);
            for (std::size_t side = 0; side < 2; ++side)
                Connected(side, scenario.Descriptor().topology[side]);
        }

        std::size_t Side(const ConnectionHandle connection) const {
            return connection.Slot() == evidence->ends[0].connection.Slot() ? 0 : 1;
        }

        void Advance(const std::uint64_t tick) {
            std::array<DeterministicTransportEvent, 128> events;
            const auto count = scenario.Advance(scope, tick, events);
            REQUIRE(count.HasValue());
            for (std::size_t index = 0; index < count.Value(); ++index)
                Normalize(events[index]);
            ObserveRestarts();
        }

        void Normalize(const DeterministicTransportEvent &event) {
            const auto side = Side(event.connection);
            REQUIRE(event.fragmentCount <= 1);  // This fixture pins whole-envelope fragmentation, not a second reassembly implementation.
            if (event.kind == DeterministicTransportEventKind::Disconnected) {
                evidence->ends[side].open = false;
            }
            REQUIRE(
                evidence
                    ->Queue(side, {.kind = event.kind == DeterministicTransportEventKind::Packet ? NetworkTransportEventKind::PacketReceived
                                                                                                 : NetworkTransportEventKind::Closed,
                                   .connection = event.connection,
                                   .channel = event.channel,
                                   .delivery = DeliveryPolicy::ReliableOrdered,
                                   .payload = {event.payload.begin(), event.payload.end()}})
                    .HasValue());
        }

        void ObserveRestarts() {
            for (std::size_t side = 0; side < 2; ++side)
                reconnect[side] = scenario.RestartedConnection(scope, evidence->ends[side].connection).HasValue();
        }

        // Connectivity is published on the following tick, after Closed reached both canonical controllers.
        void PublishRestarts() {
            for (std::size_t side = 0; side < 2; ++side) {
                if (!reconnect[side])
                    continue;
                reconnect[side] = false;
                const auto issued = scenario.RestartedConnection(scope, evidence->ends[side].connection);
                if (issued.HasError())
                    UNSCOPED_INFO(issued.ErrorValue().domain.Value()
                                  << ":" << issued.ErrorValue().code.Value() << ":" << issued.ErrorValue().message);
                REQUIRE(issued.HasValue());
                Connected(side, issued.Value());
            }
        }
    };

    class Transport final : public INetworkTransport {
    public:
        Transport(Wire &wire, const std::size_t side) : wire_(wire), side_(side) {}

        Result<void> Initialize(const NetworkTransportConfig &) override {
            return Result<void>::Success();
        }

        Result<ListenerHandle> Listen(const NetworkListenRequest &) override {
            return ListenerHandle::Create(0, 1);
        }

        Result<ConnectionHandle> Connect(const NetworkConnectRequest &) override {
            return Result<ConnectionHandle>::Success(wire_.evidence->ends[side_].connection);
        }

        Result<void> Send(const ConnectionHandle connection, const ChannelId channel, const std::span<const std::byte> payload,
                          const DeliveryPolicy delivery) override {
            if (connection != wire_.evidence->ends[side_].connection || delivery != DeliveryPolicy::ReliableOrdered)
                return Result<void>::Failure(MakeError(NetworkErrors::NetworkLifecycleOperationStale));
            return SendTo(wire_.evidence->ends[1 - side_].connection, channel, payload);
        }

        Result<void> SendTo(const ConnectionHandle receiver, const ChannelId channel, const std::span<const std::byte> payload) {
            const auto sent = wire_.scenario.Send(wire_.scope, receiver, channel, TransportTrafficClass::Reliable, 0, payload);
            if (sent.HasError()) {
                ++wire_.rejectedSends;
                return Result<void>::Failure(sent.ErrorValue());
            }
            if (sent.Value().outcome == DeterministicSendOutcome::ConnectionMustClose)
                return Result<void>::Failure(MakeError(NetworkErrors::TransportReliableBackpressure));
            return Result<void>::Success();
        }

        Result<void> Close(const ConnectionHandle connection) override {
            const auto closed = wire_.scenario.Close(wire_.scope, connection);
            return closed.HasError() ? Result<void>::Failure(closed.ErrorValue()) : Result<void>::Success();
        }

        Result<void> CloseListener(ListenerHandle) override {
            return Result<void>::Success();
        }

        Result<std::size_t> PollEvents(INetworkTransportEventConsumer &consumer) override {
            auto &endpoint = wire_.evidence->ends[side_];
            const auto count = endpoint.queued;
            for (std::size_t index = 0; index < count; ++index)
                consumer.Consume(std::move(endpoint.events[index]));
            endpoint.queued = 0;
            return Result<std::size_t>::Success(count);
        }

        NetworkTransportStats Stats() const noexcept override {
            return {};
        }

        Result<NetworkConnectionStats> ConnectionStats(ConnectionHandle) const override {
            return Result<NetworkConnectionStats>::Success({});
        }

        TransportCapabilities Capabilities() const noexcept override {
            TransportCapabilities value;
            value.revision = 1;
            value.delivery[static_cast<std::size_t>(DeliveryPolicy::ReliableOrdered)] = TransportSupport::Available;
            value.maximumChannels = 2;
            value.maximumMessageBytes = 1200;
            return value;
        }

        void Shutdown() noexcept override {
            wire_.evidence->ends[side_].open = false;
        }

    private:
        Wire &wire_;
        std::size_t side_;
    };
}  // namespace Horo::Network::ChaosTests
