#include "GnsTransportFactory.h"
#include "Horo/Foundation/CancellationToken.h"
#include "Horo/Network/NetworkErrors.h"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {
    using namespace Horo::Network;
    using namespace std::chrono_literals;

    class Collector final : public INetworkTransportEventConsumer {
    public:
        void Consume(NetworkTransportEvent event) noexcept override {
            events.push_back(std::move(event));
        }

        std::vector<NetworkTransportEvent> events;
    };

    [[nodiscard]] std::size_t Count(const Collector &collector, const NetworkTransportEventKind kind) {
        std::size_t count{};
        for (const auto &event : collector.events) {
            if (event.kind == kind)
                ++count;
        }
        return count;
    }

    [[nodiscard]] bool WaitFor(INetworkTransport &transport, Collector &collector, const NetworkTransportEventKind kind,
                               const std::size_t required) {
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        while (std::chrono::steady_clock::now() < deadline) {
            const auto polled = transport.PollEvents(collector);
            if (polled.HasError())
                return false;
            if (Count(collector, kind) >= required)
                return true;
            std::this_thread::sleep_for(10ms);
        }
        return false;
    }

    [[nodiscard]] Horo::Result<ListenerHandle> ListenLoopback(INetworkTransport &transport, NetworkAddress &address) {
        // Candidate range is finite; listen binds the actual UDP socket. No
        // separate preflight socket is held open, so parallel CI is supported.
        for (int port = 42000; port < 42100; ++port) {
            const auto parsed = NetworkAddress::Parse("127.0.0.1:" + std::to_string(port));
            if (parsed.HasError())
                break;
            auto attempted = transport.Listen({.bindAddress = parsed.Value(), .maximumConnections = 4});
            if (attempted.HasValue()) {
                address = parsed.Value();
                return attempted;
            }
        }
        return Horo::Result<ListenerHandle>::Failure(Horo::MakeError(NetworkErrors::TransportNativeUnavailable));
    }

    void VerifyLoopbackDelivery(INetworkTransport &transport, Collector &collector, const ConnectionHandle outgoing,
                                const ConnectionHandle incoming) {
        const auto channel = ChannelId::Create(0, 1);
        REQUIRE(channel.HasValue());
        const std::array<std::byte, 3> reliable{std::byte{1}, std::byte{2}, std::byte{3}};
        const std::array<std::byte, 2> unreliable{std::byte{4}, std::byte{5}};
        REQUIRE(transport.Send(outgoing, channel.Value(), reliable, DeliveryPolicy::ReliableOrdered).HasValue());
        REQUIRE(transport.Send(incoming, channel.Value(), unreliable, DeliveryPolicy::UnreliableUnordered).HasValue());
        REQUIRE(transport.Send(outgoing, channel.Value(), {}, DeliveryPolicy::ReliableOrdered).HasValue());
        REQUIRE(WaitFor(transport, collector, NetworkTransportEventKind::PacketReceived, 3));
        std::size_t reliableReceived{};
        std::size_t unreliableReceived{};
        std::size_t emptyReceived{};
        for (const auto &event : collector.events) {
            if (event.kind != NetworkTransportEventKind::PacketReceived)
                continue;
            if (event.connection == incoming && event.payload.size() == reliable.size() &&
                std::equal(event.payload.begin(), event.payload.end(), reliable.begin())) {
                REQUIRE(event.delivery == DeliveryPolicy::ReliableOrdered);
                ++reliableReceived;
            } else if (event.connection == outgoing && event.payload.size() == unreliable.size() &&
                       std::equal(event.payload.begin(), event.payload.end(), unreliable.begin())) {
                REQUIRE(event.delivery == DeliveryPolicy::UnreliableUnordered);
                ++unreliableReceived;
            } else if (event.connection == incoming && event.payload.empty()) {
                REQUIRE(event.delivery == DeliveryPolicy::ReliableOrdered);
                ++emptyReceived;
            } else {
                FAIL("GNS delivered a packet outside the exact peer, payload and mode contract");
            }
        }
        REQUIRE(reliableReceived == 1);
        REQUIRE(unreliableReceived == 1);
        REQUIRE(emptyReceived == 1);
    }
}  // namespace

