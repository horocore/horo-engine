#pragma once

/** @file CharacterErrors.h
 * @brief Stable Character error identities independent of Physics backends.
 */

#include "Horo/Foundation/ErrorCode.h"

#include <span>

namespace Horo::Character::CharacterErrors {
    /** @brief Missing or invalid Character-world identity. */
    extern const ErrorCodeDescriptor WorldInvalid;
    /** @brief Missing scene/world/slot generation in a controller handle. */
    extern const ErrorCodeDescriptor HandleMalformed;
    /** @brief A controller handle belongs to another scene or Character-world generation. */
    extern const ErrorCodeDescriptor HandleWorldMismatch;
    /** @brief The owning registry found an absent, retired or replaced controller slot. */
    extern const ErrorCodeDescriptor HandleStale;
    /** @brief Controller descriptor geometry, basis, filtering or limits are invalid. */
    extern const ErrorCodeDescriptor DescriptorInvalid;
    /** @brief A fixed-tick movement request is malformed or internally inconsistent. */
    extern const ErrorCodeDescriptor RequestInvalid;
    /** @brief A movement command is late, duplicated or targets a non-successor tick. */
    extern const ErrorCodeDescriptor CommandOrderInvalid;
    /** @brief Controller, contact or result storage exhausted its admitted bound. */
    extern const ErrorCodeDescriptor CapacityExceeded;
    /** @brief Every controller slot retired at the non-wrapping generation ceiling. */
    extern const ErrorCodeDescriptor GenerationExhausted;
    /** @brief The Character-world lifecycle phase cannot admit the operation. */
    extern const ErrorCodeDescriptor InvalidState;
    /** @brief An enum or operation is unknown to the active Character contract. */
    extern const ErrorCodeDescriptor OperationUnsupported;

    /** @brief Returns the complete canonical Character error descriptor set. @return Stable process-lifetime descriptor view. */
    [[nodiscard]] std::span<const ErrorCodeDescriptor *const> Descriptors() noexcept;
}  // namespace Horo::Character::CharacterErrors
