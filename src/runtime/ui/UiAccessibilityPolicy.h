#pragma once

#include "Horo/Runtime/Ui/UiAccessibility.h"

namespace Horo::Runtime::Ui::AccessibilityInternal {
    [[nodiscard]] bool IsKnownRole(UiAccessibilityRole role) noexcept;
    [[nodiscard]] bool IsKnownSource(UiAccessibilityControlSource source) noexcept;
    [[nodiscard]] bool IsKnownTextSource(UiAccessibilityTextSource source) noexcept;
    [[nodiscard]] bool IsKnownExposure(UiAccessibilityExposure exposure) noexcept;
    [[nodiscard]] bool IsKnownValueKind(UiAccessibilityValueKind kind) noexcept;
    [[nodiscard]] bool IsKnownSelectionMode(UiAccessibilitySelectionMode mode) noexcept;
    [[nodiscard]] bool IsKnownErrorKind(UiAccessibilityErrorKind kind) noexcept;
    [[nodiscard]] bool IsKnownRelationKind(UiAccessibilityRelationKind kind) noexcept;
    [[nodiscard]] bool IsKnownActionKind(UiAccessibilityActionKind kind) noexcept;
    [[nodiscard]] bool IsKnownActionValueKind(UiAccessibilityActionValueKind kind) noexcept;
    [[nodiscard]] bool RequiresName(UiAccessibilityRole role) noexcept;
    [[nodiscard]] bool AllowsRange(UiAccessibilityRole role) noexcept;
    [[nodiscard]] bool AllowsSelection(UiAccessibilityRole role) noexcept;
    [[nodiscard]] bool AllowsExpanded(UiAccessibilityRole role) noexcept;
    [[nodiscard]] bool AllowsScroll(UiAccessibilityRole role) noexcept;
    [[nodiscard]] bool AllowsActivate(UiAccessibilityRole role) noexcept;
    [[nodiscard]] bool AllowsDismiss(UiAccessibilityRole role) noexcept;
    [[nodiscard]] bool AllowsChecked(UiAccessibilityRole role) noexcept;
    [[nodiscard]] bool AllowsPressed(UiAccessibilityRole role) noexcept;
    [[nodiscard]] bool AllowsSelected(UiAccessibilityRole role) noexcept;
    [[nodiscard]] bool AllowsValue(UiAccessibilityRole role) noexcept;
    [[nodiscard]] bool AllowsReadOnly(UiAccessibilityRole role) noexcept;
    [[nodiscard]] bool AllowsRequired(UiAccessibilityRole role) noexcept;
    [[nodiscard]] bool AllowsMultiSelectable(UiAccessibilityRole role) noexcept;
    [[nodiscard]] bool AllowsPopup(UiAccessibilityRole role) noexcept;
    [[nodiscard]] bool IsValueCompatible(UiAccessibilityRole role, UiAccessibilityValueKind kind) noexcept;
}  // namespace Horo::Runtime::Ui::AccessibilityInternal
