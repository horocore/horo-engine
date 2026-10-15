#pragma once
#include "Horo/AI/BlackboardSchema.h"
#include "Horo/Extensions/ScriptValue.h"

namespace Horo::Gameplay::Detail {
    /** @brief Projects the complete bounded blackboard value algebra into owned typed script fields. */
    [[nodiscard]] Extensions::ScriptValue ToScriptBlackboardValue(const AI::BlackboardValue &value);
    /** @brief Reconstructs logical fields without native ABI bytes, padding, pointers, or lossy numbers. */
    [[nodiscard]] Result<AI::BlackboardValue> FromScriptBlackboardValue(const Extensions::ScriptValue &value);
}  // namespace Horo::Gameplay::Detail
