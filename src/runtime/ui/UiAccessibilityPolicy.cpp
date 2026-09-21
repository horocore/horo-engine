#include "UiAccessibilityPolicy.h"

namespace Horo::Runtime::Ui::AccessibilityInternal {
    bool IsKnownRole(const UiAccessibilityRole role) noexcept {
        switch (role) {
            case UiAccessibilityRole::Application:
            case UiAccessibilityRole::Window:
            case UiAccessibilityRole::Screen:
            case UiAccessibilityRole::Dialog:
            case UiAccessibilityRole::Alert:
            case UiAccessibilityRole::Group:
            case UiAccessibilityRole::Heading:
            case UiAccessibilityRole::StaticText:
            case UiAccessibilityRole::Button:
            case UiAccessibilityRole::Toggle:
            case UiAccessibilityRole::Checkbox:
            case UiAccessibilityRole::Radio:
            case UiAccessibilityRole::Slider:
            case UiAccessibilityRole::TextField:
            case UiAccessibilityRole::Link:
            case UiAccessibilityRole::Image:
            case UiAccessibilityRole::Progress:
            case UiAccessibilityRole::List:
            case UiAccessibilityRole::ListItem:
            case UiAccessibilityRole::Menu:
            case UiAccessibilityRole::MenuItem:
            case UiAccessibilityRole::Tab:
            case UiAccessibilityRole::TabItem:
            case UiAccessibilityRole::Tree:
            case UiAccessibilityRole::TreeItem:
            case UiAccessibilityRole::Table:
            case UiAccessibilityRole::Row:
            case UiAccessibilityRole::Cell:
            case UiAccessibilityRole::ScrollView:
                return true;
        }
        return false;
    }

    bool IsKnownSource(const UiAccessibilityControlSource source) noexcept {
        return source == UiAccessibilityControlSource::Core || source == UiAccessibilityControlSource::Contributed;
    }

    bool IsKnownTextSource(const UiAccessibilityTextSource source) noexcept {
        return source == UiAccessibilityTextSource::ResolvedMessage || source == UiAccessibilityTextSource::UserContent;
    }

    bool IsKnownExposure(const UiAccessibilityExposure exposure) noexcept {
        switch (exposure) {
            case UiAccessibilityExposure::Visible:
            case UiAccessibilityExposure::Offscreen:
            case UiAccessibilityExposure::Hidden:
            case UiAccessibilityExposure::Covered:
            case UiAccessibilityExposure::Suppressed:
            case UiAccessibilityExposure::Suspended:
                return true;
        }
        return false;
    }

    bool IsKnownValueKind(const UiAccessibilityValueKind kind) noexcept {
        switch (kind) {
            case UiAccessibilityValueKind::None:
            case UiAccessibilityValueKind::Boolean:
            case UiAccessibilityValueKind::Integer:
            case UiAccessibilityValueKind::Number:
            case UiAccessibilityValueKind::Text:
                return true;
        }
        return false;
    }

    bool IsKnownSelectionMode(const UiAccessibilitySelectionMode mode) noexcept {
        return mode == UiAccessibilitySelectionMode::None || mode == UiAccessibilitySelectionMode::Single ||
               mode == UiAccessibilitySelectionMode::Multiple;
    }

    bool IsKnownErrorKind(const UiAccessibilityErrorKind kind) noexcept {
        switch (kind) {
            case UiAccessibilityErrorKind::None:
            case UiAccessibilityErrorKind::Invalid:
            case UiAccessibilityErrorKind::Required:
            case UiAccessibilityErrorKind::Range:
            case UiAccessibilityErrorKind::Pattern:
            case UiAccessibilityErrorKind::Custom:
                return true;
        }
        return false;
    }

    bool IsKnownRelationKind(const UiAccessibilityRelationKind kind) noexcept {
        switch (kind) {
            case UiAccessibilityRelationKind::LabelledBy:
            case UiAccessibilityRelationKind::DescribedBy:
            case UiAccessibilityRelationKind::Controls:
            case UiAccessibilityRelationKind::Owns:
            case UiAccessibilityRelationKind::ActiveDescendant:
            case UiAccessibilityRelationKind::ErrorMessage:
            case UiAccessibilityRelationKind::FlowTo:
                return true;
        }
        return false;
    }

    bool IsKnownActionKind(const UiAccessibilityActionKind kind) noexcept {
        switch (kind) {
            case UiAccessibilityActionKind::Focus:
            case UiAccessibilityActionKind::Activate:
            case UiAccessibilityActionKind::Increment:
            case UiAccessibilityActionKind::Decrement:
            case UiAccessibilityActionKind::SetValue:
            case UiAccessibilityActionKind::SetText:
            case UiAccessibilityActionKind::ScrollForward:
            case UiAccessibilityActionKind::ScrollBackward:
            case UiAccessibilityActionKind::ScrollTo:
            case UiAccessibilityActionKind::Expand:
            case UiAccessibilityActionKind::Collapse:
            case UiAccessibilityActionKind::Select:
            case UiAccessibilityActionKind::ClearSelection:
            case UiAccessibilityActionKind::Dismiss:
                return true;
        }
        return false;
    }

