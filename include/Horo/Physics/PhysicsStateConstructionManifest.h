#pragma once

/** @file PhysicsStateConstructionManifest.h
 * @brief Complete typed immutable topology inputs for cold restart construction of an unpublished Physics owner.
 * The canonical snapshot codec owns these fields; this is not a Save-owned wire format or live topology replacement API.
 */
#include "Horo/Physics/PhysicsBodyDescriptor.h"
#include "Horo/Physics/PhysicsConstraintDescriptor.h"
#include "Horo/Physics/PhysicsQuery.h"
#include "Horo/Physics/PhysicsStateConstruction.h"

namespace Horo::Physics {
    /** @brief Canonical collider metadata with a contiguous exact material slice, independent of Scene local handles. */
    struct PhysicsStateConstructionCollider final {
        PhysicsPose localPose;
        std::optional<PhysicsShapeSubresourceId> subshape;
        std::uint32_t materialBegin{}, materialCount{};
        std::optional<PhysicsStateSurfaceEvidence> surface;
        std::uint64_t collider{};
    };

    /** @brief Original typed creation input and current immutable mass policy. Scene routing identities are rebound locally.
     * Exact diagonal/rotation exists only for solver-active Dynamic records. Inactive reserved inertia is deliberately absent.
     * Native mass values are explicit float bits, never reconstructed through inverse tensor roundtrips.
     */
    struct PhysicsStateConstructionBody final {
        PhysicsStateObjectId id{}, shape{};
        PhysicsPose originalPose;
        PhysicsAuthoredBodyDescriptor policy;
        /**< Current immutable mass/motion/safety policy. initialLinearVelocity/initialAngularVelocity MUST be zero:
             historical velocity inputs are deliberately omitted, never described as original constructor evidence.
             Full actual velocity/force/sleep/last-intent continuation restores before any step or event exposure. */
        std::optional<CollisionProfileId> profile;
        std::uint32_t colliderBegin{}, colliderCount{};
        bool sensor{}, motionStorageReserved{}, hasMotion{};
        float inverseMass{};
        std::array<float, 3> inverseInertia{};
        std::array<float, 4> inertiaRotation{};
        PhysicsStateFpControl floatingPoint;            /**< Actual complete settings/CreateBody constructor witness. */
        PhysicsStateFpControl massArithmeticSubprofile; /**< Exact compiled invalid-exception guard profile if native Eigen branch is
            selected. Guard source/configuration is bound by genuine SolverInputs; this does not assert the branch was executed. */
    };

    /** @brief Complete constructor-time COM pose, not the body pose at capture. */
    struct PhysicsStateConstructionCom final {
        std::array<float, 3> position{};
        std::array<float, 4> rotation{0, 0, 0, 1};
    };

    /** @brief One genuine original constraint constructor. Optional second body absent means exact world anchor.
     * Frames/COM/FP are copied from the real constructor witness; current body pose cannot replace them.
     */
    struct PhysicsStateConstructionJoint final {
        PhysicsStateObjectId id{}, first{};
        std::optional<PhysicsStateObjectId> second;
        PhysicsPose firstLocalFrame, secondLocalOrWorldFrame;
        std::variant<PhysicsFixedConstraint, PhysicsDistanceConstraint, PhysicsHingeConstraint, PhysicsSliderConstraint> parameters;
        PhysicsJointCollisionPolicy collisionPolicy{};
        PhysicsPose firstConstructorFrame, secondConstructorFrame;
        PhysicsStateConstructionCom firstCom, secondCom;
        PhysicsStateFpControl floatingPoint;
    };

    /** @brief One original query child policy. Its geometry/local pose come from the exact matching ordered recipe edge. */
    struct PhysicsStateConstructionQueryChild final {
        PhysicsShapeSubresourceId subshape;
        std::optional<PhysicsQueryMaterial> material;
        CollisionLayerId layer;
        CollisionProfileId profile;
        PhysicsQueryChannelId channel;
        PhysicsQueryFixtureResponse response{PhysicsQueryFixtureResponse::Block};
        bool trigger{};
        std::optional<PhysicsStateSurfaceEvidence> surface;
    };

    /** @brief Complete original static query fixture. Compound children are direct analytic inputs under existing limits.
     * Query metadata remains explicit; current profile/channel source authority must resolve on the receiving owner.
     */
    struct PhysicsStateConstructionFixture final {
        PhysicsStateObjectId body{}, shape{};
        PhysicsPose originalPose;
        CollisionLayerId layer;
        CollisionProfileId profile;
        PhysicsQueryChannelId channel;
        PhysicsQueryFixtureResponse response{PhysicsQueryFixtureResponse::Block};
        bool trigger{};
        std::optional<PhysicsShapeSubresourceId> subshape;
        std::optional<PhysicsQueryMaterial> material;
        std::optional<PhysicsStateSurfaceEvidence> surface;
        std::uint32_t childBegin{}, childCount{};
        PhysicsStateFpControl floatingPoint; /**< Actual static body settings/CreateBody boundary. */
    };

    /** @brief Required immutable topology section of the SAME canonical Physics snapshot.
     * Body/joint/fixture rows use strictly ordered stable IDs, collider/material/child slices have exact dense ownership.
     * Every actual resident shape, including query fixtures, is represented by a root and its transitive recipe graph.
     * It carries no Runtime World/entity/shape handles, code pointers, local generations or authority to resolve assets.
     */
    struct PhysicsStateConstructionManifest final {
        PhysicsStateShapeRecipes shapes;
        std::span<const PhysicsStateConstructionBody> bodies;
        std::span<const PhysicsStateConstructionCollider> colliders;
        std::span<const PhysicsQueryMaterial> materials;
        std::span<const PhysicsStateConstructionJoint> joints;
        std::span<const PhysicsStateConstructionFixture> fixtures;
        std::span<const PhysicsStateConstructionQueryChild> queryChildren;
        std::span<const PhysicsStatePersistentOwner>
            owners; /**< Optional generic-owner metadata; Scene Save requires complete exact keys. */
    };

    /** @brief Flat construction row/work ceilings reserved before creation/capture admission. */
    struct PhysicsStateConstructionManifestLimits final {
        PhysicsStateConstructionLimits shapes;
        std::uint32_t maximumBodies{}, maximumColliders{}, maximumMaterials{}, maximumJoints{}, maximumFixtures{}, maximumQueryChildren{};
        std::size_t maximumRetainedBytes{};
        std::size_t maximumEncodedBytes{}; /**< Independent canonical snapshot subsection byte ceiling. */
        std::uint32_t maximumOwners{};
    };

    /** @brief Full finite/identity/slice/DAG validation before native restart construction.
     * Inputs carry no asset/service permission. Depth scratch is private, bounded and disjoint from all input rows.
     */
    [[nodiscard]] Result<void> ValidatePhysicsStateConstructionManifest(const PhysicsStateConstructionManifest &,
                                                                        const PhysicsStateConstructionManifestLimits &,
                                                                        PhysicsStateFpControl current, std::span<std::uint32_t> depths);
}  // namespace Horo::Physics
