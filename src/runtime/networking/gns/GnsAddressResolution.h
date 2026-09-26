#pragma once

#include "Horo/Foundation/Result.h"
#include "Horo/Network/NetworkAddress.h"

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <steam/steamnetworkingtypes.h>
#include <string>

namespace Horo::Network::GnsDetail {
    struct Resolution final {
        std::mutex mutex;
        std::optional<SteamNetworkingIPAddr> address;
        bool complete{};
    };

    [[nodiscard]] Result<SteamNetworkingIPAddr> NativeAddress(const NetworkAddress &address);
    void ResolveHostname(const std::shared_ptr<Resolution> &state, const std::string &hostname, std::uint16_t port);
}  // namespace Horo::Network::GnsDetail
