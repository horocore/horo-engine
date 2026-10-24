#include "GnsTransportInternal.h"
#include "Horo/Network/NetworkErrors.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdint>
#include <span>
#include <thread>

namespace {
    using namespace Horo::Network;
    using namespace std::chrono_literals;

    struct Events final : INetworkTransportEventConsumer {
        void Consume(NetworkTransportEvent event) noexcept override {
            if (count == values.size()) {
                overflow = true;
                return;
            }
            values[count++] = std::move(event);
        }

        std::array<NetworkTransportEvent, 64> values;
        std::size_t count{};
        bool overflow{};

        std::size_t Count(NetworkTransportEventKind kind) const {
            std::size_t result{};
            for (std::size_t i = 0; i < count; ++i)
                result += values[i].kind == kind ? 1 : 0;
            return result;
        }
    };

    bool Wait(INetworkTransport &transport, Events &events, NetworkTransportEventKind kind, std::size_t count) {
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        while (std::chrono::steady_clock::now() < deadline) {
            if (transport.PollEvents(events).HasError() || events.overflow)
                return false;
            if (events.Count(kind) >= count)
                return true;
            std::this_thread::sleep_for(1ms);
        }
        return false;
    }

    ConnectionHandle Loopback(GnsTransport &transport, Events &events, NetworkTransportEventKind terminal, std::size_t count) {
        NetworkAddress address;
        bool listening{};
        for (unsigned port = 42100; port < 42200; ++port) {
            const auto candidate = NetworkAddress::Parse("127.0.0.1:" + std::to_string(port));
            REQUIRE(candidate.HasValue());
            if (transport.Listen({candidate.Value(), 4}).HasValue()) {
                address = candidate.Value();
                listening = true;
                break;
            }
        }
        REQUIRE(listening);
        const auto connection = transport.Connect({address, 5s});
        REQUIRE(connection.HasValue());
        REQUIRE(Wait(transport, events, terminal, count));
        return connection.Value();
    }

    struct Operations final {
        std::size_t configured{};
        std::size_t sends{};
        std::size_t freedPayloads{};
        void (*originalFree)(SteamNetworkingMessage_t *){};
        int64 originalUserData{};

        static EResult ConfigureFails(void *context, ISteamNetworkingSockets &, HSteamNetConnection, int) {
            ++static_cast<Operations *>(context)->configured;
            return k_EResultInvalidParam;
        }

        static EResult ConfigureReal(void *context, ISteamNetworkingSockets &sockets, HSteamNetConnection connection, int count) {
            ++static_cast<Operations *>(context)->configured;
            return sockets.ConfigureConnectionLanes(connection, count, nullptr, nullptr);
        }

        static void SendReal(void *, ISteamNetworkingSockets &sockets, SteamNetworkingMessage_t **message, int64 *result) {
            sockets.SendMessages(1, message, result, false);
        }

        static void FreePayload(SteamNetworkingMessage_t *message) {
            auto &context = *reinterpret_cast<Operations *>(static_cast<std::uintptr_t>(message->m_nUserData));
            ++context.freedPayloads;
            message->m_nUserData = context.originalUserData;
            if (context.originalFree)
                context.originalFree(message);
        }

        static void SendRejected(void *context, ISteamNetworkingSockets &, SteamNetworkingMessage_t **message, int64 *result) {
            auto &state = *static_cast<Operations *>(context);
            ++state.sends;
            state.originalFree = (*message)->m_pfnFreeData;
            state.originalUserData = (*message)->m_nUserData;
            (*message)->m_nUserData = static_cast<int64>(reinterpret_cast<std::uintptr_t>(context));
            (*message)->m_pfnFreeData = &FreePayload;  // Documented native ownership callback, not the reserved release function.
            *result = -static_cast<int64>(k_EResultLimitExceeded);
            // false-mode rejection MUST leave the real native allocation caller-owned.
        }
    };

    /** @brief Send an interleaved native series on each distinct configured lane. */
    void SendLaneSeries(INetworkTransport &transport, const ConnectionHandle outgoing) {
        for (std::uint8_t sequence = 0; sequence < 4; ++sequence) {
            for (std::uint8_t lane = 0; lane < 3; ++lane) {
                const std::array payload{std::byte{lane}, std::byte{sequence}};
                const auto channel = ChannelId::Create(lane, 3);
                REQUIRE(channel.HasValue());
                REQUIRE(transport.Send(outgoing, channel.Value(), payload, DeliveryPolicy::ReliableOrdered).HasValue());
            }
        }
    }

    /** @brief Assert every actual received lane preserves exact payload and its own reliable order. */
    void VerifyLaneDelivery(const Events &collector, const ConnectionHandle incoming) {
        std::array<std::uint8_t, 3> next{};
        for (const auto &event : std::span{collector.values}.first(collector.count)) {
            if (event.kind != NetworkTransportEventKind::PacketReceived)
                continue;
            REQUIRE(event.connection == incoming);
            REQUIRE(event.delivery == DeliveryPolicy::ReliableOrdered);
            REQUIRE(event.channel.Value() < next.size());
            REQUIRE(event.payload.size() == 2);
            REQUIRE(event.payload[0] == std::byte{static_cast<std::uint8_t>(event.channel.Value())});
            REQUIRE(event.payload[1] == std::byte{next[event.channel.Value()]});
            ++next[event.channel.Value()];
        }
        REQUIRE(next == std::array<std::uint8_t, 3>{4, 4, 4});
    }
}  // namespace

