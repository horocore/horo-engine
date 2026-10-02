#pragma once

/** @file EventTrackErrors.h @brief Stable event cook and dispatch failure identities. */

#include "Horo/Foundation/ErrorCode.h"

namespace Horo::Cinematic::EventTrackErrors {
    extern const ErrorCodeDescriptor UnknownName;              /**< Authored qualified event is not declared. */
    extern const ErrorCodeDescriptor SchemaMismatch;           /**< Payload or descriptor version is incompatible. */
    extern const ErrorCodeDescriptor CookInvalid;              /**< Cook input contains a duplicate or invalid identity. */
    extern const ErrorCodeDescriptor CookCapacityExceeded;     /**< Cook input exceeds a finite event or payload bound. */
    extern const ErrorCodeDescriptor BindingUnavailable;       /**< A required cooked handler is absent at activation. */
    extern const ErrorCodeDescriptor StaleBinding;             /**< A handler or player generation was retired after activation. */
    extern const ErrorCodeDescriptor DispatchCapacityExceeded; /**< A complete batch cannot enter the bounded queue. */
    extern const ErrorCodeDescriptor DispatchStateInvalid;     /**< A tick or lifecycle transition is invalid. */
}  // namespace Horo::Cinematic::EventTrackErrors
