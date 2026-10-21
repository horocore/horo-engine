#include "Horo/Runtime/Ui/UiElementCatalog.h"

#include <algorithm>
#include <type_traits>

namespace Horo::Runtime::Ui {
    namespace {
        /** @brief Initialize a real detached control descriptor without changing any active owner. */
        Result<void> ApplyInitial(UiControlDescriptor &descriptor, const UiElementAuthoredState &state) {
            return std::visit([&](auto &control) -> Result<void> {
                using T = std::decay_t<decltype(control)>;
                control.base.initiallyEnabled = state.enabled && state.visible;
                control.base.focusable = state.focusable && state.visible;
                if constexpr (std::is_same_v<T, UiToggleControlDescriptor>) {
                    if (const auto *value = std::get_if<bool>(&state.initial))
                        control.initiallyChecked = *value;
                }
                if constexpr (std::is_same_v<T, UiSliderControlDescriptor>) {
                    if (const auto *value = std::get_if<UiControlScalarValues>(&state.initial)) {
                        control.minimum = value->minimum;
                        control.maximum = value->maximum;
                        control.step = value->step;
                        control.initialValue = value->value;
                    }
                }
                if constexpr (std::is_same_v<T, UiTextInputControlDescriptor>) {
                    if (const auto *value = std::get_if<std::string>(&state.initial)) {
                        auto text = UiActionText::Create(*value);
                        if (text.HasError())
                            return Result<void>::Failure(text.ErrorValue());
                        control.initialText = text.Value();
                    }
                }
                return Result<void>::Success();
            }, descriptor);
        }

        /** @brief Require the existing runtime control variant to agree with the catalog's authored kind. */
        bool MatchesControl(const UiControlKind control, const UiElementKind element) noexcept {
            switch (control) {
                case UiControlKind::Button:
                    return element == UiElementKind::Button;
                case UiControlKind::Toggle:
                    return element == UiElementKind::Toggle;
                case UiControlKind::Slider:
                    return element == UiElementKind::Slider;
                case UiControlKind::TextInput:
                    return element == UiElementKind::TextInput;
                case UiControlKind::Count:
                    return false;
            }
            return false;
        }
    }  // namespace

    /** @copydoc ProjectUiElementControl */
    Result<UiControlDescriptor> ProjectUiElementControl(const UiDocument &document, const UiElementId id, const UiElementTree &tree,
                                                        UiControlDescriptor descriptor) {
        using R = Result<UiControlDescriptor>;
        if (tree.State() != UiElementTreeState::Active || tree.SourceDocument() != document.Id() ||
            tree.SourceDocumentRevision() != document.Revision())
            return R::Failure(MakeError(UiErrors::RevisionStale));
        const auto found = std::ranges::find(document.Elements(), id, &UiDocumentElement::id);
        if (found == document.Elements().end())
            return R::Failure(MakeError(UiErrors::ControlDescriptorInvalid));
        const auto &element = *found;
        const auto source = std::visit([](const auto &control) {
            return UiActionSource{control.base.owner, control.base.element};
        }, descriptor);
        const auto record = tree.Get(source.element);
        if (record.HasError())
            return R::Failure(record.ErrorValue());
        if (record.Value().id != id || source.owner.instance != tree.Instance() || source.owner.canvas != tree.Canvas() ||
            source.owner.document != document.Id() || source.owner.documentRevision != document.Revision() ||
            source.owner.treeRevision != tree.Revision())
            return R::Failure(MakeError(UiErrors::RevisionStale));
        if (ValidateCoreUiElement(element).HasError())
            return R::Failure(MakeError(UiErrors::ControlDescriptorInvalid));
        const auto *type = UiElementCatalog::Find(element.type.Value());
        const auto kind = UiControlKindOf(descriptor);
        if (!type || !MatchesControl(kind, type->kind))
            return R::Failure(MakeError(UiErrors::ControlDescriptorInvalid));
        if (element.presentation) {
            const auto applied = ApplyInitial(descriptor, element.presentation->state);
            if (applied.HasError())
                return R::Failure(applied.ErrorValue());
        }
        const auto valid = ValidateUiControlDescriptor(descriptor);
        return valid.HasError() ? R::Failure(valid.ErrorValue()) : R::Success(std::move(descriptor));
    }

