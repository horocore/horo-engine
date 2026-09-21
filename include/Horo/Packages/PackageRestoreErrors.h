#pragma once

/**
 * @file PackageRestoreErrors.h
 * @brief Stable failures reported by the package restore service.
 */

#include "Horo/Foundation/ErrorCode.h"

namespace Horo::Packages::PackageRestoreErrors {
    /** @brief Restore request or service policy is malformed or outside its configured bounds. */
    extern const ErrorCodeDescriptor InvalidInput;
    /** @brief Restore input exceeds a host-owned lockfile or archive bound. */
    extern const ErrorCodeDescriptor ResourceLimit;
    /** @brief Restore admission is already occupied by another operation. */
    extern const ErrorCodeDescriptor Busy;
    /** @brief The project restore lock could not be acquired. */
    extern const ErrorCodeDescriptor LockUnavailable;
    /** @brief Restore service admission has been closed. */
    extern const ErrorCodeDescriptor LifecycleClosed;
    /** @brief Restore was cooperatively cancelled before graph publication. */
    extern const ErrorCodeDescriptor Cancelled;
    /** @brief The lockfile could not be read from the project. */
    extern const ErrorCodeDescriptor LockfileReadFailed;
    /** @brief A required source was not composed for an online cache miss. */
    extern const ErrorCodeDescriptor SourceUnavailable;
    /** @brief Offline restore could not find a required artifact in verified local storage. */
    extern const ErrorCodeDescriptor OfflineArtifactUnavailable;
    /** @brief A fetched artifact did not match the exact digest in the lockfile. */
    extern const ErrorCodeDescriptor ArtifactHashMismatch;
    /** @brief A fetched artifact failed complete archive validation. */
    extern const ErrorCodeDescriptor ArtifactInvalid;
    /** @brief Archive evidence did not match the lockfile's recorded manifest identities. */
    extern const ErrorCodeDescriptor ArtifactEvidenceMismatch;
    /** @brief Publisher policy did not permit the artifact to enter the local graph. */
    extern const ErrorCodeDescriptor PublisherRejected;
    /** @brief Required publisher evidence was unavailable for a cached artifact. */
    extern const ErrorCodeDescriptor PublisherEvidenceUnavailable;
    /** @brief Cache access or publication failed during restore. */
    extern const ErrorCodeDescriptor CacheFailure;
    /** @brief Failed bytes could not be isolated from the active cache namespace. */
    extern const ErrorCodeDescriptor QuarantineFailed;
}  // namespace Horo::Packages::PackageRestoreErrors
