#pragma once
#include "Horo/AI/BlackboardSchema.h"
#include "Horo/Extensions/ScriptValue.h"

namespace Horo::Gameplay::Detail {
    inline constexpr std::string_view GameplayAiNativeErrorId = "ai.native-error";

    /** @brief Explicit script identity for one native AI error; native descriptors remain authoritative. */
    struct GameplayAiScriptErrorIdentity final {
        const ErrorCodeDescriptor *native;
        std::string_view scriptId;
    };

    /** @brief Returns the closed script-boundary mapping used by descriptor admission and failure transport. */
    [[nodiscard]] std::span<const GameplayAiScriptErrorIdentity> GameplayAiScriptErrorIdentities() noexcept;

    /** @brief Projects the complete bounded blackboard value algebra into owned typed script fields. */
    [[nodiscard]] Extensions::ScriptValue ToScriptBlackboardValue(const AI::BlackboardValue &value);
    /** @brief Reconstructs logical fields without native ABI bytes, padding, pointers, or lossy numbers. */
    [[nodiscard]] Result<AI::BlackboardValue> FromScriptBlackboardValue(const Extensions::ScriptValue &value);
}  // namespace Horo::Gameplay::Detail