TEST_CASE("GNS explicit factory initializes exact bounded capabilities and rejects malformed input", "[network][gns]") {
    auto created = CreateGnsTransport();
    REQUIRE(created.HasValue());
    auto transport = std::move(created).Value();
    const auto malformedConfig = transport->Initialize({.maximumConnections = 0});
    REQUIRE(malformedConfig.HasError());
    REQUIRE(malformedConfig.ErrorValue().code.Value() == NetworkErrors::TransportLimitExceeded.code.Value());
    REQUIRE(transport->Initialize({.maximumConnections = 8, .maximumEventsPerPoll = 8, .maximumMessageBytes = 1200}).HasValue());
    const auto capabilities = transport->Capabilities();
    REQUIRE(capabilities.delivery[static_cast<std::size_t>(DeliveryPolicy::ReliableOrdered)] == TransportSupport::Available);
    REQUIRE(capabilities.delivery[static_cast<std::size_t>(DeliveryPolicy::UnreliableUnordered)] == TransportSupport::Available);
    REQUIRE(capabilities.delivery[static_cast<std::size_t>(DeliveryPolicy::ReliableUnordered)] == TransportSupport::Unsupported);
    REQUIRE(capabilities.delivery[static_cast<std::size_t>(DeliveryPolicy::UnreliableSequenced)] == TransportSupport::Unsupported);
    REQUIRE(capabilities.maximumChannels == 1);
    auto secondCreated = CreateGnsTransport();
    REQUIRE(secondCreated.HasValue());
    auto second = std::move(secondCreated).Value();
    const auto competingHost = second->Initialize({});
    REQUIRE(competingHost.HasError());
    REQUIRE(competingHost.ErrorValue().code.Value() == NetworkErrors::TransportCapabilityUnavailable.code.Value());
    second->Shutdown();
    const auto malformedListen = transport->Listen({.maximumConnections = 1});
    REQUIRE(malformedListen.HasError());
    REQUIRE(malformedListen.ErrorValue().code.Value() == NetworkErrors::NetworkAddressInvalid.code.Value());
    const auto malformedConnect = transport->Connect({});
    REQUIRE(malformedConnect.HasError());
    REQUIRE(malformedConnect.ErrorValue().code.Value() == NetworkErrors::NetworkAddressInvalid.code.Value());
    std::string wrongThreadError;
    std::thread wrongThread([&] {
        Collector otherCollector;
        const auto polled = transport->PollEvents(otherCollector);
        if (polled.HasError())
            wrongThreadError = polled.ErrorValue().code.Value();
    });
    wrongThread.join();
    REQUIRE(wrongThreadError == NetworkErrors::NetworkIoWrongThread.code.Value());
    transport->Shutdown();
    transport->Shutdown();
    Collector collector;
    REQUIRE_FALSE(transport->PollEvents(collector).HasValue());
}

