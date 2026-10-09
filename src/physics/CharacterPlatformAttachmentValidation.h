#pragma once

#include "Horo/Physics/CharacterControllerContracts.h"

namespace Horo::Character::Detail {
    /**
     * @brief Validates the owned movement-base identity, lifecycle outcome and reconstructible local frame.
     * @param result Complete movement candidate whose support has already been validated.
     * @param descriptor Controller admission policy for eligible platform motion.
     * @return Success for coherent attachment evidence; DescriptorInvalid otherwise.
     */
    [[nodiscard]] Result<void> ValidateCharacterPlatformAttachment(const CharacterMovementResult &result,
                                                                   const CharacterControllerDescriptor &descriptor);
}  // namespace Horo::Character::Detail