    bool IsKnownActionValueKind(const UiAccessibilityActionValueKind kind) noexcept {
        switch (kind) {
            case UiAccessibilityActionValueKind::None:
            case UiAccessibilityActionValueKind::Boolean:
            case UiAccessibilityActionValueKind::Integer:
            case UiAccessibilityActionValueKind::Number:
            case UiAccessibilityActionValueKind::Text:
                return true;
        }
        return false;
    }

    bool RequiresName(const UiAccessibilityRole role) noexcept {
        switch (role) {
            case UiAccessibilityRole::Group:
            case UiAccessibilityRole::List:
            case UiAccessibilityRole::Menu:
            case UiAccessibilityRole::Tab:
            case UiAccessibilityRole::Tree:
            case UiAccessibilityRole::ScrollView:
                return false;
            default:
                return true;
        }
    }

    bool AllowsRange(const UiAccessibilityRole role) noexcept {
        return role == UiAccessibilityRole::Slider || role == UiAccessibilityRole::Progress;
    }

    bool AllowsSelection(const UiAccessibilityRole role) noexcept {
        switch (role) {
            case UiAccessibilityRole::Radio:
            case UiAccessibilityRole::ListItem:
            case UiAccessibilityRole::MenuItem:
            case UiAccessibilityRole::TabItem:
            case UiAccessibilityRole::TreeItem:
            case UiAccessibilityRole::Row:
            case UiAccessibilityRole::Cell:
                return true;
            default:
                return false;
        }
    }

    bool AllowsExpanded(const UiAccessibilityRole role) noexcept {
        switch (role) {
            case UiAccessibilityRole::Group:
            case UiAccessibilityRole::List:
            case UiAccessibilityRole::Menu:
            case UiAccessibilityRole::Tab:
            case UiAccessibilityRole::Tree:
            case UiAccessibilityRole::TreeItem:
            case UiAccessibilityRole::Row:
            case UiAccessibilityRole::Cell:
            case UiAccessibilityRole::ScrollView:
                return true;
            default:
                return false;
        }
    }

    bool AllowsScroll(const UiAccessibilityRole role) noexcept {
        return role == UiAccessibilityRole::ScrollView || role == UiAccessibilityRole::List || role == UiAccessibilityRole::Tree ||
               role == UiAccessibilityRole::Table;
    }

    bool AllowsActivate(const UiAccessibilityRole role) noexcept {
        switch (role) {
            case UiAccessibilityRole::Button:
            case UiAccessibilityRole::Toggle:
            case UiAccessibilityRole::Checkbox:
            case UiAccessibilityRole::Radio:
            case UiAccessibilityRole::Link:
            case UiAccessibilityRole::ListItem:
            case UiAccessibilityRole::MenuItem:
            case UiAccessibilityRole::TabItem:
                return true;
            default:
                return false;
        }
    }

    bool AllowsDismiss(const UiAccessibilityRole role) noexcept {
        return role == UiAccessibilityRole::Window || role == UiAccessibilityRole::Screen || role == UiAccessibilityRole::Dialog ||
               role == UiAccessibilityRole::Alert;
    }

    bool AllowsChecked(const UiAccessibilityRole role) noexcept {
        return role == UiAccessibilityRole::Toggle || role == UiAccessibilityRole::Checkbox || role == UiAccessibilityRole::Radio;
    }

    bool AllowsPressed(const UiAccessibilityRole role) noexcept {
        return role == UiAccessibilityRole::Button || role == UiAccessibilityRole::Toggle;
    }

    bool AllowsSelected(const UiAccessibilityRole role) noexcept {
        return AllowsSelection(role);
    }

    bool AllowsValue(const UiAccessibilityRole role) noexcept {
        return role == UiAccessibilityRole::Toggle || role == UiAccessibilityRole::Checkbox || role == UiAccessibilityRole::Radio ||
               role == UiAccessibilityRole::Slider || role == UiAccessibilityRole::TextField || role == UiAccessibilityRole::Progress;
    }

    bool AllowsReadOnly(const UiAccessibilityRole role) noexcept {
        return AllowsValue(role);
    }

    bool AllowsRequired(const UiAccessibilityRole role) noexcept {
        return role == UiAccessibilityRole::TextField || role == UiAccessibilityRole::Slider;
    }

    bool AllowsMultiSelectable(const UiAccessibilityRole role) noexcept {
        return role == UiAccessibilityRole::List || role == UiAccessibilityRole::Tree || role == UiAccessibilityRole::Table;
    }

    bool AllowsPopup(const UiAccessibilityRole role) noexcept {
        return role == UiAccessibilityRole::Button || role == UiAccessibilityRole::Toggle || role == UiAccessibilityRole::MenuItem;
    }

    bool IsValueCompatible(const UiAccessibilityRole role, const UiAccessibilityValueKind kind) noexcept {
        if (kind == UiAccessibilityValueKind::None)
            return true;
        switch (role) {
            case UiAccessibilityRole::Toggle:
            case UiAccessibilityRole::Checkbox:
            case UiAccessibilityRole::Radio:
                return kind == UiAccessibilityValueKind::Boolean;
            case UiAccessibilityRole::Slider:
            case UiAccessibilityRole::Progress:
                return kind == UiAccessibilityValueKind::Integer || kind == UiAccessibilityValueKind::Number;
            case UiAccessibilityRole::TextField:
                return kind == UiAccessibilityValueKind::Text;
            default:
                return false;
        }
    }
}  // namespace Horo::Runtime::Ui::AccessibilityInternal
