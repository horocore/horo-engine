#pragma once
#include "Horo/Destruction/DestructionSaveCapture.h"

namespace Horo::Destruction::Detail {
    inline constexpr std::size_t MinimumDestructionSavePayloadBytes = 213;
    /** @brief Compute exact schema1 bytes including lossless variable-length participant identity before allocation.
     * @param captured Validated bounded owner value. @return Exact encoded byte length.
     */
    [[nodiscard]] std::size_t DestructionSavePayloadBytes(const CapturedDestructionState &captured) noexcept;
    /** @brief Validate exact active membership and same-epoch Physics participant linkage without interpreting motion. */
    [[nodiscard]] Result<void> ValidateDestructionPhysicsCapture(const CanonicalDestructionSnapshot &state,
                                                                 const DestructionCanonicalArtifactView &artifact,
                                                                 const DestructionSaveCaptureFence &fence,
                                                                 const std::optional<DestructionPhysicsCaptureReceipt> &physics);
}  // namespace Horo::Destruction::Detail
