#pragma once
/** @file RenderShaderResourceErrors.h
 * @brief Stable cooked shader and graphics pipeline admission failures.
 */
#include "Horo/Foundation/ErrorCode.h"

namespace Horo::Render::RenderShaderResourceErrors {
    extern const ErrorCodeDescriptor InvalidModule;    /**< Cooked stage identity is incomplete or inconsistent. */
    extern const ErrorCodeDescriptor InvalidPipeline;  /**< Graphics state, dependency or compatibility identity is invalid. */
    extern const ErrorCodeDescriptor Unsupported;      /**< Selected backend has no implementation for the requested operation. */
    extern const ErrorCodeDescriptor AllocationFailed; /**< Owned preparation metadata allocation failed. */
    extern const ErrorCodeDescriptor WrongThread;      /**< Preparation or resource access crossed its render-owner boundary. */
}  // namespace Horo::Render::RenderShaderResourceErrors
