#pragma once
/** @file NetworkProductComposition.h @brief Non-installed exact application factories and cold fixed-lifecycle composition. */
#include "Horo/Network/NetworkModeComposition.h"
#include "Horo/Runtime/RuntimeDispatchEvidence.h"
#include "Horo/Runtime/RuntimeLifecycle.h"

#include <functional>
#include <memory>

namespace Horo::Application::Internal {
    using NetworkFixedParticipantFactory =
        std::function<Result<std::unique_ptr<Runtime::RuntimeLifecycleParticipant>>(Runtime::RuntimeDispatchSource)>;

    struct NetworkProductComposition final {
        Network::NetworkModeFactories factories;
        NetworkFixedParticipantFactory fixedParticipant;
    };
}  // namespace Horo::Application::Internal
