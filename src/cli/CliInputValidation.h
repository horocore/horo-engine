#pragma once

#include "Horo/Cli/CliOptionParser.h"

namespace Horo::Cli::Detail {
    /** @brief Captures selected stdin within the command's admitted byte and grammar limits. */
    [[nodiscard]] Result<std::optional<CliParsedInput>> ReadInput(const CliCommandDescriptor &command, const CliParserContext &context,
                                                                  const CliParserLimits &limits);
}  // namespace Horo::Cli::Detail
