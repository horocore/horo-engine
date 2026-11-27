#pragma once
/** @file NetworkProductComposition.h @brief Non-installed exact application factories and cold fixed-lifecycle composition. */
#include "Horo/Network/NetworkModeComposition.h"
#include "Horo/Runtime/RuntimeDispatchEvidence.h"
#include "Horo/Runtime/RuntimeLifecycle.h"

#include <functional>
#include <memory>
#include <optional>

namespace Horo::Application::Internal {
    using NetworkFixedParticipantFactory =
        std::function<Result<std::unique_ptr<Runtime::RuntimeLifecycleParticipant>>(Runtime::RuntimeDispatchSource)>;

    /** @brief Owner-retained polling after actual service and worker shutdown; no normal-frame wait. */
    using NetworkNativeRetirementPoll = std::function<Result<bool>(std::uint32_t)>;

    /** @brief Fixed-cardinality diagnostics for one completed best-effort owner tick. */
    struct HeadlessCrowdPressure final {
        std::uint64_t tick{};
        std::uint64_t maximumWaitTicks{};
        std::size_t requested{};
        std::size_t selected{};
        std::size_t groupsDeferred{};
        std::size_t deferred{};
        std::size_t incomplete{};
        std::size_t jobsAttempted{};
        std::size_t providerPressure{};
        bool queuePressure{};
        bool missedCutoff{};

        /** @brief Reports degraded optional work for the current fixed tick. */
        [[nodiscard]] bool IsSaturated() const noexcept {
            return deferred != 0 || incomplete != 0 || providerPressure != 0 || queuePressure || missedCutoff;
        }
    };

    /** @brief Bounded, mode-independent counters for one published crowd motion tick. */
    struct HeadlessCrowdMetrics final {
        std::uint64_t tick{};
        std::uint64_t solveNanoseconds{};
        std::size_t activeAgents{};
        std::size_t groups{};
        std::size_t neighborFacts{};
        std::uint64_t neighborsOmitted{};
        std::uint64_t boundariesOmitted{};
        std::size_t fallbacks{};
        std::size_t stalled{};
        std::size_t repaths{};
    };

    /** @brief Fixed-size observation of the current authority world with counters from its last committed decision tick. */
    struct HeadlessAiHealth final {
        std::uint64_t decisionTick{};
        std::size_t evaluated{};
        std::size_t workUnits{};
        std::size_t workersSubmitted{};
        std::size_t deferred{};
        std::size_t exhausted{};
        std::size_t failed{};
        std::size_t perceptionListeners{};
        bool degraded{};
        std::optional<HeadlessCrowdPressure> crowdPressure;
        std::optional<HeadlessCrowdMetrics> crowdMetrics;
    };

    using NetworkAiHealthSource = std::function<std::optional<HeadlessAiHealth>(Network::NetworkModeWorld, std::uint64_t)>;

    struct NetworkProductComposition final {
        Network::NetworkModeFactories factories;
        NetworkFixedParticipantFactory fixedParticipant;
        NetworkNativeRetirementPoll pollRetirement;
        NetworkAiHealthSource aiHealth;
    };
}  // namespace Horo::Application::Internal
