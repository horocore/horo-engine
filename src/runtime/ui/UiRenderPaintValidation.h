#pragma once

#include "Horo/Runtime/Ui/UiRenderSnapshot.h"

namespace Horo::Runtime::Ui::PaintInternal {
    /** @brief Validates bounded canonical mask ancestry and isolated paint intervals without allocating. */
    [[nodiscard]] Result<void> ValidateEffects(const UiElementTree &tree, const UiRenderProjection &projection);
}  // namespace Horo::Runtime::Ui::PaintInternal
