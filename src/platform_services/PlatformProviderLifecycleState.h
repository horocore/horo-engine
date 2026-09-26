#pragma once

#include "Horo/PlatformServices/PlatformProviderAdmission.h"

#include <array>
#include <chrono>
#include <cstddef>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

namespace Horo::PlatformServices {
    struct PlatformProviderLifecycleState final {
        static constexpr std::size_t MaximumRequests = 64;
        static constexpr std::size_t MaximumPayloadBytes = 4096;

        struct Completion final {
            std::chrono::steady_clock::time_point receivedAt;
            std::uint64_t requestId{};
            std::uint64_t requestGeneration{};
            std::uint64_t sessionRevision{};
            std::uint32_t service{};
            std::uint32_t operation{};
            std::uint32_t resultCode{};
            std::uint32_t size{};
            std::array<std::byte, MaximumPayloadBytes> payload{};
        };

        struct InFlight final {
            PlatformRequestId id;
            PlatformRequestGeneration generation;
            std::uint64_t sessionRevision{};
            PlatformServiceKind service;
            std::uint32_t operation{};
            std::chrono::steady_clock::time_point deadline;
            std::array<std::byte, sizeof(std::uint64_t)> payload{};
            std::optional<PlatformProviderRequestLease> lease;
            bool cancellationSent{};

            InFlight(PlatformRequestId requestId, PlatformRequestGeneration requestGeneration, std::uint64_t revision,
                     PlatformServiceKind requestService, std::uint32_t requestOperation,
                     std::chrono::steady_clock::time_point requestDeadline)
                : id(requestId), generation(requestGeneration), sessionRevision(revision), service(requestService),
                  operation(requestOperation), deadline(requestDeadline) {}
        };

        explicit PlatformProviderLifecycleState(const PlatformRequestGeneration generation, PlatformProviderRequestPolicy policy)
            : requests({.activeCapacity = MaximumRequests,
                        .terminalCapacity = MaximumRequests,
                        .observerCapacity = MaximumRequests,
                        .generation = generation}),
              requestPolicy(std::move(policy)) {
            inFlight.reserve(MaximumRequests);
        }

        struct LifecycleStages final {
            bool servicesAttempted{};
            bool sessionAttempted{};
            bool ingressAttempted{};
            bool admissionClosed{};
            bool closing{};
            bool ingressClosed{};
            bool drained{};
            bool sessionStopped{};
            bool servicesStopped{};
            bool closed{};
        };

        std::mutex mutex;  // Protects callback ingress and session evidence; native calls run without this lock.
        std::array<Completion, MaximumRequests> completions{};
        std::size_t completionHead{};
        std::size_t completionCount{};
        PlatformProviderSessionObservation session{};
        bool callbackOpen{true};
        HoroPlatformProviderSink sink{};
        HoroPlatformProviderOperations operations{};
        void *candidate{};  // NOSONAR(cpp:S5008) The versioned provider C ABI defines candidates as opaque void pointers.
        std::optional<PlatformProviderCandidateLease> lease;
        PlatformRequestStore requests;
        PlatformProviderRequestPolicy requestPolicy;
        std::vector<InFlight> inFlight;
        std::uint32_t availableServices{};
        LifecycleStages stages;
        std::shared_ptr<PlatformProviderLifecycleState> quarantine;  // BUSY teardown retains the callback context and code.
    };

    namespace Detail {
        template <typename Callback, typename... Arguments>
        [[nodiscard]] HoroExtensionStatus InvokeProvider(Callback callback, Arguments... arguments) noexcept {
            try {
                return callback(arguments...);
            } catch (...) {  // NOSONAR: no C++ exception may cross a provider ABI boundary.
                return HORO_EXTENSION_ERROR_INIT_FAILED;
            }
        }

    }  // namespace Detail
}  // namespace Horo::PlatformServices
