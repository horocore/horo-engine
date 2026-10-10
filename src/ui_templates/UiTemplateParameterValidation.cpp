#include "UiTemplateParameterValidation.h"

#include "Horo/UiTemplates/UiTemplateErrors.h"

#include <cmath>

namespace Horo::UiTemplates::Detail {
    /** @copydoc ValidateParameterConstraints */
    Result<void> ValidateParameterConstraints(const UiTemplateParameter &parameter) {
        const bool numeric = parameter.kind == UiTemplateValueKind::Integer || parameter.kind == UiTemplateValueKind::Number;
        const bool finite =
            (!parameter.minimum || std::isfinite(*parameter.minimum)) && (!parameter.maximum || std::isfinite(*parameter.maximum));
        const bool ordered = !parameter.minimum || !parameter.maximum || *parameter.minimum <= *parameter.maximum;
        if (!finite || !ordered || ((!numeric) && (parameter.minimum || parameter.maximum)))
            return Result<void>::Failure(MakeError(UiErrors::TemplateGraphInvalid, "Template parameter constraints are invalid."));
        return Result<void>::Success();
    }

    /** @copydoc ValidateParameterValue */
    Result<void> ValidateParameterValue(const UiTemplateParameter &parameter, const UiTemplateValue &value) {
        if (const auto valid = ValidateParameterConstraints(parameter); valid.HasError())
            return valid;
        if (value.valueless_by_exception() || TemplateValueKind(value) != parameter.kind)
            return Result<void>::Failure(MakeError(UiErrors::TemplateGraphInvalid, "Template value differs from its declared category."));
        if (const auto *text = std::get_if<std::string>(&value); text && text->size() > parameter.maximumTextBytes)
            return Result<void>::Failure(MakeError(UiErrors::TemplateGraphInvalid, "Template value exceeds its text constraint."));
        std::optional<long double> number;
        if (const auto *integer = std::get_if<std::int64_t>(&value))
            number = *integer;
        if (const auto *scalar = std::get_if<double>(&value)) {
            if (!std::isfinite(*scalar))
                return Result<void>::Failure(MakeError(UiErrors::TemplateGraphInvalid, "Template number is non-finite."));
            number = *scalar;
        }
        if (number && ((parameter.minimum && *number < *parameter.minimum) || (parameter.maximum && *number > *parameter.maximum)))
            return Result<void>::Failure(MakeError(UiErrors::TemplateGraphInvalid, "Template value exceeds its numeric constraint."));
        return Result<void>::Success();
    }
}  // namespace Horo::UiTemplates::Detail