TEST_CASE("GNS lane configuration failure never publishes readiness and retires exact generations once", "[network][gns][lanes]") {
    Operations operations;  // Context and callback code outlive the transport and native drain.
    GnsTransport transport{Operations::ConfigureFails, Operations::SendReal, &operations};
    REQUIRE(transport.Initialize({.maximumConnections = 4, .maximumEventsPerPoll = 32, .maximumChannels = 2}).HasValue());
    Events events;
    const auto connection = Loopback(transport, events, NetworkTransportEventKind::Failed, 2);
    REQUIRE(operations.configured >= 1);
    REQUIRE(operations.configured <= 2);
    REQUIRE(events.Count(NetworkTransportEventKind::Connected) == 0);
    REQUIRE(events.Count(NetworkTransportEventKind::Accepted) == 0);
    REQUIRE(transport.Stats().activeConnections == 0);
    std::size_t configurationFailures{};
    std::array<ConnectionHandle, 2> terminalHandles;
    std::size_t terminals{};
    for (std::size_t i = 0; i < events.count; ++i) {
        if (events.values[i].kind != NetworkTransportEventKind::Failed)
            continue;
        REQUIRE(events.values[i].connection.IsValid());
        REQUIRE(terminals < terminalHandles.size());
        terminalHandles[terminals++] = events.values[i].connection;
        if (events.values[i].failure == &NetworkErrors::TransportCapabilityUnavailable)
            ++configurationFailures;
        else
            REQUIRE(events.values[i].failure == &NetworkErrors::TransportConnectionFailed);
    }
    REQUIRE(terminals == 2);
    REQUIRE(terminalHandles[0] != terminalHandles[1]);
    REQUIRE(configurationFailures == operations.configured);
    for (unsigned poll = 0; poll < 4; ++poll)
        REQUIRE(transport.PollEvents(events).HasValue());
    REQUIRE(events.Count(NetworkTransportEventKind::Failed) == 2);
    REQUIRE(transport.Send(connection, {}, {}, DeliveryPolicy::ReliableOrdered).HasError());
    transport.Shutdown();
    transport.Shutdown();
    GnsTransport replacement;
    REQUIRE(replacement.Initialize({}).HasValue());
    replacement.Shutdown();
}

TEST_CASE("GNS rejected native lane send releases the real caller-owned payload once without accepted stats", "[network][gns][lanes]") {
    Operations operations;
    GnsTransport transport{Operations::ConfigureReal, Operations::SendRejected, &operations};
    REQUIRE(transport.Initialize({.maximumConnections = 4, .maximumEventsPerPoll = 32, .maximumChannels = 2}).HasValue());
    Events events;
    const auto connection = Loopback(transport, events, NetworkTransportEventKind::Connected, 1);
    REQUIRE(Wait(transport, events, NetworkTransportEventKind::Accepted, 1));
    const std::array payload{std::byte{1}, std::byte{2}};
    const auto rejected = transport.Send(connection, ChannelId::Create(1, 2).Value(), payload, DeliveryPolicy::ReliableOrdered);
    REQUIRE(rejected.HasError());
    REQUIRE(rejected.ErrorValue().domain.Value() == NetworkErrors::TransportReliableBackpressure.domain.Value());
    REQUIRE(rejected.ErrorValue().code.Value() == NetworkErrors::TransportReliableBackpressure.code.Value());
    REQUIRE(operations.sends == 1);
    REQUIRE(operations.freedPayloads == 1);  // Native Release actually invoked the documented data callback.
    REQUIRE(transport.Stats().sentMessages == 0);
    REQUIRE(transport.Stats().sentBytes == 0);
    transport.Shutdown();
    REQUIRE(operations.freedPayloads == 1);
}

TEST_CASE("GNS native lanes preserve exact routing and independent reliable order", "[network][gns][lanes]") {
    GnsTransport transport;
    REQUIRE(transport.Initialize({.maximumChannels = 0}).HasError());
    REQUIRE(transport.Initialize({.maximumChannels = 4}).HasError());
    REQUIRE(transport.Initialize({.maximumConnections = 8, .maximumEventsPerPoll = 32, .maximumChannels = 3}).HasValue());
    REQUIRE(transport.Capabilities().maximumChannels == 3);
    Events collector;
    const auto outgoing = Loopback(transport, collector, NetworkTransportEventKind::Connected, 1);
    REQUIRE(Wait(transport, collector, NetworkTransportEventKind::Accepted, 1));
    ConnectionHandle incoming;
    for (const auto &event : std::span{collector.values}.first(collector.count))
        if (event.kind == NetworkTransportEventKind::Accepted)
            incoming = event.connection;
    REQUIRE(incoming.IsValid());
    SendLaneSeries(transport, outgoing);
    const auto outside = ChannelId::Create(3, 4);
    REQUIRE(outside.HasValue());
    REQUIRE(transport.Send(outgoing, outside.Value(), {}, DeliveryPolicy::ReliableOrdered).HasError());
    REQUIRE(Wait(transport, collector, NetworkTransportEventKind::PacketReceived, 12));
    VerifyLaneDelivery(collector, incoming);
    REQUIRE(transport.Stats().sentMessages == 12);
    REQUIRE(transport.Stats().receivedMessages == 12);
    REQUIRE(transport.Close(outgoing).HasValue());
    REQUIRE(transport.Close(incoming).HasValue());
    transport.Shutdown();
    REQUIRE(transport.Stats().activeConnections == 0);
}
