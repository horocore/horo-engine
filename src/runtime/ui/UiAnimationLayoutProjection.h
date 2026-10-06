#pragma once

/** @file UiAnimationLayoutProjection.h
 * @brief Private projection of authoritative computed animation styles into preallocated actual layout descriptors.
 */
#include "Horo/Runtime/Ui/UiAnimationTracks.h"
#include "Horo/Runtime/Ui/UiLayout.h"

namespace Horo::Runtime::Ui::AnimationInternal {
    /** @brief Actual-tree-bound load-time binding; created only after owner admission verifies target/property/field uniqueness. */
    struct LayoutBinding final {
        UiElementHandle element;
        UiStylePropertyId property;
        UiAnimationLayoutField field;
    };

    /** @brief Projects immutable computed values into caller-preallocated inactive layout descriptors; no publication occurs. */
    [[nodiscard]] Result<void> ProjectLayout(const UiComputedStyleSnapshot &styles, std::span<const LayoutBinding> bindings,
                                             std::span<UiLayoutElementDescriptor> descriptors);
}  // namespace Horo::Runtime::Ui::AnimationInternal
