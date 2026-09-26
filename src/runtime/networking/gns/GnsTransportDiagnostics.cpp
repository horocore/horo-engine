#include "GnsTransportInternal.h"
#include "Horo/Network/NetworkErrors.h"

#include <algorithm>

namespace Horo::Network {
    NetworkTransportStats GnsTransport::Stats() const noexcept {
        std::lock_guard lock(mutex_);
        return stats_;
    }

    Result<NetworkConnectionStats> GnsTransport::ConnectionStats(const ConnectionHandle connection) const {
        std::lock_guard lock(mutex_);
        if (shutdown_)
            return Result<NetworkConnectionStats>::Failure(MakeError(NetworkErrors::TransportShuttingDown));
        if (!connection.IsValid() || connection.Slot() >= connections_.size() || connections_[connection.Slot()].handle != connection)
            return Result<NetworkConnectionStats>::Failure(MakeError(NetworkErrors::TransportHandleInvalid));
        const auto &slot = connections_[connection.Slot()];
        if (slot.phase != ConnectionPhase::Connected)
            return Result<NetworkConnectionStats>::Failure(MakeError(NetworkErrors::TransportConnectionFailed));
        SteamNetConnectionRealTimeStatus_t nativeStatus{};
        if (native_->GetConnectionRealTimeStatus(slot.native, &nativeStatus, 0, nullptr) != k_EResultOK)
            return Result<NetworkConnectionStats>::Failure(MakeError(NetworkErrors::TransportNativeUnavailable));
        NetworkConnectionStats result{};
        if (nativeStatus.m_nPing >= 0)
            result.pingMilliseconds = static_cast<std::uint32_t>(nativeStatus.m_nPing);
        if (nativeStatus.m_flConnectionQualityLocal >= 0.0f && nativeStatus.m_flConnectionQualityLocal <= 1.0f)
            result.localDeliveryQuality = nativeStatus.m_flConnectionQualityLocal;
        result.pendingReliableBytes = static_cast<std::uint32_t>(std::max(0, nativeStatus.m_cbPendingReliable));
        result.pendingUnreliableBytes = static_cast<std::uint32_t>(std::max(0, nativeStatus.m_cbPendingUnreliable));
        result.sendRateBytesPerSecond = static_cast<std::uint32_t>(std::max(0, nativeStatus.m_nSendRateBytesPerSecond));
        return Result<NetworkConnectionStats>::Success(result);
    }

    TransportCapabilities GnsTransport::Capabilities() const noexcept {
        std::lock_guard lock(mutex_);
        TransportCapabilities capabilities{};
        capabilities.revision = capabilityRevision_;
        capabilities.delivery.fill(TransportSupport::Unsupported);
        const auto support = initialized_ && !shutdown_ ? TransportSupport::Available : TransportSupport::Unavailable;
        capabilities.delivery[static_cast<std::size_t>(DeliveryPolicy::UnreliableUnordered)] = support;
        capabilities.delivery[static_cast<std::size_t>(DeliveryPolicy::ReliableOrdered)] = support;
        if (initialized_ && !shutdown_) {
            capabilities.maximumChannels = 1;
            capabilities.maximumMessageBytes = config_.maximumMessageBytes;
        }
        return capabilities;
    }
}  // namespace Horo::Network
