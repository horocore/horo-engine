#pragma once

#include "GnsAddressResolution.h"
#include "GnsTransportFactory.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <steam/isteamnetworkingutils.h>
#include <steam/steamnetworkingsockets.h>
#include <thread>
#include <vector>

namespace Horo::Network {
    enum class ConnectionPhase : std::uint8_t {
        Pending,
        Connected,
        Terminal
    };

    struct ConnectionSlot final {
        ConnectionHandle handle{};
        HSteamNetConnection native{k_HSteamNetConnection_Invalid};
        ConnectionPhase phase{ConnectionPhase::Terminal};
        CancellationToken cancellation{};
        std::chrono::steady_clock::time_point deadline{};
        std::shared_ptr<GnsDetail::Resolution> resolution;
        bool inbound{};
    };

    class GnsTransport final : public INetworkTransport {
    public:
        ~GnsTransport() override {
            Shutdown();
        }

        [[nodiscard]] Result<void> Initialize(const NetworkTransportConfig &config) override;
        [[nodiscard]] Result<ListenerHandle> Listen(const NetworkListenRequest &request) override;
        [[nodiscard]] Result<ConnectionHandle> Connect(const NetworkConnectRequest &request) override;
        [[nodiscard]] Result<void> Send(ConnectionHandle connection, ChannelId channel, std::span<const std::byte> payload,
                                        DeliveryPolicy delivery) override;
        [[nodiscard]] Result<void> Close(ConnectionHandle connection) override;
        [[nodiscard]] Result<void> CloseListener(ListenerHandle listener) override;
        [[nodiscard]] Result<std::size_t> PollEvents(INetworkTransportEventConsumer &consumer) override;
        [[nodiscard]] NetworkTransportStats Stats() const noexcept override;
        [[nodiscard]] Result<NetworkConnectionStats> ConnectionStats(ConnectionHandle connection) const override;
        [[nodiscard]] TransportCapabilities Capabilities() const noexcept override;
        void Shutdown() noexcept override;

        static void OnStatus(SteamNetConnectionStatusChangedCallback_t *status);

    private:
        [[nodiscard]] Result<ConnectionHandle> ReserveConnection();
        [[nodiscard]] Result<void> StartNative(ConnectionSlot &slot, const SteamNetworkingIPAddr &address);
        [[nodiscard]] ConnectionSlot *Find(ConnectionHandle handle) noexcept;
        [[nodiscard]] ConnectionSlot *Find(HSteamNetConnection native) noexcept;
        void OnStatusOwned(const SteamNetConnectionStatusChangedCallback_t &status);
        void End(ConnectionSlot &slot, NetworkTransportEventKind kind, const ErrorCodeDescriptor *failure = nullptr);
        void Enqueue(NetworkTransportEvent event);
        void PollMessages();
        [[nodiscard]] Result<void> StartResolution(ConnectionSlot &slot, const NetworkAddress &endpoint);
        void AdvancePending(std::chrono::steady_clock::time_point now);

        mutable std::recursive_mutex mutex_;
        ISteamNetworkingSockets *native_{};
        NetworkTransportConfig config_{};
        ListenerHandle listener_{};
        HSteamListenSocket nativeListener_{k_HSteamListenSocket_Invalid};
        std::vector<ConnectionSlot> connections_;
        std::deque<NetworkTransportEvent> events_;
        NetworkTransportStats stats_{};
        std::uint64_t capabilityRevision_{1};
        std::shared_ptr<std::atomic<unsigned>> resolverTasks_;
        std::thread::id ownerThread_{};
        std::uint32_t listenerMaximumConnections_{};
        bool overflow_{};
        bool initialized_{};
        bool shutdown_{};
    };
}  // namespace Horo::Network
