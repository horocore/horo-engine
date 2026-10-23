#pragma once

#include "GnsAddressResolution.h"
#include "GnsNativeCall.h"
#include "GnsSecurityInternal.h"
#include "GnsTransportFactory.h"

#include <array>
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
        ListenerHandle listener{};  // Exact accepting listener generation; invalid for outbound connections.
        NetworkOperationGeneration admissionGeneration{};
    };

    [[nodiscard]] bool ValidNativeMessage(const SteamNetworkingMessage_t *message, std::uint32_t maximumBytes) noexcept;
    [[nodiscard]] bool ValidGnsTransportConfig(const NetworkTransportConfig &config) noexcept;

    class GnsTransport final : public INetworkTransport {
    public:
        explicit GnsTransport(std::string resolverServer = {}) : resolverServer_(std::move(resolverServer)) {}

        /** @brief Cold qualification-only foreign cleanup boundary; function reference cannot be null.
         * @param kill Fixed executable code that must outlive this adapter. Production factories always use the real default.
         * @details One pointer is read only during lifecycle teardown, never the I/O/frame hot path.
         * Failed cleanup transfers only pending native ownership, never this override, to the process retry owner.
         */
        explicit GnsTransport(void (&kill)()) : nativeOwner_{.kill = &kill} {}

        GnsTransport(std::shared_ptr<GnsSecurity> security, const NetworkTransportConfig &config)
            : config_(config), security_(std::move(security)) {}

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
        /** @brief Borrow exact native evidence on the owning thread, binding the one admission generation. */
        [[nodiscard]] Result<SteamNetworkingSocketsLib::HoroNativeChannel> ReadSecurePeer(ConnectionHandle connection,
                                                                                          NetworkOperationGeneration generation);
        /** @brief Revalidate and attest the same locked native owner using bounded public context. */
        [[nodiscard]] Result<AuthenticationDigest> BindSecurePeer(ConnectionHandle connection, NetworkOperationGeneration generation,
                                                                  const SteamNetworkingSocketsLib::HoroNativeChannel &expected,
                                                                  const std::array<std::byte, 192> &context);

    private:
        /** @brief Reserve/start the one process owner under transport lock; no host issuer callback. */
        [[nodiscard]] Result<void> StartNativeOwner(const NetworkTransportConfig &config);
        /** @brief Prepare the bounded native request under transport lock before calling the host issuer. */
        [[nodiscard]] Result<std::vector<std::byte>> PrepareCertificateRequest();
        /** @brief Provision between lexical lock scopes; caller retires on failure before publishing initialization. */
        [[nodiscard]] Result<void> ProvisionNative();
        /** @brief Close bounded connections/callback state under transport lock before native Kill fallback. */
        [[nodiscard]] bool CloseNativeConnections() noexcept;
        [[nodiscard]] Result<ConnectionHandle> ReserveConnection();
        [[nodiscard]] Result<void> StartNative(ConnectionSlot &slot, const SteamNetworkingIPAddr &address);
        [[nodiscard]] ConnectionSlot *Find(ConnectionHandle handle) noexcept;
        [[nodiscard]] ConnectionSlot *Find(HSteamNetConnection native) noexcept;
        /** @brief Reserve the bounded original-listener pending slot and accept native ownership without publishing readiness. */
        void AcceptPendingInbound(const SteamNetConnectionStatusChangedCallback_t &status);
        void OnStatusOwned(const SteamNetConnectionStatusChangedCallback_t &status);
        void DrainNativeCallbacks();
        void End(ConnectionSlot &slot, NetworkTransportEventKind kind, const ErrorCodeDescriptor *failure = nullptr);
        void Enqueue(NetworkTransportEvent event);
        void PollMessages();
        [[nodiscard]] bool PollMessage(ConnectionSlot &slot);

        void AdvancePending(std::chrono::steady_clock::time_point now);
        void AdvancePendingSlot(ConnectionSlot &slot, std::chrono::steady_clock::time_point now);

        // Public operations hold mutex_. Native callbacks never acquire it:
        // they take host mutex -> callbackMutex_ and publish owned status data.
        // PollEvents drains that queue under mutex_ after RunCallbacks returns.
        mutable std::mutex mutex_;
        std::mutex callbackMutex_;
        std::deque<SteamNetConnectionStatusChangedCallback_t> callbacks_;

        struct NativeOwnership final {
            ISteamNetworkingSockets *sockets{};
            std::uint64_t rootGeneration{};
            bool resolverInitialized{};
            void (*kill)(){&GameNetworkingSockets_Kill};  // Non-null: real default or constructor-bound function reference.
        };

        // mutex_ protects the complete native lifetime; normal I/O also requires ownerThread_.
        // Sockets, selected-root generation and resolver initialization retire together on
        // shutdown/partial failure. Failed native retirement transfers to NativeHost retry ownership.
        NativeOwnership nativeOwner_;
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
        bool overflow_{};
        bool callbackOverflow_{};
        bool initialized_{};
        bool shutdown_{};
        std::shared_ptr<GnsSecurity> security_;
    };
}  // namespace Horo::Network
