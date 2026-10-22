#pragma once
/** @file MultiviewExtractionErrors.h
 * @brief Typed multiview admission, culling and publication failures.
 */
#include "Horo/Foundation/ErrorCode.h"

namespace Horo::Render::MultiviewExtractionErrors {
    extern const ErrorCodeDescriptor InvalidInput;
    extern const ErrorCodeDescriptor StalePublication;
    extern const ErrorCodeDescriptor CapacityExceeded;
    extern const ErrorCodeDescriptor Unsupported;
}  // namespace Horo::Render::MultiviewExtractionErrors
