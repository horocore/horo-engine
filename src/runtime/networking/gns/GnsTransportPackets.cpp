#include "GnsTransportInternal.h"
#include "Horo/Network/NetworkErrors.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <new>
#include <span>
#include <steam/steamnetworkingsockets.h>
#include <utility>

namespace Horo::Network {
    namespace {
        [[nodiscard]] Result<void> Fail(const ErrorCodeDescriptor &code) {
            return Result<void>::Failure(MakeError(code));
        }
    }  // namespace

    bool ValidNativeMessage(const SteamNetworkingMessage_t *message, const std::uint32_t maximumBytes) noexcept {
        return message && message->m_cbSize >= 0 && static_cast<std::uint32_t>(message->m_cbSize) <= maximumBytes &&
               (message->m_cbSize == 0 || message->m_pData);
    }

    Result<void> GnsTransport::Send(const ConnectionHandle connection, const ChannelId channel, const std::span<const std::byte> payload,
                                    const DeliveryPolicy delivery) {
        std::lock_guard lock(mutex_);
        if (shutdown_)
            return Fail(NetworkErrors::TransportShuttingDown);
        const auto *slot = Find(connection);
        if (!slot)
            return Fail(NetworkErrors::TransportHandleInvalid);
        if (slot->phase != ConnectionPhase::Connected)
            return Fail(NetworkErrors::TransportConnectionFailed);
        if (channel.Value() != 0 || payload.size() > config_.maximumMessageBytes)
            return Fail(NetworkErrors::TransportLimitExceeded);
        if (delivery != DeliveryPolicy::ReliableOrdered && delivery != DeliveryPolicy::UnreliableUnordered)
            return Fail(NetworkErrors::TransportDeliveryUnsupported);
        const int flags = delivery == DeliveryPolicy::ReliableOrdered ? k_nSteamNetworkingSend_Reliable : k_nSteamNetworkingSend_Unreliable;
        const std::byte empty{};
        const void *bytes = payload.empty() ? static_cast<const void *>(&empty) : payload.data();
        const auto result =
            native_->SendMessageToConnection(slot->native, bytes, static_cast<std::uint32_t>(payload.size()), flags, nullptr);
        if (result == k_EResultLimitExceeded)
            return Fail(delivery == DeliveryPolicy::ReliableOrdered ? NetworkErrors::TransportReliableBackpressure
                                                                    : NetworkErrors::TransportLimitExceeded);
        if (result != k_EResultOK)
            return Fail(NetworkErrors::TransportConnectionFailed);
        ++stats_.sentMessages;
        stats_.sentBytes += payload.size();
        return Result<void>::Success();
    }

    void GnsTransport::PollMessages() {
        const std::size_t capacity = std::size_t{config_.maximumConnections} * 4 + config_.maximumEventsPerPoll + 1;
        for (auto &slot : connections_) {
            if (slot.phase != ConnectionPhase::Connected)
                continue;
            while (events_.size() < capacity && events_.size() < config_.maximumEventsPerPoll) {
                if (!PollMessage(slot))
                    break;
            }
        }
    }

    bool GnsTransport::PollMessage(ConnectionSlot &slot) {
        SteamNetworkingMessage_t *message{};
        const int count = native_->ReceiveMessagesOnConnection(slot.native, &message, 1);
        if (count < 0) {
            End(slot, NetworkTransportEventKind::Failed, &NetworkErrors::TransportConnectionFailed);
            return false;
        }
        if (count == 0)
            return false;
        const auto release = [](SteamNetworkingMessage_t *received) {
            if (received)
                received->Release();
        };
        const std::unique_ptr<SteamNetworkingMessage_t, decltype(release)> owned(message, release);
        if (!ValidNativeMessage(message, config_.maximumMessageBytes)) {
            End(slot, NetworkTransportEventKind::Failed, &NetworkErrors::TransportMalformedPacket);
            return false;
        }
        NetworkTransportEvent event{.kind = NetworkTransportEventKind::PacketReceived,
                                    .connection = slot.handle,
                                    .delivery = (message->m_nFlags & k_nSteamNetworkingSend_Reliable)
                                                    ? DeliveryPolicy::ReliableOrdered
                                                    : DeliveryPolicy::UnreliableUnordered};
        try {
            if (message->m_cbSize != 0) {
                const auto *first = static_cast<const std::byte *>(message->m_pData);
                event.payload.assign(first, first + message->m_cbSize);
            }
        } catch (const std::bad_alloc &) {
            End(slot, NetworkTransportEventKind::Failed, &NetworkErrors::TransportCapabilityUnavailable);
            return false;
        }
        ++stats_.receivedMessages;
        stats_.receivedBytes += event.payload.size();
        Enqueue(std::move(event));
        return true;
    }
}  // namespace Horo::Network
