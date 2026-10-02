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
    /** @brief The host cancelled a private update download before it was verified. */
    extern const ErrorCodeDescriptor Cancelled;
    /** @brief HTTPS connection, timeout, or body transfer failed before a complete verified package. */
    extern const ErrorCodeDescriptor TransportFailed;
    /** @brief Persisted checkpoint bytes are malformed, oversized, or from an unsupported schema. */
    extern const ErrorCodeDescriptor InvalidCheckpoint;
    /** @brief Archive metadata contains unsafe names, links, collisions, or invalid entry types. */
    extern const ErrorCodeDescriptor InvalidArchive;
    /** @brief Archive entry count or expanded file size exceeds host policy. */
    extern const ErrorCodeDescriptor ArchiveResourceLimit;
    /** @brief Extracted private tree differs from the authenticated file inventory. */
    extern const ErrorCodeDescriptor StageMismatch;
}  // namespace Horo::Release::UpdateTransferErrors
