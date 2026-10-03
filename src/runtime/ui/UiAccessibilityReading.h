#pragma once

#include "UiAccessibilityStorage.h"

namespace Horo::Runtime::Ui::AccessibilityInternal {
    /** @brief Rejects oversized spans before duplicate, relation and text validation.
     * @param projection Borrowed candidate. @param limits Validated fixed budgets.
     * @return Success or CapacityExceeded without modifying the candidate.
     */
    [[nodiscard]] Result<void> ValidateInputBounds(const UiAccessibilityProjection &projection, const UiAccessibilityLimits &limits);
    /** @brief Builds canonical reading records from exact retained-tree residency and exposure evidence.
     * @param tree Active source tree. @param projection Validated candidate. @param lookup Candidate identity index.
     * @param preorder Reserved tree-sized scratch. @param reading Reserved node-sized output.
     * @return Success or typed lifecycle/focus failure; output is scratch until success.
     */
    [[nodiscard]] Result<void> BuildReadingProjection(const UiElementTree &tree, const UiAccessibilityProjection &projection,
                                                      ProjectionLookup lookup, std::vector<UiElementHandle> &preorder,
                                                      std::vector<UiAccessibilityNodeInput> &reading);
}  // namespace Horo::Runtime::Ui::AccessibilityInternal
