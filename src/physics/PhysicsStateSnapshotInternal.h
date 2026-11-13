#pragma once

#include "PhysicsStateSolverStorage.h"

namespace Horo::Physics {
    /** @brief One cold-prepared unpublished value slot. All views point only into
     * its own retained storage. */
    struct PhysicsStateSnapshot::Impl final {
        PhysicsStateCompatibility compatibility;
        std::unique_ptr<StateDetail::SolverStorage> solver;
        std::vector<PhysicsStateEventPair> pairs;
        std::vector<PhysicsStatePublishedEvent> events;
        std::vector<std::byte> wire;
        PhysicsStateWorldView world;
        std::span<const std::byte> construction;
        std::size_t usedWireBytes{};
    };
}  // namespace Horo::Physics
