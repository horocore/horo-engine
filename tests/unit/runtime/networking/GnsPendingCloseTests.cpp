#include "GnsTransportFactory.h"
#include "Horo/Network/NetworkErrors.h"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <span>
#include <steam/steamnetworkingsockets.h>
#include <string>
#include <thread>

namespace {
    using namespace Horo::Network;
    using namespace std::chrono_literals;

    /** @brief Fixed event recording; no allocation/assertion escapes the noexcept native-owner callback. */
    class TerminalCollector final : public INetworkTransportEventConsumer {
    public:
        void Consume(NetworkTransportEvent event) noexcept override {
            if (size == events.size()) {
                overflow = true;
                return;
            }
            events[size++] = std::move(event);
        }

        std::size_t Count(ConnectionHandle connection, NetworkTransportEventKind kind) const {
            return std::count_if(events.begin(), events.begin() + size, [=](const auto &event) {
                return event.connection == connection && event.kind == kind;
            });
        }

        std::array<NetworkTransportEvent, 32> events{};
        std::size_t size{};
        bool overflow{};
    };

    /** @brief Test listener on the SAME native process owner; reject pending incoming connections without accepting them. */
    class RejectingNativeListener final {
    public:
        RejectingNativeListener() {
            REQUIRE(active_ == nullptr);
            active_ = this;
        }

        ~RejectingNativeListener() {
            Close();
            active_ = nullptr;
        }

        void Listen() {
            SteamNetworkingConfigValue_t callback;
            callback.SetPtr(k_ESteamNetworkingConfig_Callback_ConnectionStatusChanged, reinterpret_cast<void *>(&OnStatus));
            for (unsigned port = 42500; port < 42600 && listener_ == k_HSteamListenSocket_Invalid; ++port) {
                SteamNetworkingIPAddr address;
                REQUIRE(address.ParseString(("127.0.0.1:" + std::to_string(port)).c_str()));
                listener_ = SteamNetworkingSockets()->CreateListenSocketIP(address, 1, &callback);
                if (listener_ != k_HSteamListenSocket_Invalid) {
                    auto parsed = NetworkAddress::Parse("127.0.0.1:" + std::to_string(port));
                    REQUIRE(parsed.HasValue());
                    endpoint = parsed.Value();
                }
            }
            REQUIRE(listener_ != k_HSteamListenSocket_Invalid);
        }

        void Close() noexcept {
            if (listener_ != k_HSteamListenSocket_Invalid) {
                (void)SteamNetworkingSockets()->CloseListenSocket(listener_);
                listener_ = k_HSteamListenSocket_Invalid;
            }
        }

        NetworkAddress endpoint;
        std::size_t rejected{};
        bool failed{};

    private:
        static void OnStatus(SteamNetConnectionStatusChangedCallback_t *event) noexcept {
            if (!active_ || !event || event->m_info.m_eState != k_ESteamNetworkingConnectionState_Connecting ||
                event->m_info.m_hListenSocket != active_->listener_)
                return;
            try {
                if (SteamNetworkingSockets()->CloseConnection(event->m_hConn, k_ESteamNetConnectionEnd_App_Generic, nullptr, false))
                    ++active_->rejected;
                else
                    active_->failed = true;
            } catch (...) {
                active_->failed = true;
            }
        }

        HSteamListenSocket listener_{k_HSteamListenSocket_Invalid};
        inline static RejectingNativeListener *active_{};  // Sole test-owned owner-thread callback, revoked before native shutdown.
    };

    void AwaitFailure(INetworkTransport &transport, TerminalCollector &events, ConnectionHandle connection) {
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        while (events.Count(connection, NetworkTransportEventKind::Failed) == 0 && std::chrono::steady_clock::now() < deadline) {
            REQUIRE(transport.PollEvents(events).HasValue());
            std::this_thread::sleep_for(2ms);
        }
        REQUIRE_FALSE(events.overflow);
        REQUIRE(events.Count(connection, NetworkTransportEventKind::Failed) == 1);
        REQUIRE(events.Count(connection, NetworkTransportEventKind::Closed) == 0);
        REQUIRE(events.Count(connection, NetworkTransportEventKind::Connected) == 0);
        REQUIRE(events.Count(connection, NetworkTransportEventKind::Accepted) == 0);
        for (const auto &event : std::span{events.events}.first(events.size)) {
            if (event.connection != connection || event.kind != NetworkTransportEventKind::Failed)
                continue;
            REQUIRE(event.failure);
            REQUIRE(event.failure->domain.Value() == NetworkErrors::TransportConnectionFailed.domain.Value());
            REQUIRE(event.failure->code.Value() == NetworkErrors::TransportConnectionFailed.code.Value());
        }
    }
}  // namespace

TEST_CASE("GNS peer closure before readiness fails once and stale callbacks cannot terminate a replacement handle",
          "[network][gns][lifecycle]") {
    auto created = CreateGnsTransport();
    REQUIRE(created.HasValue());
    auto transport = std::move(created).Value();
    REQUIRE(transport->Initialize({1, 16, 1200}).HasValue());
    RejectingNativeListener listener;
    listener.Listen();
    auto first = transport->Connect({listener.endpoint, 5s});
    REQUIRE(first.HasValue());
    TerminalCollector events;
    AwaitFailure(*transport, events, first.Value());
    auto replacement = transport->Connect({listener.endpoint, 5s});
    REQUIRE(replacement.HasValue());
    REQUIRE(replacement.Value().Slot() == first.Value().Slot());
    REQUIRE(replacement.Value().Generation() > first.Value().Generation());
    REQUIRE(transport->Close(first.Value()).HasError());
    REQUIRE(transport->Send(first.Value(), {}, {}, DeliveryPolicy::ReliableOrdered).HasError());
    AwaitFailure(*transport, events, replacement.Value());
    for (int poll = 0; poll < 10; ++poll) {
        REQUIRE(transport->PollEvents(events).HasValue());
        std::this_thread::sleep_for(2ms);
    }
    REQUIRE_FALSE(listener.failed);
    REQUIRE(listener.rejected == 2);
    REQUIRE_FALSE(events.overflow);
    REQUIRE(events.Count(first.Value(), NetworkTransportEventKind::Failed) == 1);
    REQUIRE(events.Count(replacement.Value(), NetworkTransportEventKind::Failed) == 1);
    REQUIRE(transport->Stats().activeConnections == 0);
    listener.Close();
    transport->Shutdown();
}
