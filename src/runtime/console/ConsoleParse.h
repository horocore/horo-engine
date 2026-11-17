#pragma once

#include "Horo/Debug/CommandRegistry.h"

namespace Horo::Debug::Detail {
    /** @brief Shared command/variable lexical rules; bounds are checked before input is copied. */
    [[nodiscard]] Result<std::vector<std::string>> TokenizeConsole(std::string_view input, const ConsoleLimits &limits);
    /** @brief Shared complete-token schema conversion without locale-dependent or partial parsing. */
    [[nodiscard]] Result<ArgumentValue> ConvertConsoleArgument(const DebugArgumentDescriptor &argument, std::string_view text);
}  // namespace Horo::Debug::Detail
