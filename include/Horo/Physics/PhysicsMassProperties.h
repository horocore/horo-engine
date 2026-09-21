#pragma once

/** @file PhysicsMassProperties.h
 * @brief Backend-neutral mass, density, inertia, and center-of-mass derivation contracts.
 */

#include "Horo/Physics/PhysicsBodyDescriptor.h"
#include "Horo/Physics/PhysicsShapeDescriptor.h"

#include <cstddef>
#include <span>
#include <variant>

namespace Horo::Physics {
    /** @brief Maximum number of explicit analytic contributors in one bounded compound derivation. */
    inline constexpr std::size_t MaximumPhysicsMassPropertyShapes = 1'024;

    /**
     * @brief Symmetric body inertia tensor in kilograms times square meters.
     *
     * The six values are the upper-triangular entries of the symmetric tensor
     * `[Ixx Ixy Ixz; Ixy Iyy Iyz; Ixz Iyz Izz]`. Off-diagonal entries use the
     * tensor sign convention and are not absolute products of inertia.
     */
    struct PhysicsInertiaTensor final {
        float xxKilogramSquareMeters{}; /**< Moment around the body-local X axis. */
        float yyKilogramSquareMeters{}; /**< Moment around the body-local Y axis. */
        float zzKilogramSquareMeters{}; /**< Moment around the body-local Z axis. */
        float xyKilogramSquareMeters{}; /**< Symmetric X/Y product entry. */
        float xzKilogramSquareMeters{}; /**< Symmetric X/Z product entry. */
        float yzKilogramSquareMeters{}; /**< Symmetric Y/Z product entry. */

        [[nodiscard]] constexpr bool operator==(const PhysicsInertiaTensor &) const noexcept = default;
    };

    /**
     * @brief Complete finite mass properties in the receiving body's local frame.
     *
     * The center of mass is expressed in meters relative to the compound reference
     * frame. The inertia tensor is expressed about that center and uses kilograms
     * times square meters. This value owns no shape, world, native solver object,
     * lease, or borrowed storage.
     */
    struct PhysicsMassProperties final {
        float massKilograms{};
        Math::Vec3 centerOfMassMeters{};
        PhysicsInertiaTensor inertia;

        [[nodiscard]] constexpr bool operator==(const PhysicsMassProperties &) const noexcept = default;
    };

    /**
     * @brief Explicit complete mass-property override supplied by an admitted Horo producer.
     *
     * An override is semantic Horo data, not a native solver snapshot. It may be
     * used when an exact cooked shape already carries verified mass properties and
     * therefore does not require runtime geometry traversal.
     */
    struct PhysicsMassPropertiesOverride final {
        PhysicsMassProperties properties;

        [[nodiscard]] constexpr bool operator==(const PhysicsMassPropertiesOverride &) const noexcept = default;
    };

    /** @brief Dynamic mass-property policy: uniform unit-safe mass, density-derived mass, or a complete override. */
    using PhysicsMassPropertiesPolicy = std::variant<PhysicsMass, PhysicsDensity, PhysicsMassPropertiesOverride>;

    /**
     * @brief One resolved analytic contributor to a bounded compound mass calculation.
     *
     * Geometry must already be in normalized SI meters and scale-free form. The
     * local pose is the contributor's body-local frame; contributors are expected
     * to describe disjoint material regions because this contract does not perform
     * overlap subtraction or mesh integration.
     */
    struct PhysicsMassPropertiesShape final {
        PhysicsShapeDescriptor geometry;
        PhysicsPose localPose;
    };

    /**
     * @brief Borrowed mass-property resolution request with no ambient ownership.
     *
     * The shape span remains owned by the caller for the duration of the call and
     * is never retained. An override is self-contained and may be resolved with an
     * empty shape span. Without an override, at least one finite-volume analytic
     * contributor is required.
     */
    struct PhysicsMassPropertiesRequest final {
        PhysicsMassPropertiesPolicy policy{PhysicsMass{}};
        std::span<const PhysicsMassPropertiesShape> shapes;
    };

    /**
     * @brief Validates a complete symmetric inertia tensor without repairing it.
     * @param inertia Tensor in kilograms times square meters.
     * @return Success or PhysicsErrors::DescriptorInvalid for non-finite, non-positive-definite,
     * or numerically malformed entries.
     * @pre The caller is outside native solver callbacks.
     * @post The input is unchanged; no native or world state is inspected.
     */
    [[nodiscard]] Result<void> ValidatePhysicsInertiaTensor(const PhysicsInertiaTensor &inertia);

    /**
     * @brief Validates complete mass properties and explicit positive inertia.
     * @param properties Owned mass, center, and center-of-mass inertia values.
     * @return Success or PhysicsErrors::DescriptorInvalid for non-finite/out-of-profile mass,
     * invalid center coordinates, or invalid inertia.
     * @pre The values use canonical SI units.
     * @post The input is unchanged and no fallback or clamping occurs.
     */
    [[nodiscard]] Result<void> ValidatePhysicsMassProperties(const PhysicsMassProperties &properties);

    /**
     * @brief Resolves density/mass policy or an explicit override into finite compound properties.
     * @param request Borrowed analytic contributors and the closed policy to apply.
     * @return Owned mass properties, OperationUnsupported for non-volumetric geometry, or
     * DescriptorInvalid for malformed, overflowing, empty, or invalid-inertia input.
     * @pre Shape scale has already been folded into geometry; the caller retains the span during the call.
     * @post The request and every contributor remain unchanged; no native solver work or ambient state is touched.
     */
    [[nodiscard]] Result<PhysicsMassProperties> ResolvePhysicsMassProperties(const PhysicsMassPropertiesRequest &request);
}  // namespace Horo::Physics