    /** @copydoc ProjectUiElementAccessibility */
    Result<UiAccessibilityNodeInput> ProjectUiElementAccessibility(const UiDocumentElement &element, UiAccessibilityNodeInput node) {
        using R = Result<UiAccessibilityNodeInput>;
        if (node.element != element.id || ValidateCoreUiElement(element).HasError())
            return R::Failure(MakeError(UiErrors::AccessibilitySchemaInvalid));
        if (element.presentation) {
            const auto &presentation = *element.presentation;
            if (!presentation.state.visible)
                node.exposure = UiAccessibilityExposure::Hidden;
            if (!presentation.state.enabled)
                node.state.flags |= static_cast<UiAccessibilityStateMask>(UiAccessibilityStateFlag::Disabled);
            if (!presentation.state.focusable || !presentation.state.visible)
                node.state.flags &= ~(static_cast<UiAccessibilityStateMask>(UiAccessibilityStateFlag::Focusable) |
                                      static_cast<UiAccessibilityStateMask>(UiAccessibilityStateFlag::Focused));
            if (presentation.accessibility) {
                const auto &access = *presentation.accessibility;
                node.role = access.role;
                if (access.hidden) {
                    node.exposure = UiAccessibilityExposure::Hidden;
                    node.state.flags &= ~(static_cast<UiAccessibilityStateMask>(UiAccessibilityStateFlag::Focusable) |
                                          static_cast<UiAccessibilityStateMask>(UiAccessibilityStateFlag::Focused));
                }
                if (access.readOnly)
                    node.state.flags |= static_cast<UiAccessibilityStateMask>(UiAccessibilityStateFlag::ReadOnly);
                if (access.required)
                    node.state.flags |= static_cast<UiAccessibilityStateMask>(UiAccessibilityStateFlag::Required);
            }
        }
        auto valid = ValidateUiAccessibilityNodeInput(node);
        return valid.HasError() ? R::Failure(valid.ErrorValue()) : R::Success(node);
    }

    /** @copydoc UiElementLocalizedText */
    const UiLocalizedText *UiElementLocalizedText(const UiDocumentElement &element) noexcept {
        const auto *type = UiElementCatalog::Find(element.type.Value());
        return type && type->kind == UiElementKind::Text && element.presentation && element.presentation->localizedText
                   ? &*element.presentation->localizedText
                   : nullptr;
    }

    /** @copydoc DefaultUiElementAccessibilityRole */
    UiAccessibilityRole DefaultUiElementAccessibilityRole(const UiElementKind kind) noexcept {
        switch (kind) {
            case UiElementKind::Canvas:
                return UiAccessibilityRole::Application;
            case UiElementKind::Screen:
                return UiAccessibilityRole::Screen;
            case UiElementKind::Text:
                return UiAccessibilityRole::StaticText;
            case UiElementKind::Image:
                return UiAccessibilityRole::Image;
            case UiElementKind::Button:
                return UiAccessibilityRole::Button;
            case UiElementKind::Toggle:
                return UiAccessibilityRole::Toggle;
            case UiElementKind::Slider:
                return UiAccessibilityRole::Slider;
            case UiElementKind::TextInput:
                return UiAccessibilityRole::TextField;
            case UiElementKind::ProgressBar:
                return UiAccessibilityRole::Progress;
            case UiElementKind::ScrollView:
                return UiAccessibilityRole::ScrollView;
            case UiElementKind::CollectionView:
                return UiAccessibilityRole::List;
            default:
                return UiAccessibilityRole::Group;
        }
    }
}  // namespace Horo::Runtime::Ui
