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
    /** @brief A runtime event cannot legally follow the current session state. */
    extern const ErrorCodeDescriptor TransitionInvalid;

    /**
     * @brief Returns the bounded XRRuntime descriptor contribution for explicit host registration.
     * @return Immutable descriptor references owned for process lifetime by XRRuntime.
     */
    [[nodiscard]] std::span<const ErrorCodeDescriptor *const> Descriptors() noexcept;
}  // namespace Horo::XR::XRSessionErrors
