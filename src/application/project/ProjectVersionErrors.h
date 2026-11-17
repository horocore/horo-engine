#pragma once

/** @file ProjectVersionErrors.h
 * @brief Narrow canonical version/hash diagnostics shared by the sole version
 * parser and project composition.
 */
#include "Horo/Foundation/ErrorCode.h"

namespace Horo::Application::ProjectErrors {
    extern const ErrorCodeDescriptor VersionInvalid;
    extern const ErrorCodeDescriptor HashInvalid;
}  // namespace Horo::Application::ProjectErrors
