#pragma once

#include "GnsAddressResolution.h"
#include "GnsNativeCall.h"
#include "GnsSecurityInternal.h"
#include "GnsTransportFactory.h"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
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

    /** @brief Closed qualification faults at the native lane boundary; production selects neither. */
    enum class GnsLaneFailure : std::uint8_t {
        ConfigureRejected,
        SendRejected
    };

    /** @brief Stable borrowed qualification state, confined to the transport owner thread.
     * SendRejected never submits its message to the SDK. The caller's existing RAII owner
     * synchronously invokes Release -> ReleaseFunc -> m_pfnFreeData before Send returns.
     * Therefore no SDK worker retains this state or callback; Shutdown cannot leave a
     * queued reference. Each rejected Send releases its message before another Send.
     * The original payload/free pair is retired before invoking native cleanup; the
     * pinned DefaultFreeData only calls free and cannot reenter engine code.
     * Declare this nonmoving state before the borrowing transport and
     * retain it through transport destruction and callback drain.
     */
    struct GnsLaneFailureInjection final {
        explicit GnsLaneFailureInjection(const GnsLaneFailure selected) noexcept : failure(selected) {}

        GnsLaneFailureInjection(const GnsLaneFailureInjection &) = delete;
        GnsLaneFailureInjection &operator=(const GnsLaneFailureInjection &) = delete;
        GnsLaneFailureInjection(GnsLaneFailureInjection &&) = delete;
        GnsLaneFailureInjection &operator=(GnsLaneFailureInjection &&) = delete;

        const GnsLaneFailure failure;
        std::size_t configured{};
        std::size_t sends{};
        std::size_t freedPayloads{};
        std::thread::id freedOn{};
        void (*originalFree)(SteamNetworkingMessage_t *){};
        std::byte *originalPayload{};
    };

    /** @brief Native lane operations with an optional typed, owner-thread qualification fault. */
    class GnsLaneOperations final {
    public:
        explicit GnsLaneOperations(GnsLaneFailureInjection *failure = nullptr) noexcept : failure_(failure) {}

        [[nodiscard]] EResult ConfigureLanes(ISteamNetworkingSockets &sockets, HSteamNetConnection connection, int count) const {
            if (failure_) {
                ++failure_->configured;
                if (failure_->failure == GnsLaneFailure::ConfigureRejected)
                    return k_EResultInvalidParam;
            }
            return sockets.ConfigureConnectionLanes(connection, count, nullptr, nullptr);
        }

        void SendMessage(ISteamNetworkingSockets &sockets, SteamNetworkingMessage_t **message, int64 *result) const {
            if (!failure_ || failure_->failure != GnsLaneFailure::SendRejected) {
                sockets.SendMessages(1, message, result, false);
                return;
            }
            ++failure_->sends;
            failure_->originalFree = (*message)->m_pfnFreeData;
            failure_->originalPayload = static_cast<std::byte *>((*message)->m_pData);
            // Qualification-only rejected messages never enter the SDK. The pinned
            // ReleaseFunc only checks m_pData for null before invoking this callback;
            // it does not inspect payload bytes. Temporarily lend this pointer slot
            // to the stable context, retaining the exact native payload separately.
            (*message)->m_pData = failure_;
            (*message)->m_pfnFreeData = &RecordFreedPayload;
            *result = -static_cast<int64>(k_EResultLimitExceeded);
            // Leave the exact native allocation caller-owned. No SDK queue sees the probe.
        }

    private:
        /** @brief Observe the pinned native Release callback, then restore and invoke its real data free. */
        static void RecordFreedPayload(SteamNetworkingMessage_t *message) {
            auto &failure = *static_cast<GnsLaneFailureInjection *>(message->m_pData);
            const auto originalFree = failure.originalFree;
            message->m_pData = failure.originalPayload;
            message->m_pfnFreeData = originalFree;
            // Restore the native payload and free function before invoking user code.
            // Retire this frame's borrow first, so a repeated or nested rejected send
            // cannot overwrite anything this callback still needs. Size, bytes and
            // m_nUserData have never changed; no queued message observes the context.
            failure.originalPayload = nullptr;
            failure.originalFree = nullptr;
            ++failure.freedPayloads;
            failure.freedOn = std::this_thread::get_id();
            if (originalFree)
                originalFree(message);
        }

        GnsLaneFailureInjection *failure_{};
    };

    /** @brief Cold, target-private composition inputs for one native transport owner.
     * kill is non-null code retained through teardown; security is owned; the optional
     * failure state is a stable owner-thread borrow governed by GnsLaneFailureInjection.
     */
    struct GnsTransportSetup final {
        std::string resolverServer;
        std::reference_wrapper<void()> kill{GameNetworkingSockets_Kill};
        std::shared_ptr<GnsSecurity> security;
        NetworkTransportConfig config{};
        GnsLaneFailureInjection *laneFailure{};
    };

    class GnsTransport final : public INetworkTransport {
    public:
        /** @brief Capture cold composition inputs without invoking native APIs or selecting a backend. */
        explicit GnsTransport(GnsTransportSetup setup = {})
            : nativeOwner_{.kill = &setup.kill.get()}, lanes_(setup.laneFailure), config_(setup.config),
              resolverServer_(std::move(setup.resolverServer)), security_(std::move(setup.security)) {}

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
        /** @brief Configure the exact selected native lanes before publishing directional readiness. */
        [[nodiscard]] bool ConfigureLanes(const ConnectionSlot &slot) noexcept;
        /** @brief Copy into one native-owned lane message; retain cleanup ownership until successful native transfer. */
        [[nodiscard]] std::optional<std::int64_t> SendOnLane(const ConnectionSlot &slot, ChannelId channel,
                                                             std::span<const std::byte> payload, DeliveryPolicy delivery);
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
        const GnsLaneOperations lanes_;
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
