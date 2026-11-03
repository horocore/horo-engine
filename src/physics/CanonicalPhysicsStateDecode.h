#pragma once

#include "CanonicalPhysicsStateBinding.h"
#include "CanonicalPhysicsStateFields.h"
#include "PhysicsStateSolverStorage.h"

namespace Horo::Physics::Detail {
    /** @brief Decodes ONLY actual native SaveState output into the complete typed
     * solver grammar. */
    [[nodiscard]] Result<PhysicsStateSolverView> DecodeCanonicalPhysicsState(CanonicalPhysicsStateStream &stream,
                                                                             const CanonicalPhysicsStateBindings &bindings,
                                                                             StateDetail::SolverStorage &staging,
                                                                             std::span<const std::uint32_t> activeNativeBodyOrder);

    /** @brief Completes flat contact state from the private trusted native capture
     * stream. */
    [[nodiscard]] Result<void> DecodeCanonicalPhysicsContacts(CanonicalPhysicsStateStream &stream,
                                                              const CanonicalPhysicsStateBindings &bindings,
                                                              StateDetail::SolverStorage &staging, std::uint32_t &pairCount,
                                                              std::uint32_t &manifoldCount, std::uint32_t &ccdCount);
}  // namespace Horo::Physics::Detail
