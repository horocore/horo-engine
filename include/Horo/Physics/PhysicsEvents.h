#pragma once

/**
 * @file PhysicsEvents.h
 * @brief Backend-neutral copied contact and trigger evidence for fixed-tick projection.
 */

#include "Horo/Assets/AssetId.h"
#include "Horo/Math/SceneMath.h"
#include "Horo/Physics/PhysicsCookedShapeDescriptor.h"
#include "Horo/Physics/PhysicsFilterIdentity.h"
#include "Horo/Physics/PhysicsIdentity.h"

#include <compare>
#include <cstdint>
#include <optional>

namespace Horo::Physics {
    /** @brief Lifecycle record kind emitted for one canonical simulation pair. */
    enum class PhysicsEventKind : std::uint8_t {
        ContactBegin,
        ContactPersist,
        ContactEnd,
        TriggerEnter,
        TriggerExit,
    };

    /**
     * @brief Stable body/shape evidence copied from a solver callback.
     *
     * The endpoint contains only Horo-owned value identities. Native body IDs, subshape IDs,
     * pointers and solver-owned storage never cross this boundary.
     */
    struct PhysicsEventEndpoint final {
        BodyHandle body;
        ShapeHandle shape;
        std::optional<PhysicsShapeSubresourceId> subshape;
        CollisionLayerId layer;
        CollisionProfileId profile;
        std::uint64_t filterSchemaGeneration{};

        [[nodiscard]] constexpr auto operator<=>(const PhysicsEventEndpoint &) const noexcept = default;
    };

    /** @brief Canonically ordered pair identity used by lifecycle reconciliation. */
    struct PhysicsEventPairKey final {
        PhysicsEventEndpoint first;
        PhysicsEventEndpoint second;

        [[nodiscard]] constexpr auto operator<=>(const PhysicsEventPairKey &) const noexcept = default;
    };

    /** @brief Stable physical-material evidence copied with one contact endpoint. */
    struct PhysicsEventMaterial final {
        Assets::AssetId asset;
        std::uint64_t assetGeneration{};
        PhysicsMaterialSlotId slot;

        [[nodiscard]] constexpr auto operator<=>(const PhysicsEventMaterial &) const noexcept = default;
    };

    /**
     * @brief Bounded contact point evidence copied before the native solver callback returns.
     *
     * The normal points from the first canonical endpoint toward the second. Impulse data is left
     * at zero until the solver-specific post-step impulse contract is available.
     */
    struct PhysicsContactSummary final {
        Math::Vec3 position{};
        Math::Vec3 normal{0.0F, 1.0F, 0.0F};
        float penetrationDepthMeters{};
        float normalImpulseNewtonSeconds{};

        [[nodiscard]] constexpr auto operator<=>(const PhysicsContactSummary &) const noexcept = default;
    };

    /**
     * @brief Owned callback evidence submitted to the fixed-tick projection.
     *
     * The native adapter copies this value while bodies/manifolds are locked. It never invokes a
     * gameplay consumer, retains a native pointer or mutates Physics world structure.
     */
    struct PhysicsContactObservation final {
        std::uint64_t simulationTick{};
        PhysicsEventEndpoint first;
        PhysicsEventEndpoint second;
        std::optional<PhysicsEventMaterial> firstMaterial;
        std::optional<PhysicsEventMaterial> secondMaterial;
        PhysicsContactSummary contact;
        bool sensor{};

        [[nodiscard]] constexpr auto operator<=>(const PhysicsContactObservation &) const noexcept = default;
    };

    /** @brief One immutable copied lifecycle record in a completed Physics tick. */
    struct PhysicsEventRecord final {
        std::uint64_t simulationTick{};
        PhysicsEventKind kind{PhysicsEventKind::ContactBegin};
        PhysicsEventPairKey pair;
        std::optional<PhysicsEventMaterial> firstMaterial;
        std::optional<PhysicsEventMaterial> secondMaterial;
        PhysicsContactSummary contact;

        [[nodiscard]] constexpr auto operator<=>(const PhysicsEventRecord &) const noexcept = default;
    };
}  // namespace Horo::Physics
