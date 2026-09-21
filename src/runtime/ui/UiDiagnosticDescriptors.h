#pragma once

#include "Horo/Runtime/Ui/UiErrors.h"

#include <span>

namespace Horo::Runtime::Ui::DiagnosticsInternal {
    [[nodiscard]] std::span<const ErrorCodeDescriptor *const> ErrorDescriptors() noexcept;
}  // namespace Horo::Runtime::Ui::DiagnosticsInternal
