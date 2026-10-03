#pragma once

#include "Horo/Foundation/Result.h"
#include "Horo/Network/NetworkAddress.h"

#include <ares.h>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <steam/steamnetworkingtypes.h>
#include <string>

namespace Horo::Network::GnsDetail {
    struct Resolution final {
        Resolution() = default;
        Resolution(const Resolution &) = delete;
        Resolution &operator=(const Resolution &) = delete;
        Resolution(Resolution &&) = delete;
        Resolution &operator=(Resolution &&) = delete;
        ~Resolution() noexcept;

        ares_channel_t *channel{};
        std::optional<SteamNetworkingIPAddr> address;
        std::size_t socketCursor{};
        std::uint16_t port{};
        bool complete{};
    };

    [[nodiscard]] Result<SteamNetworkingIPAddr> NativeAddress(const NetworkAddress &address);
    [[nodiscard]] bool StartResolution(Resolution &state, const std::string &hostname, std::uint16_t port, const std::string &server = {});
    [[nodiscard]] bool PollResolution(Resolution &state);
}  // namespace Horo::Network::GnsDetail
