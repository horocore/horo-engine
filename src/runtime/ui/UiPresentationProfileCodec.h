#pragma once
#include "Horo/Runtime/Ui/UiPresentationProfiles.h"

#include <nlohmann/json_fwd.hpp>

namespace Horo::Runtime::Ui::ProfileCodec {
    /** @brief Encodes canonical durable alternatives; no runtime evidence is serialized. */
    [[nodiscard]] nlohmann::ordered_json Encode(std::span<const UiDocumentPresentationProfiles> tables);
    /** @brief Decodes bounded inert alternatives for subsequent exact document membership validation. */
    [[nodiscard]] Result<std::vector<UiDocumentPresentationProfiles>> Decode(const nlohmann::json &value);
}  // namespace Horo::Runtime::Ui::ProfileCodec
