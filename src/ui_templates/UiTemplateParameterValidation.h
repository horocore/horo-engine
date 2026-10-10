#pragma once

#include "Horo/UiTemplates/UiTemplateModel.h"

namespace Horo::UiTemplates::Detail {
    /** @brief Checks one literal against its stable parameter's closed category and finite constraints. */
    [[nodiscard]] Result<void> ValidateParameterValue(const UiTemplateParameter &parameter, const UiTemplateValue &value);
    /** @brief Checks a declaration's finite numeric bounds independently of whether a value was supplied. */
    [[nodiscard]] Result<void> ValidateParameterConstraints(const UiTemplateParameter &parameter);
}  // namespace Horo::UiTemplates::Detail
