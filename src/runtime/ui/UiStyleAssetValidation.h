#pragma once
#include "Horo/Runtime/Ui/UiStyleAsset.h"

namespace Horo::Runtime::Ui::StyleAssetInternal {
    /** @brief Verify every direct resource/foreign namespace reference against explicit source dependencies and actual host evidence. */
    [[nodiscard]] Result<void> ValidateReferences(const UiStyleAssetDocument &document, const UiStyleSchema &schema);
}  // namespace Horo::Runtime::Ui::StyleAssetInternal
