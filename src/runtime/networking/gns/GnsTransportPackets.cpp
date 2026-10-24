#include "GnsTransportInternal.h"
#include "Horo/Network/NetworkErrors.h"

#include <algorithm>
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
        if (channel.Value() >= config_.maximumChannels || payload.size() > config_.maximumMessageBytes)
            return Fail(NetworkErrors::TransportLimitExceeded);
        if (delivery != DeliveryPolicy::ReliableOrdered && delivery != DeliveryPolicy::UnreliableUnordered)
            return Fail(NetworkErrors::TransportDeliveryUnsupported);
        const auto result = SendOnLane(*slot, channel, payload, delivery);
        if (!result.has_value())
            return Fail(NetworkErrors::TransportNativeUnavailable);
        if (*result == -static_cast<std::int64_t>(k_EResultLimitExceeded))
            return Fail(delivery == DeliveryPolicy::ReliableOrdered ? NetworkErrors::TransportReliableBackpressure
                                                                    : NetworkErrors::TransportLimitExceeded);
        if (*result <= 0)
            return Fail(NetworkErrors::TransportConnectionFailed);
        ++stats_.sentMessages;
        stats_.sentBytes += payload.size();
        return Result<void>::Success();
    }

    /** @copydoc GnsTransport::ConfigureLanes */
    bool GnsTransport::ConfigureLanes(const ConnectionSlot &slot) noexcept {
        // Native streams have independent reliable ordering. Equal priority/weight
        // avoids assigning semantic priority to a host-selected channel number.
        return GnsDetail::NativeCall([this, &slot] {
            return lanes_.ConfigureLanes(*nativeOwner_.sockets, slot.native, static_cast<int>(config_.maximumChannels));
        }).value_or(k_EResultFail) == k_EResultOK;
    }

    /** @copydoc GnsTransport::SendOnLane */
    std::optional<std::int64_t> GnsTransport::SendOnLane(const ConnectionSlot &slot, const ChannelId channel,
                                                         const std::span<const std::byte> payload, const DeliveryPolicy delivery) {
        auto *message = GnsDetail::NativeCall([payload] {
            return SteamNetworkingUtils()->AllocateMessage(static_cast<int>(payload.size()));
        }).value_or(nullptr);
        if (!message)
            return std::nullopt;
        // SendMessages(false) nulls this exact pointer when native ownership transfers.
        // A rejected message remains ours and must be released, including on exceptions.
        const auto release = [&message](SteamNetworkingMessage_t *) noexcept {
            if (message)
                (void)GnsDetail::NativeVoid([&message] {
                    message->Release();
                });
        };
        const std::unique_ptr<SteamNetworkingMessage_t, decltype(release)> owner(message, release);
        if (message->m_cbSize != static_cast<int>(payload.size()) || (!payload.empty() && !message->m_pData))
            return std::nullopt;
        if (!payload.empty())
            std::ranges::copy(payload, static_cast<std::byte *>(message->m_pData));
        message->m_conn = slot.native;
        message->m_idxLane = static_cast<std::uint16_t>(channel.Value());
        message->m_nFlags =
            delivery == DeliveryPolicy::ReliableOrdered ? k_nSteamNetworkingSend_Reliable : k_nSteamNetworkingSend_Unreliable;
        return GnsDetail::NativeCall([this, &message] {
            int64 result{};
            lanes_.SendMessage(*nativeOwner_.sockets, &message, &result);
            return static_cast<std::int64_t>(result);
        });
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
        const int count = GnsDetail::NativeCall([this, &slot, &message] {
            return nativeOwner_.sockets->ReceiveMessagesOnConnection(slot.native, &message, 1);
        }).value_or(-1);
        if (count < 0) {
            End(slot, NetworkTransportEventKind::Failed, &NetworkErrors::TransportConnectionFailed);
            return false;
        }
        if (count == 0)
            return false;
        const auto release = [](SteamNetworkingMessage_t *received) {
            if (received)
                (void)GnsDetail::NativeVoid([received] {
                    received->Release();
                });
        };
        const std::unique_ptr<SteamNetworkingMessage_t, decltype(release)> owned(message, release);
        if (!ValidNativeMessage(message, config_.maximumMessageBytes) || message->m_idxLane >= config_.maximumChannels) {
            End(slot, NetworkTransportEventKind::Failed, &NetworkErrors::TransportMalformedPacket);
            return false;
        }
        NetworkTransportEvent event{.kind = NetworkTransportEventKind::PacketReceived,
                                    .connection = slot.handle,
                                    .channel = ChannelId::Create(message->m_idxLane, config_.maximumChannels).Value(),
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
