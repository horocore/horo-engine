#pragma once

/**
 * @file XRSessionErrors.h
 * @brief Stable XRRuntime-owned lifecycle failure identities.
 */

#include "Horo/Foundation/ErrorCode.h"

#include <span>

namespace Horo::XR::XRSessionErrors {
    /** @brief A selected-backend preparation stage failed before session publication. */
    extern const ErrorCodeDescriptor ActivationFailed;
    /** @brief Cooperative cancellation prevented publication of a newly prepared session. */
    extern const ErrorCodeDescriptor ActivationCancelled;
    /** @brief The test probe cannot finish until its exact unpublished native session is retired. */
    extern const ErrorCodeDescriptor SetupCleanupPending;
    /** @brief A session operation was invoked outside its control thread. */
    extern const ErrorCodeDescriptor WrongThread;
    /** @brief A runtime event cannot legally follow the current session state. */
    extern const ErrorCodeDescriptor TransitionInvalid;

    /**
     * @brief Returns the bounded XRRuntime descriptor contribution for explicit host registration.
     * @return Immutable descriptor references owned for process lifetime by XRRuntime.
     */
    [[nodiscard]] std::span<const ErrorCodeDescriptor *const> Descriptors() noexcept;
}  // namespace Horo::XR::XRSessionErrors
