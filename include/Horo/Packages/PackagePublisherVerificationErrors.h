#pragma once

/**
 * @file PackagePublisherVerificationErrors.h
 * @brief Stable package publisher verification failure identities.
 */

#include "Horo/Foundation/ErrorCode.h"

namespace Horo::Packages::PublisherVerificationErrors {
    /** @brief Verification policy or request data is malformed. */
    extern const ErrorCodeDescriptor InvalidInput;
    /** @brief Verification input exceeds a configured resource bound. */
    extern const ErrorCodeDescriptor ResourceLimit;
    /** @brief Verification was cooperatively cancelled before publication. */
    extern const ErrorCodeDescriptor Cancelled;
    /** @brief Verification audit storage cannot accept another record. */
    extern const ErrorCodeDescriptor AuditCapacityExceeded;
    /** @brief Verification admission is closed after shutdown. */
    extern const ErrorCodeDescriptor LifecycleClosed;
}  // namespace Horo::Packages::PublisherVerificationErrors
