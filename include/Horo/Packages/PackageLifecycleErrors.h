#pragma once
/** @file PackageLifecycleErrors.h
 * @brief Stable typed package activation failures.
 */
#include "Horo/Foundation/ErrorCode.h"

namespace Horo::Packages::PackageLifecycleErrors {
    /** @brief Invalid, oversized, undeclared or incompatible candidate. */
    extern const ErrorCodeDescriptor InvalidCandidate;
    /** @brief Exact approval is absent or does not cover these installed bytes. */
    extern const ErrorCodeDescriptor TrustRequired;
    /** @brief Cancellation was accepted before publication. */
    extern const ErrorCodeDescriptor Cancelled;
    /** @brief Install generation changed during staging. */
    extern const ErrorCodeDescriptor StaleInstall;
    /** @brief A replacement would remove one or more packages with retained executable owners. */
    extern const ErrorCodeDescriptor PackageInUse;
    /** @brief Live removal has reserved or withdrawn this session's native admission. */
    extern const ErrorCodeDescriptor RemovalReserved;
    /** @brief Owner-lane, lifecycle boundary or retained-owner limit rejected activation. */
    extern const ErrorCodeDescriptor InvalidLifecycle;
    /** @brief Private verified content could not be materialized. */
    extern const ErrorCodeDescriptor StorageFailed;
}  // namespace Horo::Packages::PackageLifecycleErrors
