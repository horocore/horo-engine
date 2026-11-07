#pragma once
#include "Horo/AI/StateMachine.h"

namespace Horo::AI::Detail {
    [[nodiscard]] Result<void> ValidateStateSourceReachability(const StateMachineAssetDescriptor &source);
}
