#pragma once

/**
 * @file NetworkTransport.h
 * @brief Backend-neutral packet transport boundary selected only by a host.
 */

#include "Horo/Foundation/CancellationToken.h"
#include "Horo/Foundation/Result.h"
#include "Horo/Network/NetworkAddress.h"
#include "Horo/Network/NetworkHandles.h"
#include "Horo/Network/TransportCapabilities.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace Horo::Network {
    /** @brief Finite native message and event limits fixed at initialization. */
    struct NetworkTransportConfig final {
        std::uint32_t maximumConnections{64};    /**< Includes inbound and outbound handles. */
        std::uint32_t maximumEventsPerPoll{64};  /**< Maximum owner callbacks per poll. */
        std::uint32_t maximumMessageBytes{1200}; /**< Exact per-message admission ceiling. */
    };

    /** @brief Validated local bind and inbound admission bounds. */
    struct NetworkListenRequest final {
        NetworkAddress bindAddress;           /**< Numeric address; DNS is not a bind address. */
        std::uint32_t maximumConnections{32}; /**< Inclusive inbound connection limit. */
    };

    /** @brief Non-blocking outbound request, including caller-owned cancellation. */
    struct NetworkConnectRequest final {
        NetworkAddress endpoint;                 /**< Numeric or DNS endpoint. */
        std::chrono::milliseconds timeout{5000}; /**< Positive finite connection deadline. */
        CancellationToken cancellation{};        /**< Checked before admission and while pending. */
    };

    /** @brief Closed transport-owned event vocabulary. */
    enum class NetworkTransportEventKind : std::uint8_t {
        ListenerReady,
        Accepted,
        Connected,
        PacketReceived,
        Closed,
        Failed
    };

    /** @brief Owned event; no native handle, pointer, or backend diagnostic escapes. */
    struct NetworkTransportEvent final {
        NetworkTransportEventKind kind{NetworkTransportEventKind::Failed}; /**< Exact event kind. */
        ListenerHandle listener{};                                         /**< Populated for listener events and acceptance. */
        ConnectionHandle connection{};                                     /**< Populated for connection events. */
        DeliveryPolicy delivery{DeliveryPolicy::UnreliableUnordered};      /**< Packet delivery class. */
        std::vector<std::byte> payload;                                    /**< Owned packet bytes for PacketReceived only. */
        const ErrorCodeDescriptor *failure{};                              /**< Stable typed descriptor for Failed only. */
    };

    /** @brief Transport-level observed counters, not session/authentication evidence. */
    struct NetworkTransportStats final {
        std::uint64_t sentMessages{};      /**< Native send calls accepted. */
        std::uint64_t receivedMessages{};  /**< Valid native messages handed to the owner. */
        std::uint64_t sentBytes{};         /**< Payload bytes accepted by native send. */
        std::uint64_t receivedBytes{};     /**< Payload bytes handed to the owner. */
        std::uint32_t activeConnections{}; /**< Connected live handles. */
    };

    /** @brief Sanitized live native quality and queue snapshot for one connection. */
    struct NetworkConnectionStats final {
        std::optional<std::uint32_t> pingMilliseconds; /**< Empty until native ping is measured. */
        std::optional<float> localDeliveryQuality;     /**< Empty until native 0..1 quality is measured. */
        std::uint32_t pendingReliableBytes{};          /**< Native buffered/retransmission bytes, clamped non-negative. */
        std::uint32_t pendingUnreliableBytes{};        /**< Native buffered bytes, clamped non-negative. */
        std::uint32_t sendRateBytesPerSecond{};        /**< Native estimate, clamped non-negative. */
    };

    /** @brief Stack-only owner callback; a backend never retains this reference. */
    class INetworkTransportEventConsumer {
    public:
        virtual ~INetworkTransportEventConsumer() = default;
        /** @brief Consumes one owned event in poll order. @param event Moved event. */
        virtual void Consume(NetworkTransportEvent event) noexcept = 0;
    };

    /** @brief Unique host-owned transport; no implementation is auto-selected or substituted. */
    class INetworkTransport {
    public:
        virtual ~INetworkTransport() = default;
        /** @brief Initializes native state once. @param config Positive finite bounds. @return Typed status. */
        [[nodiscard]] virtual Result<void> Initialize(const NetworkTransportConfig &config) = 0;
        /** @brief Binds one local numeric endpoint. @param request Valid bind request. @return Owner-issued listener handle or failure. */
        [[nodiscard]] virtual Result<ListenerHandle> Listen(const NetworkListenRequest &request) = 0;
        /** @brief Starts an asynchronous connection. @param request Bounded endpoint and cancellation. @return Pending handle or failure.
         */
        [[nodiscard]] virtual Result<ConnectionHandle> Connect(const NetworkConnectRequest &request) = 0;
        /** @brief Copies and sends exact admitted bytes; no policy fallback. @param connection Live handle. @param channel Channel zero.
         * @param payload Borrowed bytes copied before return. @param delivery Exact mode. @return Typed status. */
        [[nodiscard]] virtual Result<void> Send(ConnectionHandle connection, ChannelId channel, std::span<const std::byte> payload,
                                                DeliveryPolicy delivery) = 0;
        /** @brief Closes a connection once; repeated close of the same generation succeeds. @param connection Issued handle. @return Typed
         * status. */
        [[nodiscard]] virtual Result<void> Close(ConnectionHandle connection) = 0;
        /** @brief Closes a listener and its admission boundary. @param listener Issued handle. @return Typed status. */
        [[nodiscard]] virtual Result<void> CloseListener(ListenerHandle listener) = 0;
        /** @brief Delivers at most configured callbacks on the owner thread. @param consumer Stack-only consumer. @return Number delivered
         * or typed failure. */
        [[nodiscard]] virtual Result<std::size_t> PollEvents(INetworkTransportEventConsumer &consumer) = 0;
        /** @brief Captures stable transport counters. @return Current typed snapshot. */
        [[nodiscard]] virtual NetworkTransportStats Stats() const noexcept = 0;
        /** @brief Maps native live statistics without native types. @param connection Exact live handle. @return Sanitized snapshot or
         * typed stale/unavailable failure. */
        [[nodiscard]] virtual Result<NetworkConnectionStats> ConnectionStats(ConnectionHandle connection) const = 0;
        /** @brief Returns exact current delivery support. @return Immutable candidate evidence. */
        [[nodiscard]] virtual TransportCapabilities Capabilities() const noexcept = 0;
        /** @brief Cancels all pending operations and releases native state; idempotent. */
        virtual void Shutdown() noexcept = 0;
    };
}  // namespace Horo::Network
