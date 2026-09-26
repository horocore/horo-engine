#include "GnsAddressResolution.h"

#include "Horo/Network/NetworkErrors.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <netdb.h>
#include <netinet/in.h>
#endif

namespace Horo::Network::GnsDetail {
    Result<SteamNetworkingIPAddr> NativeAddress(const NetworkAddress &address) {
        if (!address.IsValid())
            return Result<SteamNetworkingIPAddr>::Failure(MakeError(NetworkErrors::NetworkAddressInvalid));
        if (address.RequiresResolution())
            return Result<SteamNetworkingIPAddr>::Failure(MakeError(NetworkErrors::NetworkAddressUnsupported));
        SteamNetworkingIPAddr native{};
        const auto bytes = address.AddressBytes();
        if (address.Kind() == NetworkAddressKind::Ipv4) {
            const std::uint32_t ipv4 = (std::uint32_t{bytes[0]} << 24) | (std::uint32_t{bytes[1]} << 16) | (std::uint32_t{bytes[2]} << 8) |
                                       std::uint32_t{bytes[3]};
            native.SetIPv4(ipv4, address.Port());
        } else if (address.Kind() == NetworkAddressKind::Ipv6) {
            native.SetIPv6(bytes.data(), address.Port());
        } else {
            return Result<SteamNetworkingIPAddr>::Failure(MakeError(NetworkErrors::NetworkAddressInvalid));
        }
        return Result<SteamNetworkingIPAddr>::Success(native);
    }

    /** @brief Resolves into owned numeric data only; no transport reference crosses the worker boundary. */
    void ResolveHostname(const std::shared_ptr<Resolution> &state, const std::string &hostname, const std::uint16_t port) {
        addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_DGRAM;
        hints.ai_protocol = IPPROTO_UDP;
        addrinfo *results{};
        const int status = getaddrinfo(hostname.c_str(), nullptr, &hints, &results);
        std::optional<SteamNetworkingIPAddr> found;
        if (status == 0) {
            for (auto *entry = results; entry; entry = entry->ai_next) {
                SteamNetworkingIPAddr numeric{};
                if (entry->ai_family == AF_INET && entry->ai_addrlen >= sizeof(sockaddr_in)) {
                    const auto *addr = reinterpret_cast<const sockaddr_in *>(entry->ai_addr);
                    const auto *bytes = reinterpret_cast<const std::uint8_t *>(&addr->sin_addr);
                    const std::uint32_t ipv4 = (std::uint32_t{bytes[0]} << 24) | (std::uint32_t{bytes[1]} << 16) |
                                               (std::uint32_t{bytes[2]} << 8) | std::uint32_t{bytes[3]};
                    numeric.SetIPv4(ipv4, port);
                } else if (entry->ai_family == AF_INET6 && entry->ai_addrlen >= sizeof(sockaddr_in6)) {
                    const auto *addr = reinterpret_cast<const sockaddr_in6 *>(entry->ai_addr);
                    numeric.SetIPv6(reinterpret_cast<const std::uint8_t *>(&addr->sin6_addr), port);
                } else {
                    continue;
                }
                // Prefer IPv4 for canonical localhost against an IPv4 bind;
                // IPv6-only names remain supported.
                if (!found || entry->ai_family == AF_INET)
                    found = numeric;
                if (entry->ai_family == AF_INET)
                    break;
            }
        }
        if (results)
            freeaddrinfo(results);
        {
            std::lock_guard lock(state->mutex);
            state->address = found;
            state->complete = true;
        }
    }
}  // namespace Horo::Network::GnsDetail
