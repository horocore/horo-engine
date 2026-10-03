#pragma once

#include "Horo/PlatformServices/PlatformProviderAdmission.h"

#include <cstddef>
#include <memory>
#include <mutex>
#include <thread>

namespace Horo::PlatformServices {
    /** @brief Target-private candidate ownership shared by contribution admission and native operation lifecycle. */
    struct PlatformProviderCandidateState final {
        std::mutex mutex;  // Protects admission, lease count and native retirement state; callbacks run outside it.
        std::weak_ptr<PlatformProviderRetirementState> retirement;
        std::shared_ptr<void> moduleCodeLease;
        PlatformProviderContributionDescriptor descriptor;
        PlatformProviderId provider;
        void *candidate{};
        HoroPlatformProviderRetireFunc retire{};
        HoroPlatformProviderDestroyFunc destroy{};
        HoroPlatformProviderOperations operations{};
        std::thread::id ownerThread;
        std::size_t leaseCount{};
        bool backendActive{};
        bool revoked{};
        bool retiring{};
        bool retired{};
    };
}  // namespace Horo::PlatformServices
