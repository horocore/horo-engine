#pragma once

#include "Horo/Runtime/Ui/UiElementPresentation.h"

#include <nlohmann/json_fwd.hpp>
#include <string>
#include <string_view>

namespace Horo::Runtime::Ui::PresentationCodec {
    /** @brief Encodes a locally validated presentation with the same canonical codec used by source and cook. */
    [[nodiscard]] nlohmann::ordered_json Encode(const UiElementPresentation &presentation);
    /** @brief Decodes bounded local values; host style references are resolved later by RuntimeStyleRegistry. */
    [[nodiscard]] Result<UiElementPresentation> Decode(const nlohmann::json &value);
    /** @brief Encodes a bounded presentation payload for cooked format 3. */
    [[nodiscard]] Result<std::string> Write(const UiElementPresentation &presentation);
    /** @brief Parses one bounded canonical presentation payload from cooked format 3. */
    [[nodiscard]] Result<UiElementPresentation> Read(std::string_view source);
}  // namespace Horo::Runtime::Ui::PresentationCodec
