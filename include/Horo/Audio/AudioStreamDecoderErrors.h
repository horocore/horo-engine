#pragma once

/**
 * @file AudioStreamDecoderErrors.h
 * @brief Stable worker-side runtime audio stream decoder failure identities.
 */

#include "Horo/Foundation/ErrorCode.h"

namespace Horo::Audio::AudioStreamDecoderErrors {
    extern const ErrorCodeDescriptor Invalid;              /**< Malformed specification, provider or request. */
    extern const ErrorCodeDescriptor CapacityExceeded;     /**< Declared resource bound or output span is insufficient. */
    extern const ErrorCodeDescriptor SeekUnsupported;      /**< Seeking was not advertised by this provider. */
    extern const ErrorCodeDescriptor Cancelled;            /**< Control cancelled the in-flight or next worker operation. */
    extern const ErrorCodeDescriptor ProviderFailed;       /**< Provider threw or failed without an owned typed error. */
    extern const ErrorCodeDescriptor ProtocolViolation;    /**< Provider reported impossible frame or terminal progress. */
    extern const ErrorCodeDescriptor LifecycleUnavailable; /**< Failed, cancelled or closed session cannot accept more work. */
}  // namespace Horo::Audio::AudioStreamDecoderErrors
