#pragma once
#include "Horo/Runtime/Ui/UiElementPresentation.h"

#include <nlohmann/json_fwd.hpp>

namespace Horo::Runtime::Ui::PresentationCodec {
    /** @brief Append canonical authored fields to a version-2 presentation object. */
    void WriteAuthoring(nlohmann::ordered_json &output, const UiElementPresentation &presentation);
    /** @brief Decode the bounded authoring fields using existing Localization validation. */
    [[nodiscard]] Result<void> ReadAuthoring(const nlohmann::json &input, UiElementPresentation &presentation);
}  // namespace Horo::Runtime::Ui::PresentationCodec
