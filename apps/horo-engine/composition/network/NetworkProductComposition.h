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
    };

    using NetworkAiHealthSource = std::function<std::optional<HeadlessAiHealth>(Network::NetworkModeWorld, std::uint64_t)>;

    struct NetworkProductComposition final {
        Network::NetworkModeFactories factories;
        NetworkFixedParticipantFactory fixedParticipant;
        NetworkNativeRetirementPoll pollRetirement;
        NetworkAiHealthSource aiHealth;
    };
}  // namespace Horo::Application::Internal
