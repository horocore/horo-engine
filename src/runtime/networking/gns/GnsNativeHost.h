#pragma once
/** @file GnsNativeHost.h @brief The existing single native process owner and retained cleanup retry token. */
#include <cstdint>
#include <mutex>

namespace Horo::Network {
    class GnsTransport;

    /** @brief Mutex protects callback lifetime, process exclusivity and cleanup ownership after adapter destruction. */
    struct NativeHostState final {
        std::mutex mutex;
        GnsTransport *active{};
        bool shuttingDown{};
        bool cleanupRunning{};
        bool nativeCleanupPending{};
        std::uint64_t cleanupRoot{};
    };

    /** @brief Borrow the same process-global GNS owner; this does not select or initialize a backend. */
    NativeHostState &NativeHost();
}  // namespace Horo::Network
