#pragma once

#include "Horo/Physics/PhysicsStateSnapshot.h"

#include <atomic>
#include <thread>

namespace Horo::Physics {
    namespace Detail {
        struct PhysicsStateResourceRecord;
    }

    /** @brief One cold control block with a weak back-reference; public handles
     * never expose shared/weak promotion authority. */
    struct PhysicsStateRestoreCandidate::Impl final {
        std::shared_ptr<const Detail::PhysicsStateResourceRecord> resources; /**< Keeps actual image/native/content retention until owner
                                                                                collection. */
        std::weak_ptr<Detail::PhysicsWorldSnapshotState> owner;
        std::thread::id ownerThread;
        std::atomic<std::uint64_t> abandonedSerial{};
        std::atomic<bool> activeHandle{};
    };
}  // namespace Horo::Physics
