#pragma once

#include "Horo/Physics/CharacterWorld.h"
#include "Horo/Physics/PhysicsSceneActivation.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

namespace Horo::Physics::Detail {
    /** @brief Generation-qualified runtime binding; template-local/authored IDs never become native entity identity. */
    struct StructuralBodyBinding final {
        Runtime::EntityRef entity;
        Runtime::PhysicsBodySlotId slot;
        BodyHandle handle;
    };

    /** @brief Exact runtime collider occurrence and its native owning body. */
    struct StructuralShapeBinding final {
        Runtime::EntityRef entity;
        Runtime::PhysicsColliderSlotId slot;
        ShapeHandle handle;
        BodyHandle body;
    };

    /** @brief Exact runtime constraint occurrence; never an authored or template-local handle. */
    struct StructuralConstraintBinding final {
        Runtime::EntityRef entity;
        Runtime::PhysicsConstraintSlotId slot;
        ConstraintHandle handle;
    };

    /** @brief Retained publication fence; closed before the borrowed world can be destroyed. */
    struct PhysicsStructuralState final {
        PhysicsWorld *world{};
        Runtime::SceneRuntimeId scene;
        const PhysicsSceneActivationAuthority *authority{};
        PhysicsSceneActivationEvidence evidence;
        std::vector<StructuralBodyBinding> bodies;
        std::vector<StructuralShapeBinding> shapes;
        std::vector<StructuralConstraintBinding> constraints;
        std::vector<PhysicsSceneBodyBinding> authoredBodies;
        std::vector<PhysicsSceneShapeBinding> authoredShapes;
        std::vector<PhysicsSceneConstraintBinding> authoredConstraints;
        std::uint64_t revision{1};
        bool closed{};
    };

    /** @brief Explicit application owner slot populated only by whole-Scene activation publication. */
    struct PhysicsStructuralRegistration final {
        std::shared_ptr<PhysicsStructuralState> active;
    };

    /** @brief Creates the concrete Physics structural adapter without selecting a backend or discovering services. */
    [[nodiscard]] std::unique_ptr<Runtime::SceneStructuralParticipant> MakePhysicsStructuralParticipant(
        std::shared_ptr<PhysicsStructuralRegistration> registration);

    /** @brief Resolves the shared unit-world-scale native body pose policy for activation and structural groups. */
    [[nodiscard]] Result<PhysicsPose> ToBodyPhysicsPose(const Math::Transform &transform);

    struct AuthoredBodyKey final {
        std::uint64_t object{};
        std::uint64_t body{};

        [[nodiscard]] constexpr bool operator==(const AuthoredBodyKey &) const noexcept = default;
    };

    struct PlannedCollider final {
        Runtime::SceneObjectId object;
        Runtime::PhysicsComponentId component;
        Runtime::PhysicsColliderSlotId collider;
        PhysicsShapeDescriptor geometry;
        PhysicsPose localPose;
        bool sensor{};
    };

    struct PlannedBody final {
        Runtime::SceneObjectId object;
        Runtime::PhysicsComponentId component;
        Runtime::PhysicsBodySlotId slot;
        PhysicsPose pose;
        PhysicsAuthoredBodyDescriptor authored;
        bool sensor{};
        std::vector<PlannedCollider> colliders;
    };

    struct PlannedConstraint final {
        Runtime::SceneObjectId object;
        Runtime::PhysicsComponentId component;
        Runtime::PhysicsConstraintSlotId slot;
        Runtime::PhysicsConstraintComponent authored;
        std::size_t firstBody{};
        std::optional<std::size_t> secondBody;
    };

    struct PhysicsScenePlan final {
        std::vector<PlannedBody> bodies;
        std::vector<PlannedConstraint> constraints;
    };

    struct StagedPhysicsScene final {
        std::unique_ptr<PhysicsWorld> physics;
        std::unique_ptr<Character::CharacterWorld> character;
        std::vector<PhysicsSceneBodyBinding> bodyBindings;
        std::vector<PhysicsSceneShapeBinding> shapeBindings;
        std::vector<PhysicsSceneConstraintBinding> constraintBindings;
    };

    /** @brief Adds the authored object/component context to a transactional activation error. */
    [[nodiscard]] Error AddActivationContext(Error error, std::string_view stage, Runtime::SceneObjectId object, std::uint64_t component,
                                             std::optional<Assets::AssetId> asset);

    /** @brief Verifies the capabilities required by the active authored physics portions of a scene. */
    [[nodiscard]] Result<void> RequirePhysicsSceneCapabilities(const PhysicsRuntime &runtime,
                                                               const Runtime::RuntimeSceneDefinition &definition);

    /** @brief Builds a deterministic, fully validated authored Physics scene plan. */
    [[nodiscard]] Result<PhysicsScenePlan> BuildPhysicsScenePlan(const Runtime::RuntimeSceneDefinition &definition,
                                                                 Runtime::RuntimeSceneView scene);

    /** @brief Activates and stages a validated Physics scene plan with rollback ownership. */
    [[nodiscard]] Result<StagedPhysicsScene> StagePhysicsScene(std::unique_ptr<PhysicsWorld> physics,
                                                               std::unique_ptr<Character::CharacterWorld> character,
                                                               PhysicsWorldId identity, const PhysicsScenePlan &plan);
}  // namespace Horo::Physics::Detail
