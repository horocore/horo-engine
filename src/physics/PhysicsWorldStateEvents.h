#pragma once
#include "CanonicalPhysicsStateBackend.h"
#include "PhysicsEventProjection.h"

namespace Horo::Physics::Detail {
    struct PhysicsStateEventCounts final {
        std::size_t pairs{}, events{};
    };

    [[nodiscard]] bool StatePairLess(const PhysicsStateEventPair &one, const PhysicsStateEventPair &two) noexcept;
    /** @brief Maps a genuine retained projection to canonical stable keys, preserving per-pair transition order. */
    [[nodiscard]] Result<PhysicsStateEventCounts> CapturePhysicsStateEvents(const CanonicalPhysicsStateWorkspace &workspace,
                                                                            const PhysicsEventProjectionStateView &source,
                                                                            std::span<PhysicsStateEventPair> pairs,
                                                                            std::span<PhysicsStatePublishedEvent> events);
    /** @brief Rebinds all copied metadata to actual admitted resources before publishing an unpublished projection. */
    [[nodiscard]] Result<void> PreparePhysicsStateEvents(const CanonicalPhysicsStateWorkspace &workspace,
                                                         const PhysicsStateWorldView &source,
                                                         std::span<PhysicsEventProjectionPairState> pairs,
                                                         std::span<PhysicsEventRecord> events, PhysicsEventProjection &staging);
    [[nodiscard]] Result<PhysicsStateEventPair> CapturePhysicsStatePair(const CanonicalPhysicsStateWorkspace &workspace,
                                                                        const PhysicsEventProjectionPairState &source);
    [[nodiscard]] Result<PhysicsEventProjectionPairState> RestorePhysicsStatePair(const CanonicalPhysicsStateWorkspace &workspace,
                                                                                  const PhysicsStateEventPair &source);
}  // namespace Horo::Physics::Detail
