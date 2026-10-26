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
        if (GnsDetail::NativeCall([this, &slot, &nativeStatus] {
            return nativeOwner_.sockets->GetConnectionRealTimeStatus(slot.native, &nativeStatus, 0, nullptr);
        }).value_or(k_EResultFail) != k_EResultOK)
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
        using enum TransportSupport;
        std::lock_guard lock(mutex_);
        TransportCapabilities capabilities{};
        capabilities.revision = capabilityRevision_;
        capabilities.delivery.fill(Unsupported);
        // An inert exact secure candidate declaration is not native/auth readiness.
        const bool declared = (initialized_ || security_) && !shutdown_;
        const auto support = declared ? Available : Unavailable;
        capabilities.delivery[static_cast<std::size_t>(DeliveryPolicy::UnreliableUnordered)] = support;
        capabilities.delivery[static_cast<std::size_t>(DeliveryPolicy::ReliableOrdered)] = support;
        if (declared) {
            capabilities.maximumChannels = config_.maximumChannels;
            capabilities.maximumMessageBytes = config_.maximumMessageBytes;
        }
        return capabilities;
    }
}  // namespace Horo::Network