TEST_CASE("GNS loopback connects, exchanges exact delivery modes, and closes without duplicate terminal events", "[network][gns]") {
    auto created = CreateGnsTransport();
    REQUIRE(created.HasValue());
    auto transport = std::move(created).Value();
    REQUIRE(transport->Initialize({.maximumConnections = 8, .maximumEventsPerPoll = 8, .maximumMessageBytes = 1200}).HasValue());
    NetworkAddress address;
    auto listener = ListenLoopback(*transport, address);
    REQUIRE(listener.HasValue());
    auto outgoing = transport->Connect({.endpoint = address, .timeout = 5s});
    REQUIRE(outgoing.HasValue());
    Collector collector;
    REQUIRE(WaitFor(*transport, collector, NetworkTransportEventKind::Connected, 2));
    REQUIRE(collector.events.front().kind == NetworkTransportEventKind::ListenerReady);
    REQUIRE(Count(collector, NetworkTransportEventKind::Accepted) == 1);
    ConnectionHandle incoming{};
    for (const auto &event : collector.events) {
        if (event.kind == NetworkTransportEventKind::Accepted)
            incoming = event.connection;
    }
    REQUIRE(incoming.IsValid());
    VerifyLoopbackDelivery(*transport, collector, outgoing.Value(), incoming);
    const auto channel = ChannelId::Create(0, 1);
    REQUIRE(channel.HasValue());
    const std::array<std::byte, 3> reliable{std::byte{1}, std::byte{2}, std::byte{3}};
    const auto unsupported = transport->Send(outgoing.Value(), channel.Value(), reliable, DeliveryPolicy::ReliableUnordered);
    REQUIRE(unsupported.HasError());
    REQUIRE(unsupported.ErrorValue().code.Value() == NetworkErrors::TransportDeliveryUnsupported.code.Value());
    const std::vector<std::byte> oversized(1201);
    const auto tooLarge = transport->Send(outgoing.Value(), channel.Value(), oversized, DeliveryPolicy::ReliableOrdered);
    REQUIRE(tooLarge.HasError());
    REQUIRE(tooLarge.ErrorValue().code.Value() == NetworkErrors::TransportLimitExceeded.code.Value());
    const auto stats = transport->Stats();
    REQUIRE(transport->ConnectionStats(outgoing.Value()).HasValue());
    REQUIRE(stats.sentMessages == 3);
    REQUIRE(stats.receivedMessages == 3);
    REQUIRE(stats.sentBytes == 5);
    REQUIRE(stats.receivedBytes == 5);
    REQUIRE(transport->Close(outgoing.Value()).HasValue());
    REQUIRE(transport->Close(outgoing.Value()).HasValue());
    REQUIRE(transport->Close(incoming).HasValue());
    REQUIRE(transport->CloseListener(listener.Value()).HasValue());
    REQUIRE(transport->CloseListener(listener.Value()).HasValue());
    REQUIRE(WaitFor(*transport, collector, NetworkTransportEventKind::Closed, 2));
    REQUIRE(Count(collector, NetworkTransportEventKind::Closed) == 2);
    REQUIRE(stats.activeConnections == 2);
    REQUIRE(transport->Stats().activeConnections == 0);
    REQUIRE_FALSE(transport->ConnectionStats(outgoing.Value()).HasValue());
    transport->Shutdown();
}

TEST_CASE("GNS cancellation and stale generations fail closed", "[network][gns]") {
    auto created = CreateGnsTransport();
    REQUIRE(created.HasValue());
    auto transport = std::move(created).Value();
    REQUIRE(transport->Initialize({.maximumConnections = 2, .maximumEventsPerPoll = 8, .maximumMessageBytes = 1200}).HasValue());
    const auto endpoint = NetworkAddress::Parse("127.0.0.1:42499");
    REQUIRE(endpoint.HasValue());
    Horo::CancellationSource cancellation;
    cancellation.RequestCancellation();
    const auto cancelledBeforeAdmission = transport->Connect({.endpoint = endpoint.Value(), .cancellation = cancellation.Token()});
    REQUIRE(cancelledBeforeAdmission.HasError());
    REQUIRE(cancelledBeforeAdmission.ErrorValue().code.Value() == NetworkErrors::TransportOperationCancelled.code.Value());
    Horo::CancellationSource pending;
    auto connection = transport->Connect({.endpoint = endpoint.Value(), .cancellation = pending.Token()});
    REQUIRE(connection.HasValue());
    pending.RequestCancellation();
    Collector collector;
    REQUIRE(WaitFor(*transport, collector, NetworkTransportEventKind::Failed, 1));
    REQUIRE(Count(collector, NetworkTransportEventKind::Failed) == 1);
    REQUIRE(collector.events.back().failure != nullptr);
    REQUIRE(collector.events.back().failure->code.Value() == NetworkErrors::TransportOperationCancelled.code.Value());
    REQUIRE(transport->Close(connection.Value()).HasValue());
    auto replacement = transport->Connect({.endpoint = endpoint.Value(), .timeout = 5s});
    REQUIRE(replacement.HasValue());
    REQUIRE(replacement.Value() != connection.Value());
    REQUIRE_FALSE(transport->Send(connection.Value(), ChannelId{}, {}, DeliveryPolicy::ReliableOrdered).HasValue());
    const auto stale = transport->Close(connection.Value());
    REQUIRE(stale.HasError());
    REQUIRE(stale.ErrorValue().code.Value() == NetworkErrors::TransportHandleInvalid.code.Value());
    collector.events.clear();
    for (int i = 0; i < 10; ++i) {
        REQUIRE(transport->PollEvents(collector).HasValue());
        std::this_thread::sleep_for(5ms);
    }
    for (const auto &event : collector.events)
        REQUIRE(event.connection != connection.Value());
    transport->Shutdown();
}

