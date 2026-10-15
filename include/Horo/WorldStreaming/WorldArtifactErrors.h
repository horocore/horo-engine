#pragma once

/** @file WorldArtifactErrors.h
 * @brief Stable typed failures at the canonical world artifact boundary.
 */
#include "Horo/Foundation/ErrorCode.h"

namespace Horo::WorldStreaming::WorldArtifactErrors {
    /** @brief Artifact structure, canonical value or complete inventory is invalid. */
    extern const ErrorCodeDescriptor Invalid;
    /** @brief Submitted artifact or declared storage exceeds an explicit caller bound. */
    extern const ErrorCodeDescriptor CapacityExceeded;
    /** @brief Artifact schema or requested encoding capability is unsupported. */
    extern const ErrorCodeDescriptor Unsupported;
    /** @brief Caller cancellation prevented complete artifact admission or production. */
    extern const ErrorCodeDescriptor Cancelled;
    /** @brief A dependency sibling or captured source belongs to another exact index generation. */
    extern const ErrorCodeDescriptor Stale;
}  // namespace Horo::WorldStreaming::WorldArtifactErrors
