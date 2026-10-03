#pragma once

/**
 * @file PackageInstallErrors.h
 * @brief Typed failures for project package install commits.
 */

#include "Horo/Foundation/ErrorCode.h"

namespace Horo::Packages::PackageInstallErrors {
    /** @brief Project root or candidate graph is malformed or unbounded. */
    extern const ErrorCodeDescriptor InvalidInput;
    /** @brief Candidate archive evidence no longer matches its lock record. */
    extern const ErrorCodeDescriptor EvidenceMismatch;
    /** @brief Cancellation was accepted before the install commit point. */
    extern const ErrorCodeDescriptor Cancelled;
    /** @brief Another installer owns the project transaction lock. */
    extern const ErrorCodeDescriptor LockUnavailable;
    /** @brief Durable install metadata could not be committed. */
    extern const ErrorCodeDescriptor CommitFailed;
}  // namespace Horo::Packages::PackageInstallErrors