TEST_CASE("GNS resolves a DNS endpoint asynchronously without exposing resolver state", "[network][gns]") {
    auto created = CreateGnsTransport();
    REQUIRE(created.HasValue());
    auto transport = std::move(created).Value();
    REQUIRE(transport->Initialize({.maximumConnections = 4, .maximumEventsPerPoll = 8, .maximumMessageBytes = 1200}).HasValue());
    NetworkAddress bind;
    auto listener = ListenLoopback(*transport, bind);
    REQUIRE(listener.HasValue());
    const auto hostname = NetworkAddress::Parse("localhost:" + std::to_string(bind.Port()));
    REQUIRE(hostname.HasValue());
    REQUIRE(hostname.Value().RequiresResolution());
    auto connecting = transport->Connect({.endpoint = hostname.Value(), .timeout = 5s});
    REQUIRE(connecting.HasValue());
    Collector collector;
    REQUIRE(WaitFor(*transport, collector, NetworkTransportEventKind::Connected, 2));
    REQUIRE(Count(collector, NetworkTransportEventKind::Accepted) == 1);
    transport->Shutdown();
    REQUIRE_FALSE(transport->PollEvents(collector).HasValue());
}

TEST_CASE("GNS DNS cancellation and shutdown discard late resolver results", "[network][gns]") {
    auto created = CreateGnsTransport();
    REQUIRE(created.HasValue());
    auto transport = std::move(created).Value();
    REQUIRE(transport->Initialize({.maximumConnections = 2, .maximumEventsPerPoll = 8, .maximumMessageBytes = 1200}).HasValue());
    const auto hostname = NetworkAddress::Parse("localhost:42499");
    REQUIRE(hostname.HasValue());
    Horo::CancellationSource cancellation;
    auto pending = transport->Connect({.endpoint = hostname.Value(), .timeout = 5s, .cancellation = cancellation.Token()});
    REQUIRE(pending.HasValue());
    cancellation.RequestCancellation();
    Collector collector;
    REQUIRE(WaitFor(*transport, collector, NetworkTransportEventKind::Failed, 1));
    REQUIRE(collector.events.back().connection == pending.Value());
    REQUIRE(collector.events.back().failure->code.Value() == NetworkErrors::TransportOperationCancelled.code.Value());
    const auto delivered = collector.events.size();
    transport->Shutdown();
    REQUIRE_FALSE(transport->PollEvents(collector).HasValue());
    REQUIRE(collector.events.size() == delivered);
}

TEST_CASE("GNS IPv6 loopback listens, connects, and closes when host IPv6 is available", "[network][gns]") {
    auto created = CreateGnsTransport();
    REQUIRE(created.HasValue());
    auto transport = std::move(created).Value();
    REQUIRE(transport->Initialize({.maximumConnections = 4, .maximumEventsPerPoll = 8, .maximumMessageBytes = 1200}).HasValue());
    NetworkAddress bind;
    Horo::Result<ListenerHandle> listener =
        Horo::Result<ListenerHandle>::Failure(Horo::MakeError(NetworkErrors::TransportNativeUnavailable));
    for (int port = 42200; port < 42300; ++port) {
        const auto parsed = NetworkAddress::Parse("[::1]:" + std::to_string(port));
        REQUIRE(parsed.HasValue());
        listener = transport->Listen({.bindAddress = parsed.Value(), .maximumConnections = 2});
        if (listener.HasValue()) {
            bind = parsed.Value();
            break;
        }
    }
    if (listener.HasError())
        SKIP("Host IPv6 loopback bind is unavailable");
    auto outgoing = transport->Connect({.endpoint = bind, .timeout = 5s});
    REQUIRE(outgoing.HasValue());
    Collector collector;
    REQUIRE(WaitFor(*transport, collector, NetworkTransportEventKind::Connected, 2));
    REQUIRE(transport->Close(outgoing.Value()).HasValue());
    REQUIRE(transport->CloseListener(listener.Value()).HasValue());
    transport->Shutdown();
}
