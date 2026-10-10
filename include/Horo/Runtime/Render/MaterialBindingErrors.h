#pragma once
/** @file MaterialBindingErrors.h
 * @brief Stable generic material binding admission and lifetime failures.
 */
#include "Horo/Foundation/ErrorCode.h"

namespace Horo::Render::MaterialBindingErrors {
    extern const ErrorCodeDescriptor InvalidDescriptor; /**< @brief Logical input, reflection or resource ownership is invalid. */
    extern const ErrorCodeDescriptor Unsupported;       /**< @brief The selected adapter cannot realize this binding contract. */
    extern const ErrorCodeDescriptor CapacityExceeded;  /**< @brief Current and retained generations exceed finite budgets. */
    extern const ErrorCodeDescriptor StaleBinding;      /**< @brief The exact generation is no longer discoverable. */
    extern const ErrorCodeDescriptor Closed;            /**< @brief Table admission has stopped. */
    extern const ErrorCodeDescriptor WrongThread;       /**< @brief Operation crossed the render-owner thread boundary. */
    extern const ErrorCodeDescriptor AllocationFailed;  /**< @brief Bounded preparation storage allocation failed. */
    extern const ErrorCodeDescriptor BackendFailure;    /**< @brief Adapter threw or returned an empty resident lease. */

}  // namespace Horo::Render::MaterialBindingErrors
