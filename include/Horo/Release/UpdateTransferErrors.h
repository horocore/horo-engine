#pragma once

/**
 * @file UpdateTransferErrors.h
 * @brief Typed capacity, response, and continuity failures for update downloads.
 */

#include "Horo/Foundation/ErrorCode.h"

namespace Horo::Release::UpdateTransferErrors {
    /** @brief Package size exceeds policy or private-storage capacity. */
    extern const ErrorCodeDescriptor InsufficientSpace;
    /** @brief Response status, URL, length, or range contradicts the selected package. */
    extern const ErrorCodeDescriptor InvalidResponse;
    /** @brief A partial transfer cannot be continued under its original source and validator. */
    extern const ErrorCodeDescriptor ResumeMismatch;
    /** @brief Persisted checkpoint bytes are malformed, oversized, or from an unsupported schema. */
    extern const ErrorCodeDescriptor InvalidCheckpoint;
}  // namespace Horo::Release::UpdateTransferErrors
