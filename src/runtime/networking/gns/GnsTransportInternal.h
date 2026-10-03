#pragma once

#include "GnsAddressResolution.h"
#include "GnsTransportFactory.h"

#include <chrono>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <steam/isteamnetworkingutils.h>
#include <steam/steamnetworkingsockets.h>
#include <string>
#include <thread>
#include <utility>
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
        std::optional<NetworkAddress> pendingEndpoint;
        std::unique_ptr<GnsDetail::Resolution> resolution;
        bool inbound{};
    };

    [[nodiscard]] bool ValidNativeMessage(const SteamNetworkingMessage_t *message, std::uint32_t maximumBytes) noexcept;

    class GnsTransport final : public INetworkTransport {
    public:
        explicit GnsTransport(std::string resolverServer = {}) : resolverServer_(std::move(resolverServer)) {}

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
        void DrainNativeCallbacks();
        void End(ConnectionSlot &slot, NetworkTransportEventKind kind, const ErrorCodeDescriptor *failure = nullptr);
        void Enqueue(NetworkTransportEvent event);
        void PollMessages();
        [[nodiscard]] bool PollMessage(ConnectionSlot &slot);
        [[nodiscard]] const ErrorCodeDescriptor *StartResolution(ConnectionSlot &slot) const;
        void AdvancePending(std::chrono::steady_clock::time_point now);
        void AdvancePendingSlot(ConnectionSlot &slot, std::chrono::steady_clock::time_point now);

        // Public operations hold mutex_. Native callbacks never acquire it:
        // they take host mutex -> callbackMutex_ and publish owned status data.
        // PollEvents drains that queue under mutex_ after RunCallbacks returns.
        mutable std::mutex mutex_;
        std::mutex callbackMutex_;
        std::deque<SteamNetConnectionStatusChangedCallback_t> callbacks_;
        ISteamNetworkingSockets *native_{};
        NetworkTransportConfig config_{};
        ListenerHandle listener_{};
        HSteamListenSocket nativeListener_{k_HSteamListenSocket_Invalid};
        std::vector<ConnectionSlot> connections_;
        std::deque<NetworkTransportEvent> events_;
        NetworkTransportStats stats_{};
        std::uint64_t capabilityRevision_{1};
        std::thread::id ownerThread_{};
        std::uint32_t listenerMaximumConnections_{};
        // Empty in production. An internal test may route DNS to a silent local
        // server to make in-flight cancellation and timeout deterministic.
        std::string resolverServer_;
        bool caresInitialized_{};
        bool overflow_{};
        bool callbackOverflow_{};
        bool initialized_{};
        bool shutdown_{};
    };
}  // namespace Horo::Network
