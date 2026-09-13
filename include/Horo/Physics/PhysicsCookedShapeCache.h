#pragma once

/**
 * @file PhysicsCookedShapeCache.h
 * @brief Bounded process-local sharing and lease lifetime for verified cooked Physics shapes.
 */

#include "Horo/Foundation/Result.h"
#include "Horo/Physics/PhysicsConvexHullCook.h"
#include "Horo/Physics/PhysicsCookedShapeDescriptor.h"
#include "Horo/Physics/PhysicsTriangleMeshCook.h"
#include "Horo/Physics/PhysicsWorldBudgets.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

namespace Horo::Physics {
    namespace Detail {
        struct PhysicsCookedShapeResource;
        struct PhysicsCookedShapeCacheState;
    }  // namespace Detail

    /** @brief Process/profile limits for cache-owned immutable cooked shape resources. */
    struct PhysicsCookedShapeCacheLimits final {
        std::uint32_t maximumShapes{65'536};
        std::uint64_t maximumResidentBytes{256ULL * 1024ULL * 1024ULL};

        [[nodiscard]] constexpr auto operator<=>(const PhysicsCookedShapeCacheLimits &) const noexcept = default;
    };

    /** @brief Current cache-owned residency, excluding resources pinned only by retired leases. */
    struct PhysicsCookedShapeCacheStats final {
        std::uint32_t residentShapes{};
        std::uint64_t residentBytes{};
        bool admissionClosed{};

        [[nodiscard]] constexpr auto operator<=>(const PhysicsCookedShapeCacheStats &) const noexcept = default;
    };

    /**
     * @brief Move-only immutable lifetime pin for one exact verified cooked shape resource.
     *
     * Leases contain no native solver handle. They may be moved, inspected and released on any
     * thread. Eviction and cache shutdown remove only the cache retain; an existing lease keeps
     * the immutable resource alive until that lease is destroyed.
     */
    class PhysicsCookedShapeLease final {
    public:
        PhysicsCookedShapeLease() noexcept = default;
        PhysicsCookedShapeLease(const PhysicsCookedShapeLease &) = delete;
        PhysicsCookedShapeLease &operator=(const PhysicsCookedShapeLease &) = delete;
        PhysicsCookedShapeLease(PhysicsCookedShapeLease &&other) noexcept;
        PhysicsCookedShapeLease &operator=(PhysicsCookedShapeLease &&other) noexcept;
        ~PhysicsCookedShapeLease();

        /** @brief Checks whether this lease pins an immutable resource. @return True for an active lease. */
        [[nodiscard]] explicit operator bool() const noexcept;
        /** @brief Returns the exact descriptor that constructed the shared resource. @pre This lease is active. */
        [[nodiscard]] const PhysicsCookedShapeDescriptor &Descriptor() const noexcept;
        /** @brief Returns the bounded resident byte charge for this resource. @pre This lease is active. */
        [[nodiscard]] std::uint64_t ResidentBytes() const noexcept;
        /** @brief Returns immutable verified convex tables, or null when this lease has another kind. */
        [[nodiscard]] const LoadedPhysicsConvexHull *ConvexHull() const noexcept;
        /** @brief Returns immutable verified triangle-mesh tables, or null when this lease has another kind. */
        [[nodiscard]] const LoadedPhysicsTriangleMesh *TriangleMesh() const noexcept;
        /** @brief Tests whether two active leases pin the same immutable resource. */
        [[nodiscard]] bool SharesResourceWith(const PhysicsCookedShapeLease &other) const noexcept;

    private:
        friend class PhysicsCookedShapeCache;
        PhysicsCookedShapeLease(std::shared_ptr<const Detail::PhysicsCookedShapeResource> resource,
                                std::shared_ptr<Detail::PhysicsCookedShapeCacheState> state) noexcept;
        void Release() noexcept;

        std::shared_ptr<const Detail::PhysicsCookedShapeResource> resource_;
        std::shared_ptr<Detail::PhysicsCookedShapeCacheState> state_;
    };

    /**
     * @brief Thread-safe bounded cache keyed by complete cook, payload, target and kind identity.
     *
     * Construction validates and owns canonical runtime tables but exposes no backend-native type.
     * ConvexHull and TriangleMesh are supported by the current qualified loaders. Other cooked
     * kinds fail explicitly until their artifact contracts and native adapters are qualified.
     */
    class PhysicsCookedShapeCache final {  // NOSONAR(cpp:S3624) Pimpl destructor is out-of-line
    public:
        PhysicsCookedShapeCache(const PhysicsCookedShapeCache &) = delete;
        PhysicsCookedShapeCache &operator=(const PhysicsCookedShapeCache &) = delete;
        PhysicsCookedShapeCache(PhysicsCookedShapeCache &&) noexcept;
        PhysicsCookedShapeCache &operator=(PhysicsCookedShapeCache &&) noexcept;
        ~PhysicsCookedShapeCache();

        /**
         * @brief Creates an open cache for one exact qualified Physics target.
         * @param target Complete process/profile cook target identity.
         * @param limits Positive bounded resident shape and byte ceilings.
         * @return Open cache, or CapacityExceeded when limits are zero or outside qualified maxima.
         */
        [[nodiscard]] static Result<PhysicsCookedShapeCache> Create(const PhysicsShapeCookTargetDigest &target,
                                                                    const PhysicsCookedShapeCacheLimits &limits = {});

        /**
         * @brief Acquires an existing exact resource or verifies, constructs and publishes one cache entry.
         * @param descriptor Exact immutable cooked-shape reference.
         * @param payload Candidate cooked bytes used only on a cache miss; an exact resident key ignores them.
         * @return Move-only resource lease, or a stable validation, compatibility, capacity or lifecycle error.
         * @post Failure publishes no partial entry. Concurrent success for one key shares one published resource.
         */
        [[nodiscard]] Result<PhysicsCookedShapeLease> Acquire(const PhysicsCookedShapeDescriptor &descriptor,
                                                              std::span<const std::uint8_t> payload) const;

        /**
         * @brief Removes the cache retain for one exact descriptor while preserving active leases.
         * @param descriptor Complete descriptor whose key should be removed.
         * @return True when a resident entry was removed, false when absent, or validation/lifecycle failure.
         */
        [[nodiscard]] Result<bool> Evict(const PhysicsCookedShapeDescriptor &descriptor) const;

        /**
         * @brief Closes admission and removes every cache retain; active leases remain valid.
         * @return Success. Repeated calls are idempotent.
         */
        [[nodiscard]] Result<void> Shutdown() const noexcept;

        /** @brief Returns an atomic snapshot of cache-owned residency and admission state. */
        [[nodiscard]] PhysicsCookedShapeCacheStats Stats() const noexcept;

    private:
        struct Impl;
        explicit PhysicsCookedShapeCache(std::unique_ptr<Impl> impl) noexcept;

        std::unique_ptr<Impl> impl_;
    };
}  // namespace Horo::Physics
