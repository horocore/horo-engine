#include "Horo/Physics/PhysicsSceneActivation.h"

#include "Horo/Physics/CharacterWorld.h"
#include "Horo/Physics/PhysicsErrors.h"
#include "PhysicsSceneActivationInternal.h"

#include <algorithm>
#include <array>
#include <format>
#include <memory>
#include <ranges>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace Horo::Physics::Detail {
    /** @copydoc AddActivationContext */
    Error AddActivationContext(Error error, const std::string_view stage, const Runtime::SceneObjectId object,
                               const std::uint64_t component, const std::optional<Assets::AssetId> asset) {
        const std::string assetText = asset.has_value() ? asset->ToString() : std::string{"<none>"};
        error.message = std::format("Physics scene activation {} failed for object {} component {} asset {}: {}", stage, object.value,
                                    component, assetText, error.message);
        error.diagnostics.emplace_back(DiagnosticCode{"physics.scene_activation.context"}, DiagnosticSeverity::Note,
                                       std::format("{} object {} component {} asset {}.", stage, object.value, component, assetText),
                                       SourceLocation{assetText, 0, 0});
        return error;
    }

    namespace {
        /** @brief Converts one planned constraint into the backend-neutral runtime descriptor. */
        [[nodiscard]] PhysicsConstraintDescriptor ToRuntimeConstraint(const PlannedConstraint &planned,
                                                                      const std::vector<BodyHandle> &handles) {
            PhysicsConstraintDescriptor result;
            result.first = {handles[planned.firstBody],
                            {planned.authored.first.localFrame.translation, planned.authored.first.localFrame.rotation}};
            if (planned.secondBody.has_value()) {
                const auto &body = std::get<Runtime::PhysicsConstraintBodyEndpoint>(planned.authored.second);
                result.second = PhysicsBodyAnchor{handles[*planned.secondBody], {body.localFrame.translation, body.localFrame.rotation}};
            } else {
                const auto &world = std::get<Runtime::PhysicsConstraintWorldEndpoint>(planned.authored.second);
                result.second = PhysicsWorldAnchor{{world.frame.translation, world.frame.rotation}};
            }
            using RuntimeParameters = std::variant<PhysicsFixedConstraint, PhysicsDistanceConstraint>;
            result.parameters = std::visit([]<typename Parameter>(const Parameter &parameter) {
                using ParameterType = std::decay_t<Parameter>;
                if constexpr (std::is_same_v<ParameterType, Runtime::PhysicsFixedConstraint>)
                    return RuntimeParameters{PhysicsFixedConstraint{}};
                else
                    return RuntimeParameters{PhysicsDistanceConstraint{parameter.minimumMeters, parameter.maximumMeters}};
            }, planned.authored.parameters);
            return result;
        }

        /** @brief Stages all planned bodies and their immutable shape bindings in one detached world. */
        [[nodiscard]] Result<void> StageSceneBodies(const PhysicsWorld &physics, const PhysicsScenePlan &plan,
                                                    std::vector<PhysicsSceneBodyBinding> &bodyBindings,
                                                    std::vector<PhysicsSceneShapeBinding> &shapeBindings,
                                                    std::vector<BodyHandle> &bodyHandles) {
            bodyBindings.reserve(plan.bodies.size());
            std::size_t shapeCount{};
            for (const PlannedBody &body : plan.bodies)
                shapeCount += body.colliders.size();
            shapeBindings.reserve(shapeCount);
            bodyHandles.reserve(plan.bodies.size());
            for (const PlannedBody &body : plan.bodies) {
                std::vector<PhysicsSceneShapeInstance> instances;
                instances.reserve(body.colliders.size());
                for (const PlannedCollider &collider : body.colliders) {
                    const Result<ShapeHandle> shape = physics.CreateSceneShape(collider.geometry);
                    if (shape.HasError())
                        return Result<void>::Failure(
                            AddActivationContext(shape.ErrorValue(), "shape", collider.object, collider.component.value, std::nullopt));
                    instances.emplace_back(shape.Value(), collider.localPose);
                    shapeBindings.emplace_back(collider.object, collider.collider, shape.Value());
                }
                ShapeHandle bodyShape = instances.front().shape;
                if (instances.size() > 1) {
                    const Result<ShapeHandle> compound = physics.CreateSceneCompoundShape(instances);
                    if (compound.HasError())
                        return Result<void>::Failure(
                            AddActivationContext(compound.ErrorValue(), "compound", body.object, body.component.value, std::nullopt));
                    bodyShape = compound.Value();
                }
                const PhysicsBodyDescriptor descriptor{.shape = bodyShape,
                                                       .pose = body.pose,
                                                       .motion = body.authored.motion,
                                                       .mass = body.authored.mass,
                                                       .linearVelocity = body.authored.initialLinearVelocity,
                                                       .angularVelocity = body.authored.initialAngularVelocity,
                                                       .motionSafety = body.authored.motionSafety};
                const Result<BodyHandle> nativeBody = physics.CreateSceneBody({descriptor, body.sensor});
                if (nativeBody.HasError())
                    return Result<void>::Failure(
                        AddActivationContext(nativeBody.ErrorValue(), "body", body.object, body.component.value, std::nullopt));
                bodyHandles.emplace_back(nativeBody.Value());
                bodyBindings.emplace_back(body.object, body.slot, nativeBody.Value());
            }
            return Result<void>::Success();
        }

        /** @brief Stages all planned constraints after their body handles have been assigned. */
        [[nodiscard]] Result<void> StageSceneConstraints(const PhysicsWorld &physics, const PhysicsScenePlan &plan,
                                                         const std::vector<BodyHandle> &bodyHandles,
                                                         std::vector<PhysicsSceneConstraintBinding> &constraintBindings) {
            constraintBindings.reserve(plan.constraints.size());
            for (const PlannedConstraint &constraint : plan.constraints) {
                const PhysicsConstraintDescriptor descriptor = ToRuntimeConstraint(constraint, bodyHandles);
                const Result<ConstraintHandle> nativeConstraint = physics.CreateSceneConstraint(descriptor);
                if (nativeConstraint.HasError())
                    return Result<void>::Failure(AddActivationContext(nativeConstraint.ErrorValue(), "constraint", constraint.object,
                                                                      constraint.component.value, std::nullopt));
                constraintBindings.emplace_back(constraint.object, constraint.slot, nativeConstraint.Value());
            }
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc StagePhysicsScene */
    Result<StagedPhysicsScene> StagePhysicsScene(std::unique_ptr<PhysicsWorld> physics,
                                                 std::unique_ptr<Character::CharacterWorld> character, const PhysicsWorldId identity,
                                                 const PhysicsScenePlan &plan) {
        StagedPhysicsScene staged{.physics = std::move(physics), .character = std::move(character)};
        const auto abort = [&](Error error) {
            if (staged.character)
                staged.character->Shutdown();
            if (staged.physics)
                staged.physics->Shutdown();
            return Result<StagedPhysicsScene>::Failure(std::move(error));
        };
        if (const Result<void> activated = staged.physics->Activate(identity); activated.HasError())
            return abort(activated.ErrorValue());

        std::vector<BodyHandle> bodyHandles;
        if (const Result<void> bodies = StageSceneBodies(*staged.physics, plan, staged.bodyBindings, staged.shapeBindings, bodyHandles);
            bodies.HasError())
            return abort(bodies.ErrorValue());
        if (const Result<void> constraints = StageSceneConstraints(*staged.physics, plan, bodyHandles, staged.constraintBindings);
            constraints.HasError())
            return abort(constraints.ErrorValue());
        if (const Result<void> activated = staged.character->Activate(); activated.HasError())
            return abort(activated.ErrorValue());
        return Result<StagedPhysicsScene>::Success(std::move(staged));
    }
}  // namespace Horo::Physics::Detail

namespace Horo::Physics {
    /** @copydoc PhysicsSceneActivationParticipant::PhysicsSceneActivationParticipant */
    PhysicsSceneActivationParticipant::PhysicsSceneActivationParticipant(PhysicsRuntime &runtime,
                                                                         PhysicsSceneActivationAuthority &authority,
                                                                         PhysicsSceneActivationSettings settings) noexcept
        : runtime_(&runtime), authority_(&authority), settings_(std::move(settings)) {}

    /** @copydoc PhysicsSceneActivationParticipant::Prepare */
    Result<std::unique_ptr<Runtime::SceneActivationCandidate>> PhysicsSceneActivationParticipant::Prepare(
        const Runtime::RuntimeSceneDefinition &definition, const Runtime::RuntimeSceneView scene) {
        if (const std::array valid{runtime_->State() == PhysicsRuntimeState::Ready, scene.IsCurrent(), scene.RuntimeId().IsValid()};
            !std::ranges::all_of(valid, std::identity{}))
            return Result<std::unique_ptr<Runtime::SceneActivationCandidate>>::Failure(MakeError(PhysicsErrors::WorldInvalid));

        try {
            if (const Result<void> capabilities = Detail::RequirePhysicsSceneCapabilities(*runtime_, definition); capabilities.HasError())
                return Result<std::unique_ptr<Runtime::SceneActivationCandidate>>::Failure(capabilities.ErrorValue());
            Result<Detail::PhysicsScenePlan> builtPlan = Detail::BuildPhysicsScenePlan(definition, scene);
            if (builtPlan.HasError())
                return Result<std::unique_ptr<Runtime::SceneActivationCandidate>>::Failure(builtPlan.ErrorValue());
            Detail::PhysicsScenePlan plan = std::move(builtPlan).Value();
            const PhysicsSceneActivationEvidence evidence = authority_->Capture();
            const Result<PhysicsWorldId> identity = runtime_->IssueWorldIdentity();
            if (identity.HasError())
                return Result<std::unique_ptr<Runtime::SceneActivationCandidate>>::Failure(identity.ErrorValue());
            auto physics = runtime_->PrepareWorld(settings_.physics);
            if (physics.HasError())
                return Result<std::unique_ptr<Runtime::SceneActivationCandidate>>::Failure(physics.ErrorValue());
            auto character = Character::CharacterWorld::Prepare({scene.RuntimeId().value, identity.Value(),
                                                                 evidence.collisionFilterGeneration, evidence.originGeneration},
                                                                settings_.character);
            if (character.HasError())
                return Result<std::unique_ptr<Runtime::SceneActivationCandidate>>::Failure(character.ErrorValue());
            Result<Detail::StagedPhysicsScene> staged =
                Detail::StagePhysicsScene(std::move(physics).Value(), std::move(character).Value(), identity.Value(), plan);
            if (staged.HasError())
                return Result<std::unique_ptr<Runtime::SceneActivationCandidate>>::Failure(staged.ErrorValue());
            Detail::StagedPhysicsScene resources = std::move(staged).Value();
            auto candidate = PhysicsSceneActivationCandidate::Create({.physics = std::move(resources.physics),
                                                                      .character = std::move(resources.character),
                                                                      .authority = authority_,
                                                                      .evidence = evidence,
                                                                      .bodyBindings = std::move(resources.bodyBindings),
                                                                      .shapeBindings = std::move(resources.shapeBindings),
                                                                      .constraintBindings = std::move(resources.constraintBindings)});
            return Result<std::unique_ptr<Runtime::SceneActivationCandidate>>::Success(std::move(candidate));
        } catch (const std::bad_alloc &) {
            return Result<std::unique_ptr<Runtime::SceneActivationCandidate>>::Failure(
                MakeError(PhysicsErrors::CapacityExceeded, "Unable to retain the complete Physics scene activation plan."));
        }
    }
}  // namespace Horo::Physics
