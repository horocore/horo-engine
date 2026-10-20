#include "Horo/Runtime/Ui/UiElementPresentation.h"

namespace Horo::Runtime::Ui {
    /** @copydoc UiElementPresentation::IsValid */
    bool UiElementPresentation::IsValid() const noexcept {
        if (!layout.IsValid() || !intrinsic.IsValid() || classes.size() > MaximumUiStyleClasses ||
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
}  // namespace Horo::Runtime::Ui
