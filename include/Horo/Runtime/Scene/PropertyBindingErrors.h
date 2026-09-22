#pragma once

/**
 * @file PropertyBindingErrors.h
 * @brief Stable errors for the scene-owned typed property-binding registry.
 */

#include "Horo/Foundation/ErrorCode.h"

namespace Horo::Runtime::PropertyBindingErrors {
    /** @brief A binding descriptor is incomplete or contains an invalid policy/value range. */
    extern const ErrorCodeDescriptor InvalidDescriptor;
    /** @brief A binding or component/property pair is already registered. */
    extern const ErrorCodeDescriptor DuplicateBinding;
    /** @brief Registration was attempted after the immutable snapshot was published. */
    extern const ErrorCodeDescriptor RegistryFrozen;
    /** @brief A requested binding is absent from the immutable registry snapshot. */
    extern const ErrorCodeDescriptor BindingMissing;
    /** @brief A target component does not match the descriptor's declared component type. */
    extern const ErrorCodeDescriptor ComponentTypeMismatch;
    /** @brief A supplied value does not match the descriptor's declared property type. */
    extern const ErrorCodeDescriptor ValueTypeMismatch;
    /** @brief A getter rejected a typed read at the owner boundary. */
    extern const ErrorCodeDescriptor ReadRejected;
    /** @brief A setter rejected a typed write at the owner boundary. */
    extern const ErrorCodeDescriptor WriteRejected;
    /** @brief A value violates the descriptor's finite range constraint. */
    extern const ErrorCodeDescriptor ValueOutOfRange;
}  // namespace Horo::Runtime::PropertyBindingErrors
