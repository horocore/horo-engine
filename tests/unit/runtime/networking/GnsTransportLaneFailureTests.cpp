#include "GnsTransportInternal.h"
#include "Horo/Network/NetworkErrors.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdint>
#include <memory>
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

    struct Payload;

    /** @brief Callback observations borrowed for the lifetime of the native message owner. */
    struct Verification {
        const Payload *allocation{};
        const GnsLaneOperations *lanes{};
        std::size_t frees{};
        bool restored{true};
        std::thread::id freedOn{};
    };

    /** @brief Native allocation contents used to verify callback restoration before release. */
    struct Payload {
        Verification *verification;
        void (*nativeFree)(SteamNetworkingMessage_t *);
        std::uint64_t marker;
    };

    /** @brief Verify restored payload metadata and permit a nested rejected release before native cleanup. */
    void VerifyAndFreePayload(SteamNetworkingMessage_t *value) {
        auto *payload = static_cast<Payload *>(value->m_pData);
        auto &observed = *payload->verification;
        observed.restored = observed.restored && value->m_pData == observed.allocation &&
                            value->m_cbSize == static_cast<int>(sizeof(Payload)) && value->m_nUserData == 123 &&
                            payload->marker == 0x12345678;
        observed.freedOn = std::this_thread::get_id();
        ++observed.frees;
        const auto nativeFree = payload->nativeFree;
        // A nested empty rejection has no data free of its own. It must retire
        // its borrow without corrupting this outer callback's restored payload.
        auto *nested = SteamNetworkingUtils()->AllocateMessage(0);
        if (nested) {
            int64 nestedResult{};
            observed.lanes->SendMessage(*SteamNetworkingSockets(), &nested, &nestedResult);
            observed.restored = observed.restored && nestedResult == -static_cast<int64>(k_EResultLimitExceeded);
            nested->Release();
        } else {
            observed.restored = false;
        }
        observed.restored = observed.restored && value->m_pData == payload && payload->marker == 0x12345678;
        std::destroy_at(payload);
        nativeFree(value);  // Free the original AllocateMessage allocation through its exact callback.
    }
}  // namespace

TEST_CASE("GNS lane configuration failure never publishes readiness and retires exact generations once", "[network][gns][lanes]") {
    GnsLaneFailureInjection operations{GnsLaneFailure::ConfigureRejected};
    GnsTransport transport{GnsTransportSetup{.laneFailure = &operations}};
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
    // This nonmoving borrow precedes the transport. Rejected messages never enter
    // an SDK queue: the pinned ReleaseFunc calls m_pfnFreeData synchronously.
    GnsLaneFailureInjection operations{GnsLaneFailure::SendRejected};
    GnsTransport transport{GnsTransportSetup{.laneFailure = &operations}};
    REQUIRE(transport.Initialize({.maximumConnections = 4, .maximumEventsPerPoll = 32, .maximumChannels = 2}).HasValue());
    Events events;
    const auto connection = Loopback(transport, events, NetworkTransportEventKind::Connected, 1);
    REQUIRE(Wait(transport, events, NetworkTransportEventKind::Accepted, 1));
    const std::array payload{std::byte{1}, std::byte{2}};
    for (std::size_t rejectedCount = 1; rejectedCount <= 3; ++rejectedCount) {
        const auto rejected = transport.Send(connection, ChannelId::Create(1, 2).Value(), payload, DeliveryPolicy::ReliableOrdered);
        REQUIRE(rejected.HasError());
        REQUIRE(rejected.ErrorValue().domain.Value() == NetworkErrors::TransportReliableBackpressure.domain.Value());
        REQUIRE(rejected.ErrorValue().code.Value() == NetworkErrors::TransportReliableBackpressure.code.Value());
        REQUIRE(operations.sends == rejectedCount);
        REQUIRE(operations.freedPayloads == rejectedCount);  // Actual native Release callback, not an inferred RAII count.
        REQUIRE(operations.freedOn == std::this_thread::get_id());
        REQUIRE(operations.originalPayload == nullptr);
        REQUIRE(operations.originalFree == nullptr);
        REQUIRE(transport.Stats().sentMessages == 0);
        REQUIRE(transport.Stats().sentBytes == 0);
    }
    transport.Shutdown();
    REQUIRE(operations.freedPayloads == 3);
}

TEST_CASE("GNS rejected callback restores the exact native payload before freeing it", "[network][gns][lanes]") {
    Verification verification;
    GnsLaneFailureInjection failure{GnsLaneFailure::SendRejected};
    GnsTransport transport;
    REQUIRE(transport.Initialize({}).HasValue());
    const GnsLaneOperations lanes{&failure};
    verification.lanes = &lanes;
    for (std::size_t count = 1; count <= 3; ++count) {
        auto *message = SteamNetworkingUtils()->AllocateMessage(static_cast<int>(sizeof(Payload)));
        REQUIRE(message != nullptr);
        const std::unique_ptr<SteamNetworkingMessage_t, void (*)(SteamNetworkingMessage_t *)> owner{message,
                                                                                                    [](SteamNetworkingMessage_t *value) {
            value->Release();
        }};
        REQUIRE(message->m_pData != nullptr);
        REQUIRE(message->m_pfnFreeData != nullptr);
        verification.allocation = static_cast<Payload *>(message->m_pData);
        std::construct_at(static_cast<Payload *>(message->m_pData), Payload{&verification, message->m_pfnFreeData, 0x12345678});
        message->m_nUserData = 123;
        message->m_pfnFreeData = &VerifyAndFreePayload;
        int64 result{};
        auto *originalMessage = message;
        lanes.SendMessage(*SteamNetworkingSockets(), &message, &result);
        REQUIRE(result == -static_cast<int64>(k_EResultLimitExceeded));
        REQUIRE(message == originalMessage);
        REQUIRE(message->m_cbSize == static_cast<int>(sizeof(Payload)));
        REQUIRE(message->m_nUserData == 123);
        // owner destruction invokes the real pinned ReleaseFunc and our data callback.
    }
    REQUIRE(verification.restored);
    REQUIRE(verification.frees == 3);
    REQUIRE(verification.freedOn == std::this_thread::get_id());
    REQUIRE(failure.sends == 6);
    REQUIRE(failure.freedPayloads == 6);  // Three payload releases plus three nested empty-message releases.
    REQUIRE(failure.originalPayload == nullptr);
    REQUIRE(failure.originalFree == nullptr);
    transport.Shutdown();
    REQUIRE(verification.frees == 3);
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
