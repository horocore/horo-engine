#include "Horo/Runtime/Ui/UiElementPresentation.h"

#include "Horo/Foundation/Utf8.h"
#include "UiAccessibilityPolicy.h"
#include "UiStyleInternal.h"

namespace Horo::Runtime::Ui {
    /** @copydoc UiElementAuthoredState::IsValid */
    bool UiElementAuthoredState::IsValid() const noexcept {
        if (const auto *scalar = std::get_if<UiControlScalarValues>(&initial))
            return scalar->IsValid();
        if (const auto *text = std::get_if<std::string>(&initial))
            return UiActionText::Create(*text).HasValue();
        return !initial.valueless_by_exception();
    }

    /** @copydoc UiElementAuthoredState::InitialVisualState */
    UiVisualStateMask UiElementAuthoredState::InitialVisualState() const noexcept {
        UiVisualStateMask result;
        if (!enabled || !visible)
            result = result | UiVisualState::Disabled;
        if (const auto *checked = std::get_if<bool>(&initial); checked && *checked)
            result = result | UiVisualState::Checked;
        return result;
    }

    /** @copydoc UiElementAccessibility::IsValid */
    bool UiElementAccessibility::IsValid() const noexcept {
        using namespace AccessibilityInternal;
        return IsKnownRole(role) && (!name || name->IsValid()) && (!description || description->IsValid()) &&
               (!RequiresName(role) || name.has_value()) && (!readOnly || AllowsReadOnly(role)) && (!required || AllowsRequired(role));
    }

    /** @copydoc UiElementPresentation::UsesAuthoringExtensions */
    bool UiElementPresentation::UsesAuthoringExtensions() const noexcept {
        return state != UiElementAuthoredState{} || localizedText.has_value() || accessibility.has_value();
    }

    /** @copydoc UiElementPresentation::IsValid */
    bool UiElementPresentation::IsValid() const noexcept {
        if (!state.IsValid() || (localizedText && !localizedText->IsValid()) || (accessibility && !accessibility->IsValid()) ||
            !layout.IsValid() || !intrinsic.IsValid() || classes.size() > MaximumUiStyleClasses ||
            inlineStyle.size() > MaximumUiStyleProperties ||
            (!typeClass.IsValid() && (typeClass.asset.IsValid() || typeClass.id.IsValid())))
            return false;
        for (std::size_t index = 0; index < classes.size(); ++index) {
            if (!classes[index].IsValid())
                return false;
            for (std::size_t previous = 0; previous < index; ++previous)
                if (classes[previous] == classes[index])
                    return false;
        }
        for (std::size_t index = 0; index < inlineStyle.size(); ++index) {
            const auto &assignment = inlineStyle[index];
            if (!assignment.property.IsValid() || !assignment.value.IsValid() || assignment.sealed)
                return false;
            for (std::size_t previous = 0; previous < index; ++previous)
                if (inlineStyle[previous].property == assignment.property)
                    return false;
        }
        return true;
    }

    /** @copydoc ValidateUiElementPresentationStyles */
    Result<void> ValidateUiElementPresentationStyles(const RuntimeStyleRegistry &registry, const UiElementPresentation &presentation) {
        if (registry.State() != RuntimeStyleRegistryState::Active)
            return Result<void>::Failure(MakeError(UiErrors::StyleLifecycleUnavailable));
        if (!presentation.IsValid())
            return Result<void>::Failure(MakeError(UiErrors::StyleInvalid));
        // ResolveElement is the same pure cascade used by the runtime resolver; it never reads input.element.
        const UiStyleElementInput input{.asset = presentation.styleAsset,
                                        .typeClass = presentation.typeClass,
                                        .classes = presentation.classes,
                                        .inlineProperties = presentation.inlineStyle,
                                        .state = presentation.state.InitialVisualState()};
        const auto resolved = StyleInternal::ResolveElement(registry, input, nullptr, MaximumUiStyleProperties);
        return resolved.HasError() ? Result<void>::Failure(resolved.ErrorValue()) : Result<void>::Success();
    }
}  // namespace Horo::Runtime::Ui
