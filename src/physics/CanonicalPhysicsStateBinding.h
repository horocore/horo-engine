#pragma once

#include "Horo/Physics/PhysicsIdentity.h"
#include "Horo/Physics/PhysicsStateSnapshot.h"

#include <algorithm>
#include <span>

namespace Horo::Physics::Detail {
    struct CanonicalWorld;

    /** @brief Physics-owned resolved stable identity; native fields are private and
     * never emitted by the wire encoder. */
    struct CanonicalPhysicsStateBodyBinding final {
        PhysicsStateObjectId id{};
        BodyHandle body;
        std::uint32_t nativeId{};
        bool hasMotion{};
        bool hasPolicyIntent{};
    };

    /** @brief Exact native manager index is captured cold under retained topology;
     * public identity remains the supplied key. */
    struct CanonicalPhysicsStateJointBinding final {
        PhysicsStateObjectId id{};
        ConstraintHandle constraint;
        std::uint32_t nativeIndex{};
        PhysicsStateConstraintKind kind{};
    };

    /** @brief Borrowed from one complete prepared owner-bound topology manifest,
     * never assembled by a public caller. */
    struct CanonicalPhysicsStateBindings final {
        std::span<const CanonicalPhysicsStateBodyBinding> nativeBodies;  /**< Strict native-ID order for actual recorder resolution.
                                                                          */
        std::span<const CanonicalPhysicsStateBodyBinding> stableBodies;  /**< Strict checkpoint-ID order for restore resolution. */
        std::span<const CanonicalPhysicsStateJointBinding> nativeJoints; /**< Dense actual manager-index order. */
        std::span<const CanonicalPhysicsStateJointBinding> stableJoints; /**< Strict checkpoint-ID order. */
    };

    /** @brief Complete cold-resolved immutable inventory, retained with exactly one
     * configured Physics world topology. */
    struct CanonicalPhysicsStateBindingStorage final {
        std::vector<CanonicalPhysicsStateBodyBinding> nativeBodies, stableBodies;
        std::vector<CanonicalPhysicsStateJointBinding> nativeJoints, stableJoints;

        [[nodiscard]] CanonicalPhysicsStateBindings View() const noexcept {
            return {nativeBodies, stableBodies, nativeJoints, stableJoints};
        }
    };

    /** @brief Resolves complete caller stable bindings against actual native
     * identities at a cold joined owner fence. */
    [[nodiscard]] Result<CanonicalPhysicsStateBindingStorage> PrepareCanonicalPhysicsStateBindings(
        const CanonicalWorld &world, std::span<const PhysicsStateBodyBinding> bodies,
        std::span<const PhysicsStateConstraintBinding> constraints);

    /** @brief Resolves an actual native body from the complete retained inventory
     * in bounded logarithmic work. */
    inline const CanonicalPhysicsStateBodyBinding *FindNativeStateBody(const CanonicalPhysicsStateBindings &bindings,
                                                                       const std::uint32_t id) noexcept {
        const auto found = std::ranges::lower_bound(bindings.nativeBodies, id, {}, &CanonicalPhysicsStateBodyBinding::nativeId);
        return found != bindings.nativeBodies.end() && found->nativeId == id ? &*found : nullptr;
    }

    /** @brief Resolves a stable public body key without accepting native indexes as
     * serialized identity. */
    inline const CanonicalPhysicsStateBodyBinding *FindStableStateBody(const CanonicalPhysicsStateBindings &bindings,
                                                                       const PhysicsStateObjectId id) noexcept {
        const auto found = std::ranges::lower_bound(bindings.stableBodies, id, {}, &CanonicalPhysicsStateBodyBinding::id);
        return found != bindings.stableBodies.end() && found->id == id ? &*found : nullptr;
    }
}  // namespace Horo::Physics::Detail
