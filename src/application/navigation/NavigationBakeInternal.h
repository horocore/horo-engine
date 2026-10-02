#pragma once

#include "Horo/Application/NavigationBakeService.h"
#include "Horo/Assets/AssetCookCache.h"
#include "Horo/Navigation/NavigationErrors.h"

#include <atomic>

namespace Horo::Application::NavigationBakeDetail {
    using namespace Horo::Navigation;
    constexpr std::uint64_t Adopted = std::uint64_t{1} << 63U;

    /** @brief Shared worker lifetime; desired generation fences adoption without blocking the host thread. */
    struct ServiceState {
        NavigationBakeServiceConfig config;
        std::atomic<std::uint64_t> desired{};
        std::atomic<std::shared_ptr<const NavigationBakePublication>> published;
    };

    /** @brief One immutable source capture and stage-owned detached result; stages execute sequentially. */
    struct Attempt {
        NavigationBakeRequest request;
        std::uint64_t generation{};
        OperationId operation{};
        std::shared_ptr<CancellationSource> cancellation;
        std::vector<NavigationPreparedTile> prepared;
        std::shared_ptr<NavigationBakePublication> candidate;
        std::vector<std::uint8_t> envelope;
    };

    /** @brief Creates complete concrete gather/build/validate/publish work for the process scheduler. */
    NavigationBakeJobDescriptor Descriptor(const std::shared_ptr<ServiceState> &state, const std::shared_ptr<Attempt> &attempt);
    /** @brief Cancels a queued operation that was replaced before scheduler admission. */
    void CancelPending(OperationStore &operations, const std::shared_ptr<Attempt> &attempt) noexcept;
}  // namespace Horo::Application::NavigationBakeDetail
