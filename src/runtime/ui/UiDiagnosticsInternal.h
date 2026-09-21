#pragma once

#include "Horo/Runtime/Ui/UiDiagnostics.h"

namespace Horo::Runtime::Ui::Detail {
    /** @brief Returns the immutable canonical Runtime UI diagnostic descriptor table. */
    [[nodiscard]] std::span<const ErrorCodeDescriptor *const> DiagnosticDescriptors() noexcept;
}  // namespace Horo::Runtime::Ui::Detail
