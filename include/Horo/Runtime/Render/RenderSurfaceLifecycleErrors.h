#pragma once

/**
 * @file RenderSurfaceLifecycleErrors.h
 * @brief Stable errors for renderer primary-surface lifecycle transitions.
 */

#include "Horo/Foundation/ErrorCode.h"

namespace Horo::Render::RenderSurfaceLifecycleErrors {
    extern const ErrorCodeDescriptor GenerationExhausted;
    extern const ErrorCodeDescriptor InvalidCommand;
    extern const ErrorCodeDescriptor InvalidIdentity;
    extern const ErrorCodeDescriptor InvalidOutcome;
    extern const ErrorCodeDescriptor InvalidState;
    extern const ErrorCodeDescriptor NoPendingRequest;
    extern const ErrorCodeDescriptor PendingRequestInvalidated;
    extern const ErrorCodeDescriptor RevisionExhausted;
    extern const ErrorCodeDescriptor StaleRequest;
    extern const ErrorCodeDescriptor StaleTransition;
    extern const ErrorCodeDescriptor TransitionBusy;
    extern const ErrorCodeDescriptor WrongThread;
}  // namespace Horo::Render::RenderSurfaceLifecycleErrors
