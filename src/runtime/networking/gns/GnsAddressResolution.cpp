#include "GnsAddressResolution.h"

#include "Horo/Network/NetworkErrors.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstring>
#include <type_traits>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <netinet/in.h>
#include <sys/select.h>
#endif

namespace Horo::Network::GnsDetail {
    namespace {
        constexpr std::size_t MaximumAddressResults = 32;
        constexpr std::size_t MaximumReadySocketsPerPoll = 4;

        /** @brief Copies native socket bytes into an owned GNS address without aliasing socket structs. */
        std::optional<SteamNetworkingIPAddr> CopyAddress(const ares_addrinfo_node &node, const std::uint16_t port) {
            if (!node.ai_addr)
                return std::nullopt;
            SteamNetworkingIPAddr numeric{};
            if (node.ai_family == AF_INET && node.ai_addrlen >= sizeof(sockaddr_in)) {
                sockaddr_in address{};
                std::memcpy(&address, node.ai_addr, sizeof(address));
                static_assert(sizeof(decltype(address.sin_addr)) == 4);
                static_assert(std::is_trivially_copyable_v<decltype(address.sin_addr)>);
                // Preserve network-order bytes, not host-endian integer order.
                const auto bytes = std::bit_cast<std::array<std::byte, 4>>(address.sin_addr);
                const std::uint32_t ipv4 = (std::to_integer<std::uint32_t>(bytes[0]) << 24) |
                                           (std::to_integer<std::uint32_t>(bytes[1]) << 16) |
                                           (std::to_integer<std::uint32_t>(bytes[2]) << 8) | std::to_integer<std::uint32_t>(bytes[3]);
                numeric.SetIPv4(ipv4, port);
                return numeric;
            }
            if (node.ai_family == AF_INET6 && node.ai_addrlen >= sizeof(sockaddr_in6)) {
                sockaddr_in6 address{};
                std::memcpy(&address, node.ai_addr, sizeof(address));
                static_assert(sizeof(decltype(address.sin6_addr)) == 16);
                static_assert(std::is_trivially_copyable_v<decltype(address.sin6_addr)>);
                const auto bytes = std::bit_cast<std::array<std::uint8_t, 16>>(address.sin6_addr);
                numeric.SetIPv6(bytes.data(), port);
                return numeric;
            }
            return std::nullopt;
        }

        /** @brief Poll invokes this on the owner thread; synchronous cancel may invoke it on the closing thread.
         *  Only the resolution state is touched under the transport lock; user events remain owner-thread delivered.
         */
        void OnResolved(void *argument, const int status, int, ares_addrinfo *results) {
            auto &state = *static_cast<Resolution *>(argument);
            if (status == ARES_SUCCESS && results) {
                std::size_t examined{};
                for (const auto *node = results->nodes; node && examined < MaximumAddressResults; node = node->ai_next, ++examined) {
                    auto numeric = CopyAddress(*node, state.port);
                    if (!numeric)
                        continue;
                    if (!state.address || node->ai_family == AF_INET)
                        state.address = *numeric;
                    if (node->ai_family == AF_INET)
                        break;
                }
            }
            state.complete = true;
            if (results)
                ares_freeaddrinfo(results);
        }
    }  // namespace

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

    Resolution::~Resolution() noexcept {
        if (channel) {
            // Cancellation can synchronously call OnResolved. State is still alive
            // until ares_destroy returns; no transport pointer is retained.
            ares_cancel(channel);
            ares_destroy(channel);
        }
    }

    bool StartResolution(Resolution &state, const std::string &hostname, const std::uint16_t port, const std::string &server) {
        if (ares_init_options(&state.channel, nullptr, 0) != ARES_SUCCESS)
            return false;
        if (!server.empty() && ares_set_servers_ports_csv(state.channel, server.c_str()) != ARES_SUCCESS)
            return false;
        state.port = port;
        ares_addrinfo_hints hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_DGRAM;
        hints.ai_protocol = IPPROTO_UDP;
        ares_getaddrinfo(state.channel, hostname.c_str(), nullptr, &hints, &OnResolved, &state);
        return true;
    }

    bool PollResolution(Resolution &state) {
        if (state.complete)
            return true;
        std::array<ares_socket_t, ARES_GETSOCK_MAXNUM> sockets{};
        const int interest = ares_getsock(state.channel, sockets.data(), static_cast<int>(sockets.size()));
        fd_set readable{};
        fd_set writable{};
        FD_ZERO(&readable);
        FD_ZERO(&writable);
        int highest{};
        bool hasSockets{};
        for (std::size_t i = 0; i < sockets.size(); ++i) {
            if (!ARES_GETSOCK_READABLE(interest, i) && !ARES_GETSOCK_WRITABLE(interest, i))
                continue;
            hasSockets = true;
#ifndef _WIN32
            if (sockets[i] >= FD_SETSIZE)
                return false;
#endif
            if (ARES_GETSOCK_READABLE(interest, i))
                FD_SET(sockets[i], &readable);
            if (ARES_GETSOCK_WRITABLE(interest, i))
                FD_SET(sockets[i], &writable);
#ifndef _WIN32
            highest = std::max(highest, sockets[i]);
#endif
        }
        timeval zero{};
        if (hasSockets && select(highest + 1, &readable, &writable, nullptr, &zero) < 0)
            return false;
        std::array<ares_fd_events_t, MaximumReadySocketsPerPoll> ready{};
        std::size_t count{};
        for (std::size_t offset = 0; offset < sockets.size() && count < ready.size(); ++offset) {
            const auto i = (state.socketCursor + offset) % sockets.size();
            unsigned int events{};
            if (ARES_GETSOCK_READABLE(interest, i) && FD_ISSET(sockets[i], &readable))
                events |= ARES_FD_EVENT_READ;
            if (ARES_GETSOCK_WRITABLE(interest, i) && FD_ISSET(sockets[i], &writable))
                events |= ARES_FD_EVENT_WRITE;
            if (events != 0)
                ready[count++] = {.fd = sockets[i], .events = events};
        }
        state.socketCursor = (state.socketCursor + count + 1) % sockets.size();
        return ares_process_fds(state.channel, ready.data(), count, ARES_PROCESS_FLAG_NONE) == ARES_SUCCESS;
    }
}  // namespace Horo::Network::GnsDetail
