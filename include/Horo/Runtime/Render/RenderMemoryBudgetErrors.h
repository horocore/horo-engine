#pragma once

/**
 * @file RenderMemoryBudgetErrors.h
 * @brief Stable typed failures for renderer memory admission and accounting.
 */

#include "Horo/Foundation/ErrorCode.h"

namespace Horo::Render::RenderMemoryBudgetErrors {
    /** @brief The host supplied an invalid or non-finite memory configuration. */
    extern const ErrorCodeDescriptor InvalidConfiguration;
    /** @brief A backend supplied malformed or unsupported allocation requirements. */
    extern const ErrorCodeDescriptor InvalidCostPlan;
    /** @brief The caller supplied a malformed owner scope or resource attempt. */
    extern const ErrorCodeDescriptor InvalidRequest;
    /** @brief The configured hard accounting cap cannot admit the requested backing. */
    extern const ErrorCodeDescriptor BudgetExceeded;
    /** @brief A bounded pool, block, reservation, or allocation table is full. */
    extern const ErrorCodeDescriptor CapacityExceeded;
    /** @brief The reservation identity is malformed, foreign, stale, or no longer pending. */
    extern const ErrorCodeDescriptor InvalidReservation;
    /** @brief The allocation identity is malformed, foreign, stale, or in the wrong state. */
    extern const ErrorCodeDescriptor InvalidAllocation;
    /** @brief The pool identity is malformed, foreign, stale, or no longer present. */
    extern const ErrorCodeDescriptor InvalidPool;
    /** @brief New reservations are disabled because the ledger is shutting down. */
    extern const ErrorCodeDescriptor Stopped;
    /** @brief The requested suballocation cannot be represented by the configured block policy. */
    extern const ErrorCodeDescriptor UnsupportedAllocation;
}  // namespace Horo::Render::RenderMemoryBudgetErrors
