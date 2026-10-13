#include "UiTemplateParameterValidation.h"

#include "Horo/UiTemplates/UiTemplateErrors.h"

#include <cmath>

namespace Horo::UiTemplates::Detail {
    namespace {
        /** @brief Explicitly checks optional presence before finiteness; an authored zero remains an engaged bound. */
        bool FiniteBound(const std::optional<double> bound) {
            return !bound.has_value() || std::isfinite(*bound);
        }

        /** @brief Checks the admitted numeric range without changing integer-versus-number value authority. */
        bool WithinBounds(const UiTemplateParameter &parameter, const long double number) {
            return (!parameter.minimum.has_value() || number >= *parameter.minimum) &&
                   (!parameter.maximum.has_value() || number <= *parameter.maximum);
        }

        /** @brief Validates finite numeric alternatives after declared-category and text-bound admission. */
        Result<void> ValidateNumber(const UiTemplateParameter &parameter, const UiTemplateValue &value) {
            std::optional<long double> number;
            if (const auto *integer = std::get_if<std::int64_t>(&value))
                number = *integer;
            if (const auto *scalar = std::get_if<double>(&value)) {
                if (!std::isfinite(*scalar))
                    return Result<void>::Failure(MakeError(UiErrors::TemplateGraphInvalid, "Template number is non-finite."));
                number = *scalar;
            }
            if (number.has_value() && !WithinBounds(parameter, *number))
                return Result<void>::Failure(MakeError(UiErrors::TemplateGraphInvalid, "Template value exceeds its numeric constraint."));
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc ValidateParameterConstraints */
    Result<void> ValidateParameterConstraints(const UiTemplateParameter &parameter) {
        const bool numeric = parameter.kind == UiTemplateValueKind::Integer || parameter.kind == UiTemplateValueKind::Number;
        const bool finite = FiniteBound(parameter.minimum) && FiniteBound(parameter.maximum);
        if (const bool ordered =
                !parameter.minimum.has_value() || !parameter.maximum.has_value() || *parameter.minimum <= *parameter.maximum;
            !finite || !ordered || ((!numeric) && (parameter.minimum.has_value() || parameter.maximum.has_value())))
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
        return ValidateNumber(parameter, value);
    }
}  // namespace Horo::UiTemplates::Detail
