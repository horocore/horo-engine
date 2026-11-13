#include "CanonicalPhysicsRuntimeInternal.h"
#include "CanonicalPhysicsStateBodyControl.h"
#include "CanonicalPhysicsStateConstraintReplay.h"
#include "CanonicalPhysicsStateShapeCreation.h"
#include "CanonicalPhysicsStateShapeRecipe.h"
#include "PhysicsStateConstructionNative.h"

#include <Jolt/Physics/Constraints/DistanceConstraint.h>
#include <Jolt/Physics/Constraints/FixedConstraint.h>
#include <Jolt/Physics/Constraints/HingeConstraint.h>
#include <Jolt/Physics/Constraints/SliderConstraint.h>
#include <limits>

namespace Horo::Physics::Detail {
    namespace {
        /** @brief Stable pair key includes Jolt's native body reuse sequence. */
        [[nodiscard]] std::uint64_t CollisionPairKey(const JPH::BodyID first, const JPH::BodyID second) noexcept {
            const auto low = std::min(first.GetIndexAndSequenceNumber(), second.GetIndexAndSequenceNumber());
            const auto high = std::max(first.GetIndexAndSequenceNumber(), second.GetIndexAndSequenceNumber());
            return (static_cast<std::uint64_t>(low) << 32U) | high;
        }

        [[nodiscard]] const CanonicalSceneShapeRecord *FindSceneShape(const CanonicalWorld &world, const ShapeHandle handle) {
            const auto found = std::ranges::find_if(world.scene.shapes, [handle](const auto &shape) {
                return shape.handle == handle;
            });
            return found == world.scene.shapes.end() ? nullptr : std::to_address(found);
        }

        [[nodiscard]] const CanonicalSceneBodyRecord *FindSceneBody(const CanonicalWorld &world, const BodyHandle handle) {
            const auto found = std::ranges::find_if(world.scene.bodies, [handle](const auto &body) {
                return body.handle == handle;
            });
            return found == world.scene.bodies.end() ? nullptr : std::to_address(found);
        }

        [[nodiscard]] Result<void> AddCanonicalCompoundChildren(const CanonicalWorld &world, const PhysicsWorldId owner,
                                                                const std::span<const PhysicsSceneShapeInstance> instances,
                                                                JPH::StaticCompoundShapeSettings &settings) {
            for (const PhysicsSceneShapeInstance &instance : instances) {
                if (const Result<void> valid = ValidatePhysicsHandleOwner(instance.shape, owner); valid.HasError())
                    return Result<void>::Failure(valid.ErrorValue());
                if (const Result<void> pose = ValidatePhysicsPose(instance.localPose); pose.HasError())
                    return Result<void>::Failure(pose.ErrorValue());
                const auto *child = FindSceneShape(world, instance.shape);
                if (child == nullptr)
                    return Result<void>::Failure(MakeError(PhysicsErrors::HandleStale));
                settings.AddShape(ToNative(instance.localPose.translation), ToNative(instance.localPose.rotation), child->shape.GetPtr(),
                                  static_cast<JPH::uint32>(&instance - instances.data() + 1));
            }
            return Result<void>::Success();
        }

        [[nodiscard]] Result<CanonicalConstraintBodies> ResolveCanonicalConstraintBodies(
            const CanonicalWorld &world, const PhysicsWorldId owner, const PhysicsConstraintDescriptor &descriptor,
            const std::span<const CanonicalSceneBodyRecord> staged = {}) {
            if (const Result<void> valid = ValidatePhysicsConstraintDescriptor(descriptor, owner); valid.HasError())
                return Result<CanonicalConstraintBodies>::Failure(valid.ErrorValue());
            const auto find = [&](const BodyHandle handle) {
                const auto found = std::ranges::find(staged, handle, &CanonicalSceneBodyRecord::handle);
                return found == staged.end() ? FindSceneBody(world, handle) : std::to_address(found);
            };
            const auto *first = find(descriptor.first.body);
            if (first == nullptr)
                return Result<CanonicalConstraintBodies>::Failure(MakeError(PhysicsErrors::HandleStale));
            const auto *secondBody = std::get_if<PhysicsBodyAnchor>(&descriptor.second);
            const auto *second = secondBody == nullptr ? nullptr : find(secondBody->body);
            if (secondBody != nullptr && second == nullptr)
                return Result<CanonicalConstraintBodies>::Failure(MakeError(PhysicsErrors::HandleStale));
            return Result<CanonicalConstraintBodies>::Success(CanonicalConstraintBodies{first, second});
        }

        [[nodiscard]] bool HasCanonicalConstraintCapacity(const CanonicalWorld &world) noexcept {
            return world.scene.nextConstraintSlot != std::numeric_limits<std::uint32_t>::max() &&
                   world.scene.constraints.size() < world.scene.maximumConstraints;
        }

        [[nodiscard]] Result<void> ValidateCanonicalConstraintLocks(const JPH::BodyLockWrite &firstLock,
                                                                    const JPH::BodyLockWrite &secondLock,
                                                                    const CanonicalSceneBodyRecord *second) {
            if (!firstLock.Succeeded() || (second != nullptr && !secondLock.Succeeded()))
                return Result<void>::Failure(MakeError(PhysicsErrors::HandleStale));
            return Result<void>::Success();
        }

        [[nodiscard]] JPH::Ref<JPH::Constraint> CreateNativeFixedConstraint(JPH::Body &body1, JPH::Body &body2,
                                                                            const PhysicsPose &firstFrame, const PhysicsPose &secondFrame) {
            JPH::FixedConstraintSettings settings;
            settings.mSpace = JPH::EConstraintSpace::WorldSpace;
            settings.mAutoDetectPoint = false;
            settings.mPoint1 = ToNativePoint(firstFrame.translation);
            settings.mAxisX1 = ToNative(firstFrame.rotation.Rotate({1.0F, 0.0F, 0.0F}));
            settings.mAxisY1 = ToNative(firstFrame.rotation.Rotate({0.0F, 1.0F, 0.0F}));
            settings.mPoint2 = ToNativePoint(secondFrame.translation);
            settings.mAxisX2 = ToNative(secondFrame.rotation.Rotate({1.0F, 0.0F, 0.0F}));
            settings.mAxisY2 = ToNative(secondFrame.rotation.Rotate({0.0F, 1.0F, 0.0F}));
            return settings.Create(body1, body2);
        }

        [[nodiscard]] JPH::Ref<JPH::Constraint> CreateNativeDistanceConstraint(JPH::Body &body1, JPH::Body &body2,
                                                                               const PhysicsPose &firstFrame,
                                                                               const PhysicsPose &secondFrame,
                                                                               const PhysicsDistanceConstraint &distance) {
            JPH::DistanceConstraintSettings settings;
            settings.mSpace = JPH::EConstraintSpace::WorldSpace;
            settings.mPoint1 = ToNativePoint(firstFrame.translation);
            settings.mPoint2 = ToNativePoint(secondFrame.translation);
            settings.mMinDistance = distance.minimumMeters;
            settings.mMaxDistance = distance.maximumMeters;
            return settings.Create(body1, body2);
        }

        /** @brief Maps two explicit anchor frames to Jolt's world-space hinge axes. */
        [[nodiscard]] JPH::Ref<JPH::Constraint> CreateNativeHingeConstraint(JPH::Body &body1, JPH::Body &body2,
                                                                            const PhysicsPose &firstFrame, const PhysicsPose &secondFrame,
                                                                            const PhysicsHingeConstraint &hinge) {
            JPH::HingeConstraintSettings settings;
            settings.mSpace = JPH::EConstraintSpace::WorldSpace;
            settings.mPoint1 = ToNativePoint(firstFrame.translation);
            settings.mHingeAxis1 = ToNative(firstFrame.rotation.Rotate({0.0F, 1.0F, 0.0F}));
            settings.mNormalAxis1 = ToNative(firstFrame.rotation.Rotate({1.0F, 0.0F, 0.0F}));
            settings.mPoint2 = ToNativePoint(secondFrame.translation);
            settings.mHingeAxis2 = ToNative(secondFrame.rotation.Rotate({0.0F, 1.0F, 0.0F}));
            settings.mNormalAxis2 = ToNative(secondFrame.rotation.Rotate({1.0F, 0.0F, 0.0F}));
            settings.mLimitsMin = hinge.minimumRadians;
            settings.mLimitsMax = hinge.maximumRadians;
            return settings.Create(body1, body2);
        }

        /** @brief Maps two explicit anchor frames to Jolt's world-space slider axes. */
        [[nodiscard]] JPH::Ref<JPH::Constraint> CreateNativeSliderConstraint(JPH::Body &body1, JPH::Body &body2,
                                                                             const PhysicsPose &firstFrame, const PhysicsPose &secondFrame,
                                                                             const PhysicsSliderConstraint &slider) {
            JPH::SliderConstraintSettings settings;
            settings.mSpace = JPH::EConstraintSpace::WorldSpace;
            settings.mAutoDetectPoint = false;
            settings.mPoint1 = ToNativePoint(firstFrame.translation);
            settings.mSliderAxis1 = ToNative(firstFrame.rotation.Rotate({1.0F, 0.0F, 0.0F}));
            settings.mNormalAxis1 = ToNative(firstFrame.rotation.Rotate({0.0F, 1.0F, 0.0F}));
            settings.mPoint2 = ToNativePoint(secondFrame.translation);
            settings.mSliderAxis2 = ToNative(secondFrame.rotation.Rotate({1.0F, 0.0F, 0.0F}));
            settings.mNormalAxis2 = ToNative(secondFrame.rotation.Rotate({0.0F, 1.0F, 0.0F}));
            settings.mLimitsMin = slider.minimumMeters;
            settings.mLimitsMax = slider.maximumMeters;
            return settings.Create(body1, body2);
        }

        /** @brief Resolves a live unconstrained mutation target before deriving
         * replacement policy. */
        [[nodiscard]] Result<const CanonicalSceneBodyRecord *> ValidateMutationTarget(const CanonicalWorld &canonical,
                                                                                      const PhysicsWorldId owner,
                                                                                      const PhysicsBodyMutation &mutation) {
            if (const auto handle = ValidatePhysicsHandleOwner(mutation.body, owner); handle.HasError())
                return Result<const CanonicalSceneBodyRecord *>::Failure(handle.ErrorValue());
            const auto *body = FindSceneBody(canonical, mutation.body);
            if (body == nullptr)
                return Result<const CanonicalSceneBodyRecord *>::Failure(MakeError(PhysicsErrors::HandleStale));
            if (JPH::BodyLockRead bodyLock(canonical.native.system->GetBodyLockInterfaceNoLock(), body->nativeBody); !bodyLock.Succeeded())
                return Result<const CanonicalSceneBodyRecord *>::Failure(MakeError(PhysicsErrors::HandleStale));
            if (mutation.wake > PhysicsBodyWakePolicy::Wake)
                return Result<const CanonicalSceneBodyRecord *>::Failure(
                    MakeError(PhysicsErrors::OperationUnsupported, "Unknown body wake policy."));
            if (!mutation.shape && !mutation.motion && !mutation.mass && !mutation.motionSafety && !mutation.linearVelocity &&
                !mutation.angularVelocity && !mutation.continuousCollision && mutation.wake != PhysicsBodyWakePolicy::Wake)
                return Result<const CanonicalSceneBodyRecord *>::Failure(
                    MakeError(PhysicsErrors::DescriptorInvalid, "A body mutation requires a policy field or explicit wake."));
            if (const bool wakeOnly = !mutation.shape && !mutation.motion && !mutation.mass && !mutation.motionSafety &&
                                      !mutation.linearVelocity && !mutation.angularVelocity && !mutation.continuousCollision &&
                                      mutation.wake == PhysicsBodyWakePolicy::Wake;
                !wakeOnly && std::ranges::any_of(canonical.scene.constraints, [&mutation](const auto &constraint) {
                return constraint.first == mutation.body || constraint.second == mutation.body;
            }))
                return Result<const CanonicalSceneBodyRecord *>::Failure(
                    MakeError(PhysicsErrors::OperationUnsupported, "Constrained body mutation requires constraint reconciliation."));
            return Result<const CanonicalSceneBodyRecord *>::Success(body);
        }

        /** @brief Combines retained body policy with the fields supplied by one
         * deferred mutation. */
        [[nodiscard]] PhysicsBodyDescriptor MergeBodyMutation(const PhysicsBodyDescriptor &policy, const PhysicsBodyMutation &mutation) {
            PhysicsBodyDescriptor desired = policy;
            if (mutation.shape)
                desired.shape = *mutation.shape;
            if (mutation.motion) {
                desired.motion = *mutation.motion;
                if (desired.motion != PhysicsMotionType::Dynamic && !mutation.mass)
                    desired.mass = PhysicsNoMass{};
                if (desired.motion == PhysicsMotionType::Static) {
                    if (!mutation.linearVelocity)
                        desired.linearVelocity = {};
                    if (!mutation.angularVelocity)
                        desired.angularVelocity = {};
                }
            }
            if (mutation.continuousCollision)
                desired.continuousCollision = *mutation.continuousCollision;
            if (mutation.mass)
                desired.mass = *mutation.mass;
            if (mutation.motionSafety)
                desired.motionSafety = *mutation.motionSafety;
            if (mutation.linearVelocity)
                desired.linearVelocity = *mutation.linearVelocity;
            if (mutation.angularVelocity)
                desired.angularVelocity = *mutation.angularVelocity;
            return desired;
        }

        /** @brief Calculates and validates native mass properties before any body
         * setter runs. */
        [[nodiscard]] Result<JPH::MassProperties> PrepareMutationMass(const CanonicalSceneBodyRecord &body,
                                                                      const CanonicalSceneShapeRecord &shape,
                                                                      const PhysicsBodyDescriptor &desired) {
            if (desired.motion == PhysicsMotionType::Static)
                return Result<JPH::MassProperties>::Success(JPH::MassProperties{});
            JPH::BodyCreationSettings settings(shape.shape.GetPtr(), ToNativePoint(body.pose.translation), ToNative(body.pose.rotation),
                                               ToNativeMotion(desired.motion), JPH::ObjectLayer{0});
            if (const auto mass = ApplyCanonicalMassPolicy(settings, desired.mass); mass.HasError())
                return Result<JPH::MassProperties>::Failure(mass.ErrorValue());
            JPH::MassProperties prepared = settings.GetMassProperties();
            if (!std::isfinite(prepared.mMass) || prepared.mMass <= 0.0F)
                return Result<JPH::MassProperties>::Failure(
                    MakeError(PhysicsErrors::DescriptorInvalid, "The replacement shape cannot produce finite body mass."));
            return Result<JPH::MassProperties>::Success(std::move(prepared));
        }

        /** @brief Read sensor eligibility while the owner retains the exact live native
         * body. */
        [[nodiscard]] Result<JPH::EMotionQuality> ResolveMutationQuality(const CanonicalWorld &canonical,
                                                                         const CanonicalSceneBodyRecord &body,
                                                                         const CanonicalSceneShapeRecord &shape,
                                                                         const PhysicsBodyDescriptor &desired) {
            JPH::BodyLockRead lock(canonical.native.system->GetBodyLockInterfaceNoLock(), body.nativeBody);
            if (!lock.Succeeded())
                return Result<JPH::EMotionQuality>::Failure(MakeError(PhysicsErrors::HandleStale));
            return ResolveCanonicalMotionQuality(canonical.continuousCollision.policy, desired, *shape.shape, lock.GetBody().IsSensor());
        }

        /** @brief Checks a resident body's captured profile and replacement motion
         * before native mutation. */
        [[nodiscard]] Result<void> ValidateMutationCollision(const CanonicalWorld &canonical, const CanonicalSceneBodyRecord &body,
                                                             const PhysicsBodyDescriptor &desired) {
            if (!body.collision.has_value())
                return Result<void>::Success();
            const auto profile = canonical.simulation.binding.schema->ResolveProfile(body.collision->profile);
            if (profile.HasError())
                return Result<void>::Failure(profile.ErrorValue());
            const auto layers = std::span{canonical.simulation.layers.data(), canonical.simulation.layerCount};
            const auto layer = std::ranges::find(layers, profile.Value()->layer, &CollisionLayerDefinition::id);
            if (layer == layers.end())
                return Result<void>::Failure(MakeError(PhysicsErrors::DescriptorInvalid));
            if (!CanonicalLayerAdmitsMotion(*layer, desired.motion))
                return Result<void>::Failure(MakeError(PhysicsErrors::OperationUnsupported));
            return Result<void>::Success();
        }

        /** @brief Checks replacement policy against resident shape and native motion
         * capabilities. */
        [[nodiscard]] Result<void> ValidateMutationPolicy(const CanonicalWorld &canonical, const PhysicsWorldId owner,
                                                          const CanonicalSceneBodyRecord &body, const PhysicsBodyMutation &mutation,
                                                          const PhysicsBodyDescriptor &desired) {
            if (const auto valid = ValidatePhysicsBodyDescriptor(desired, owner); valid.HasError())
                return valid;
            if (desired.motion == PhysicsMotionType::Static && mutation.wake == PhysicsBodyWakePolicy::Wake && !mutation.shape &&
                !mutation.motion && !mutation.mass && !mutation.motionSafety && !mutation.linearVelocity && !mutation.angularVelocity)
                return Result<void>::Failure(MakeError(PhysicsErrors::OperationUnsupported, "Static bodies cannot be woken."));
            if (mutation.shape && *mutation.shape != body.policy.shape && body.collision && !body.collision->colliders.empty())
                return Result<void>::Failure(
                    MakeError(PhysicsErrors::OperationUnsupported,
                              "Replacing an authored collider body requires detached recreation with fresh owned metadata."));
            const auto *shape = FindSceneShape(canonical, desired.shape);
            if (shape == nullptr)
                return Result<void>::Failure(MakeError(PhysicsErrors::HandleStale));
            const auto quality = ResolveMutationQuality(canonical, body, *shape, desired);
            if (quality.HasError())
                return Result<void>::Failure(quality.ErrorValue());
            if (const auto collision = ValidateMutationCollision(canonical, body, desired); collision.HasError())
                return collision;
            if (quality.Value() == JPH::EMotionQuality::LinearCast) {
                JPH::BodyLockRead lock(canonical.native.system->GetBodyLockInterfaceNoLock(), body.nativeBody);
                if (!lock.Succeeded())
                    return Result<void>::Failure(MakeError(PhysicsErrors::HandleStale));
                if (lock.GetBody().GetObjectLayer() == 0)
                    return Result<void>::Failure(MakeError(PhysicsErrors::CapabilityUnavailable));
            }
            if (desired.motion != PhysicsMotionType::Static && shape->shape->MustBeStatic())
                return Result<void>::Failure(
                    MakeError(PhysicsErrors::ShapeMotionUnsupported, "The requested shape requires a static body."));
            if (desired.motion != PhysicsMotionType::Static && !body.motionStorageReserved)
                return Result<void>::Failure(
                    MakeError(PhysicsErrors::OperationUnsupported, "This static body has no reserved moving-body storage."));
            if (desired.motionSafety.maximumDepenetrationSpeed != body.policy.motionSafety.maximumDepenetrationSpeed)
                return Result<void>::Failure(
                    MakeError(PhysicsErrors::OperationUnsupported, "Per-body depenetration speed cannot be changed by this solver."));
            if (desired.motion != PhysicsMotionType::Static && desired.motionSafety.lockedAxes == PhysicsAxisLock::All)
                return Result<void>::Failure(
                    MakeError(PhysicsErrors::DescriptorInvalid, "A moving body cannot lock all translation and rotation axes."));
            if (const auto mass = PrepareMutationMass(body, *shape, desired); mass.HasError())
                return Result<void>::Failure(mass.ErrorValue());
            return Result<void>::Success();
        }

        /** @brief Updates the resident motion properties while holding its native write
         * lock. */
        [[nodiscard]] Result<void> ApplyMutationMotionProperties(const CanonicalWorld &canonical, const CanonicalSceneBodyRecord &body,
                                                                 const PhysicsBodyDescriptor &desired,
                                                                 const JPH::MassProperties &preparedMass) {
            if (desired.motion == PhysicsMotionType::Static)
                return Result<void>::Success();
            // The owner thread retains this exact native body ID through the safe point.
            JPH::BodyLockWrite lock(canonical.native.system->GetBodyLockInterfaceNoLock(), body.nativeBody);
            if (!lock.Succeeded())
                return Result<void>::Failure(
                    MakeError(PhysicsErrors::SolverFatalCondition, "Resident body vanished during owner-thread mutation."));
            JPH::MotionProperties *properties = lock.GetBody().GetMotionProperties();
            properties->SetMassProperties(ToNativeAllowedDOFs(desired.motionSafety.lockedAxes), preparedMass);
            properties->SetLinearDamping(desired.motionSafety.linearDampingPerSecond);
            properties->SetAngularDamping(desired.motionSafety.angularDampingPerSecond);
            properties->SetMaxLinearVelocity(desired.motionSafety.maximumLinearSpeed);
            properties->SetMaxAngularVelocity(desired.motionSafety.maximumAngularSpeed);
            return Result<void>::Success();
        }

        /** @brief Preserves unspecified velocity components while applying new native
         * axis locks and ceilings. */
        void ApplyMutationVelocity(JPH::BodyInterface &interface, const JPH::BodyID body, const PhysicsBodyMutation &mutation,
                                   const PhysicsMotionType motion, const bool reconcileSafety) {
            if (motion == PhysicsMotionType::Static || (!mutation.linearVelocity && !mutation.angularVelocity && !reconcileSafety))
                return;
            JPH::Vec3 linear;
            JPH::Vec3 angular;
            interface.GetLinearAndAngularVelocity(body, linear, angular);
            interface.SetLinearAndAngularVelocity(body, mutation.linearVelocity ? ToNative(*mutation.linearVelocity) : linear,
                                                  mutation.angularVelocity ? ToNative(*mutation.angularVelocity) : angular);
        }

        /** @brief Validated safe-point actions shared by geometry and policy
         * reconciliation. */
        struct MutationReconciliation {
            bool changedCcd;
            bool safetyWakes;
            bool wake;
        };

        /** @brief Reconciles motion storage, CCD contacts and wake state after the
         * validated geometry transition. */
        [[nodiscard]] Result<void> ReconcileMutationPolicy(CanonicalWorld &canonical, CanonicalSceneBodyRecord &body,
                                                           const CanonicalSceneShapeRecord &shape, const PhysicsBodyDescriptor &desired,
                                                           const PhysicsBodyMutation &mutation, const JPH::MassProperties &preparedMass,
                                                           const MutationReconciliation actions) {
            auto &interface = canonical.native.system->GetBodyInterface();
            if (desired.motion != PhysicsMotionType::Static) {
                const auto quality = ResolveMutationQuality(canonical, body, shape, desired);
                if (quality.HasError())
                    return Result<void>::Failure(quality.ErrorValue());
                interface.SetMotionQuality(body.nativeBody, quality.Value());
            }
            if (actions.changedCcd)
                interface.InvalidateContactCache(body.nativeBody);
            if (const auto updated = ApplyMutationMotionProperties(canonical, body, desired, preparedMass); updated.HasError())
                return updated;
            ApplyMutationVelocity(interface, body.nativeBody, mutation, desired.motion, actions.safetyWakes);
            if (actions.wake && desired.motion != PhysicsMotionType::Static)
                interface.ActivateBody(body.nativeBody);
            body.policy = desired;
            body.shapeEvidence = shape.shape;
            body.shapeNativeRequestedBytes = shape.nativeRequestedBytes;
            body.shapeRecipe = shape.recipe;
            body.shapeConstruction = shape.construction;
            return Result<void>::Success();
        }

    }  // namespace

    /** @copydoc BuildCanonicalSceneConstraint */
    Result<JPH::Ref<JPH::Constraint>> BuildCanonicalSceneConstraint(JPH::Body &body1, JPH::Body &body2, const PhysicsPose &firstFrame,
                                                                    const PhysicsPose &secondFrame,
                                                                    const decltype(PhysicsConstraintDescriptor::parameters) &parameters) {
        JPH::Ref<JPH::Constraint> nativeConstraint = std::visit([&]<typename Parameter>(const Parameter &value) {
            using ParameterType = std::decay_t<Parameter>;
            if constexpr (std::is_same_v<ParameterType, PhysicsFixedConstraint>)
                return CreateNativeFixedConstraint(body1, body2, firstFrame, secondFrame);
            else if constexpr (std::is_same_v<ParameterType, PhysicsDistanceConstraint>)
                return CreateNativeDistanceConstraint(body1, body2, firstFrame, secondFrame, value);
            else if constexpr (std::is_same_v<ParameterType, PhysicsHingeConstraint>)
                return CreateNativeHingeConstraint(body1, body2, firstFrame, secondFrame, value);
            else
                return CreateNativeSliderConstraint(body1, body2, firstFrame, secondFrame, value);
        }, parameters);
        if (nativeConstraint == nullptr)
            return Result<JPH::Ref<JPH::Constraint>>::Failure(
                MakeError(PhysicsErrors::OperationUnsupported, "Canonical solver rejected the scene constraint admission."));
        return Result<JPH::Ref<JPH::Constraint>>::Success(std::move(nativeConstraint));
    }

    /** @copydoc CreateCanonicalSceneShape */
    Result<ShapeHandle> CreateCanonicalSceneShape(const CanonicalWorldHandle world, const PhysicsWorldId owner,
                                                  const PhysicsShapeDescriptor &descriptor,
                                                  const StateDetail::ConstructionRecipeLease *preparedRecipe) {
        if (world.value == nullptr || !owner.IsValid())
            return Result<ShapeHandle>::Failure(MakeError(PhysicsErrors::WorldInvalid));
        auto &canonical = *static_cast<CanonicalWorld *>(world.value);
        if (canonical.nextResourceSlot == std::numeric_limits<std::uint32_t>::max() ||
            canonical.scene.shapes.size() >= canonical.scene.maximumShapes)
            return Result<ShapeHandle>::Failure(MakeError(PhysicsErrors::CapacityExceeded));
        if (const Result<void> valid = ValidatePhysicsShapeDescriptor(descriptor); valid.HasError())
            return Result<ShapeHandle>::Failure(valid.ErrorValue());
        if (auto admitted = ValidateCanonicalRecipeAdmission(canonical.owner.constructionRecipes, preparedRecipe); admitted.HasError())
            return Result<ShapeHandle>::Failure(admitted.ErrorValue());
        const auto nativeShape = ObserveCanonicalPhysicsShapeCreation([&descriptor] {
            return CreateNativeShape(descriptor);
        });
        if (nativeShape.HasError())
            return Result<ShapeHandle>::Failure(nativeShape.ErrorValue());

        const auto witness = BindCanonicalShapeCreationRecipe(CanonicalPrimitiveRecipe(descriptor), nativeShape.Value().creationControl);
        auto construction = ValidateCanonicalCreatedRecipe(canonical.owner.constructionRecipes, preparedRecipe,
                                                           nativeShape.Value().creationControl, witness);
        if (construction.HasError())
            return Result<ShapeHandle>::Failure(construction.ErrorValue());
        const std::uint32_t slot = canonical.nextResourceSlot++;
        const ShapeHandle identity{owner, {slot, 1}};
        canonical.scene.shapes.emplace_back(CanonicalSceneShapeRecord{.handle = identity,
                                                                      .shape = nativeShape.Value().shape,
                                                                      .nativeRequestedBytes = nativeShape.Value().nativeRequestedBytes,
                                                                      .construction = std::move(construction).Value(),
                                                                      .recipe = witness,
                                                                      .analytic = descriptor});
        return Result<ShapeHandle>::Success(identity);
    }

    namespace {
        /** @brief Parent construction requests plus retained child geometry form one
         * conservative immutable DAG reservation. */
        Result<CanonicalPhysicsStateCreatedShape> ReservedCompound(CanonicalWorld &world,
                                                                   std::span<const PhysicsSceneShapeInstance> instances,
                                                                   JPH::StaticCompoundShapeSettings &settings) {
            using Outcome = Result<CanonicalPhysicsStateCreatedShape>;
            std::size_t children{};
            for (const auto &instance : instances)
                if (!AddCanonicalPhysicsShapeReservation(FindSceneShape(world, instance.shape)->nativeRequestedBytes, children))
                    return Outcome::Failure(MakeError(PhysicsErrors::CapacityExceeded));
            auto observed = ObserveCanonicalPhysicsShapeCreation([&settings] {
                using Native = Result<JPH::Ref<JPH::Shape>>;
                const auto created = settings.Create();
                if (created.HasError())
                    return Native::Failure(
                        MakeError(PhysicsErrors::ShapeArtifactInvalid, "Canonical solver rejected the scene compound shape."));
                return Native::Success(created.Get());
            });
            if (observed.HasError())
                return observed;
            auto retained = std::move(observed).Value();
            if (!AddCanonicalPhysicsShapeReservation(children, retained.nativeRequestedBytes))
                return Outcome::Failure(MakeError(PhysicsErrors::CapacityExceeded));
            return Outcome::Success(std::move(retained));
        }

        /** @brief Same actual retained child construction witness, independent of
         * transient ShapeHandle values. */
        Sha256Digest CompoundRecipe(const CanonicalWorld &world, std::span<const PhysicsSceneShapeInstance> instances) noexcept {
            auto hash = BeginCanonicalCompoundRecipe(instances.size());
            for (const auto &instance : instances) {
                const auto &recipe = FindSceneShape(world, instance.shape)->recipe;
                if (!HasCanonicalShapeRecipe(recipe))
                    return {};
                hash.Digest(recipe);
                hash.Pose(instance.localPose);
            }
            return hash.Finish();
        }
    }  // namespace

    /** @copydoc CreateCanonicalSceneCompoundShape */
    Result<ShapeHandle> CreateCanonicalSceneCompoundShape(const CanonicalWorldHandle world, const PhysicsWorldId owner,
                                                          const std::span<const PhysicsSceneShapeInstance> instances,
                                                          const StateDetail::ConstructionRecipeLease *preparedRecipe) {
        if (world.value == nullptr || !owner.IsValid())
            return Result<ShapeHandle>::Failure(MakeError(PhysicsErrors::WorldInvalid));
        if (instances.empty())
            return Result<ShapeHandle>::Failure(
                MakeError(PhysicsErrors::DescriptorInvalid, "A canonical scene compound requires at least one child shape."));
        auto &canonical = *static_cast<CanonicalWorld *>(world.value);
        if (canonical.nextResourceSlot == std::numeric_limits<std::uint32_t>::max() ||
            canonical.scene.shapes.size() >= canonical.scene.maximumShapes)
            return Result<ShapeHandle>::Failure(MakeError(PhysicsErrors::CapacityExceeded));

        if (auto admitted = ValidateCanonicalRecipeAdmission(canonical.owner.constructionRecipes, preparedRecipe); admitted.HasError())
            return Result<ShapeHandle>::Failure(admitted.ErrorValue());
        JPH::StaticCompoundShapeSettings settings;
        if (const Result<void> children = AddCanonicalCompoundChildren(canonical, owner, instances, settings); children.HasError())
            return Result<ShapeHandle>::Failure(children.ErrorValue());
        const auto created = ReservedCompound(canonical, instances, settings);
        if (created.HasError())
            return Result<ShapeHandle>::Failure(created.ErrorValue());

        const auto witness = BindCanonicalShapeCreationRecipe(CompoundRecipe(canonical, instances), created.Value().creationControl);
        auto construction =
            ValidateCanonicalCreatedRecipe(canonical.owner.constructionRecipes, preparedRecipe, created.Value().creationControl, witness);
        if (construction.HasError())
            return Result<ShapeHandle>::Failure(construction.ErrorValue());
        const std::uint32_t slot = canonical.nextResourceSlot++;
        const ShapeHandle identity{owner, {slot, 1}};
        std::vector<PhysicsSceneShapeInstance> children(instances.begin(), instances.end());
        canonical.scene.shapes.emplace_back(CanonicalSceneShapeRecord{.handle = identity,
                                                                      .shape = created.Value().shape,
                                                                      .nativeRequestedBytes = created.Value().nativeRequestedBytes,
                                                                      .construction = std::move(construction).Value(),
                                                                      .recipe = witness,
                                                                      .compoundChildren = std::move(children)});
        return Result<ShapeHandle>::Success(identity);
    }

    /** @copydoc ResolveCanonicalBodyMutation */
    Result<PhysicsBodyDescriptor> ResolveCanonicalBodyMutation(const CanonicalWorldHandle world, const PhysicsWorldId owner,
                                                               const PhysicsBodyMutation &mutation) {
        if (world.value == nullptr || !owner.IsValid())
            return Result<PhysicsBodyDescriptor>::Failure(MakeError(PhysicsErrors::WorldInvalid));
        const auto &canonical = *static_cast<CanonicalWorld *>(world.value);
        const auto target = ValidateMutationTarget(canonical, owner, mutation);
        if (target.HasError())
            return Result<PhysicsBodyDescriptor>::Failure(target.ErrorValue());
        PhysicsBodyDescriptor desired = MergeBodyMutation(target.Value()->policy, mutation);
        if (const auto valid = ValidateMutationPolicy(canonical, owner, *target.Value(), mutation, desired); valid.HasError())
            return Result<PhysicsBodyDescriptor>::Failure(valid.ErrorValue());
        return Result<PhysicsBodyDescriptor>::Success(std::move(desired));
    }

    namespace {
        /** @brief Exact legacy wake-only command, without changing motion/mass state. */
        bool IsWakeOnly(const PhysicsBodyMutation &mutation) noexcept {
            return !mutation.shape && !mutation.motion && !mutation.mass && !mutation.motionSafety && !mutation.linearVelocity &&
                   !mutation.angularVelocity && !mutation.continuousCollision && mutation.wake == PhysicsBodyWakePolicy::Wake;
        }

        /** @brief Only the existing sleep-invalidating safety changes force activation. */
        bool MutationSafetyWakes(const PhysicsBodyMutation &mutation, const PhysicsBodyDescriptor &before,
                                 const PhysicsBodyDescriptor &desired) noexcept {
            return mutation.motionSafety && (desired.motionSafety.lockedAxes != before.motionSafety.lockedAxes ||
                                             desired.motionSafety.maximumLinearSpeed != before.motionSafety.maximumLinearSpeed ||
                                             desired.motionSafety.maximumAngularSpeed != before.motionSafety.maximumAngularSpeed);
        }

        /** @brief Preserves existing explicit wake and policy-change activation semantics. */
        bool MutationWakes(const PhysicsBodyMutation &mutation, bool changedShape, bool changedMotion, bool changedCcd,
                           bool safetyWakes) noexcept {
            return changedShape || changedMotion || changedCcd || mutation.mass || safetyWakes || mutation.linearVelocity ||
                   mutation.angularVelocity || mutation.wake == PhysicsBodyWakePolicy::Wake;
        }

        /** @brief Motion ordering preserves the pinned safe-point mass-reconciliation contract. */
        void ApplyMutationGeometry(JPH::BodyInterface &interface, const CanonicalSceneBodyRecord &body,
                                   const CanonicalSceneShapeRecord &shape, const PhysicsBodyDescriptor &desired,
                                   JPH::EActivation activation) {
            const bool changedMotion = body.policy.motion != desired.motion;
            if (changedMotion && desired.motion == PhysicsMotionType::Static)
                interface.SetMotionType(body.nativeBody, JPH::EMotionType::Static, JPH::EActivation::DontActivate);
            if (body.policy.shape != desired.shape)
                interface.SetShape(body.nativeBody, shape.shape.GetPtr(), false, activation);
            if (changedMotion && desired.motion != PhysicsMotionType::Static)
                interface.SetMotionType(body.nativeBody, ToNativeMotion(desired.motion), activation);
        }

        /** @brief Performs admitted mutation and conservatively revokes incompatible arithmetic evidence. */
        Result<void> ApplyTrackedMutation(CanonicalWorld &world, CanonicalSceneBodyRecord &body, const CanonicalSceneShapeRecord &shape,
                                          const PhysicsBodyDescriptor &desired, const PhysicsBodyMutation &mutation) {
            const auto before = ReadCanonicalBodyControl();
            if (auto control = AdmitCanonicalBodyControl(world.owner.constructionRecipes, before); control.HasError())
                return control;
            auto &interface = world.native.system->GetBodyInterface();
            if (IsWakeOnly(mutation)) {
                interface.ActivateBody(body.nativeBody);
                FoldCanonicalBodyControl(body.stateArithmeticControl, before, ReadCanonicalBodyControl());
                return Result<void>::Success();
            }
            const auto mass = PrepareMutationMass(body, shape, desired);
            if (mass.HasError())
                return Result<void>::Failure(mass.ErrorValue());
            const bool changedCcd = body.policy.continuousCollision != desired.continuousCollision;
            const bool safety = MutationSafetyWakes(mutation, body.policy, desired);
            const bool wake =
                MutationWakes(mutation, body.policy.shape != desired.shape, body.policy.motion != desired.motion, changedCcd, safety);
            ApplyMutationGeometry(interface, body, shape, desired, wake ? JPH::EActivation::Activate : JPH::EActivation::DontActivate);
            auto reconciled = ReconcileMutationPolicy(world, body, shape, desired, mutation, mass.Value(), {changedCcd, safety, wake});
            FoldCanonicalBodyControl(body.stateArithmeticControl, before, ReadCanonicalBodyControl());
            return reconciled;
        }
    }  // namespace

    /** @copydoc ApplyCanonicalBodyMutation */
    Result<void> ApplyCanonicalBodyMutation(const CanonicalWorldHandle world, const PhysicsWorldId owner,
                                            const PhysicsBodyMutation &mutation) {
        const auto resolved = ResolveCanonicalBodyMutation(world, owner, mutation);
        if (resolved.HasError())
            return Result<void>::Failure(resolved.ErrorValue());
        auto &canonical = *static_cast<CanonicalWorld *>(world.value);
        auto found = std::ranges::find(canonical.scene.bodies, mutation.body, &CanonicalSceneBodyRecord::handle);
        const auto *shape = FindSceneShape(canonical, resolved.Value().shape);
        return ApplyTrackedMutation(canonical, *found, *shape, resolved.Value(), mutation);
    }

    namespace {
        Result<CanonicalConstraintCreation> ConstraintCreation(const CanonicalConstraintBodies &bodies,
                                                               const PhysicsConstraintDescriptor &descriptor, JPH::Body &first,
                                                               JPH::Body &second, const CanonicalConstraintCreation *recorded) {
            if (recorded) {
                auto current = ReadCanonicalPhysicsStateFpControl();
                if (current.HasError())
                    return Result<CanonicalConstraintCreation>::Failure(current.ErrorValue());
                if (current.Value() != recorded->floatingPoint)
                    return Result<CanonicalConstraintCreation>::Failure(MakeError(PhysicsErrors::ProfileUnsupported));
                return Result<CanonicalConstraintCreation>::Success(*recorded);
            }
            const auto firstFrame = ComposePhysicsPose(bodies.first->pose, descriptor.first.localFrame);
            const auto secondFrame =
                bodies.second ? ComposePhysicsPose(bodies.second->pose, std::get<PhysicsBodyAnchor>(descriptor.second).localFrame)
                              : std::get<PhysicsWorldAnchor>(descriptor.second).frame;
            return CaptureCanonicalConstraintCreation(first, second, firstFrame, secondFrame);
        }

        Result<JPH::Ref<JPH::Constraint>> ConstructConstraint(const PhysicsConstraintDescriptor &descriptor,
                                                              const CanonicalConstraintBodies &bodies,
                                                              const CanonicalConstraintCreation &creation, JPH::Body &first,
                                                              JPH::Body &second, bool replay) {
            if (replay)
                return ReplayCanonicalConstraintCreation(creation, descriptor, bodies.second != nullptr, first, second);
            return BuildCanonicalSceneConstraint(first, second, creation.firstFrame, creation.secondFrame, descriptor.parameters);
        }

        /** @brief Holds exact endpoint locks through constructor provenance capture and
         * the single canonical native build. */
        Result<CanonicalSceneConstraintRecord> PrepareLockedConstraint(CanonicalWorld &canonical, PhysicsWorldId owner,
                                                                       const PhysicsConstraintDescriptor &descriptor,
                                                                       const CanonicalConstraintBodies &bodies,
                                                                       const CanonicalConstraintCreation *recorded = nullptr) {
            using Prepared = Result<CanonicalSceneConstraintRecord>;
            const CanonicalSceneBodyRecord *first = bodies.first;
            const CanonicalSceneBodyRecord *second = bodies.second;
            const auto *secondBody = std::get_if<PhysicsBodyAnchor>(&descriptor.second);

            JPH::BodyLockWrite firstLock(canonical.native.system->GetBodyLockInterfaceNoLock(), first->nativeBody);
            JPH::BodyLockWrite secondLock(canonical.native.system->GetBodyLockInterfaceNoLock(),
                                          second != nullptr ? second->nativeBody : JPH::BodyID{});
            if (const Result<void> locks = ValidateCanonicalConstraintLocks(firstLock, secondLock, second); locks.HasError())
                return Prepared::Failure(locks.ErrorValue());

            auto &secondNative = second ? secondLock.GetBody() : JPH::Body::sFixedToWorld;
            auto creation = ConstraintCreation(bodies, descriptor, firstLock.GetBody(), secondNative, recorded);
            if (creation.HasError())
                return Prepared::Failure(creation.ErrorValue());
            auto nativeConstraint =
                ConstructConstraint(descriptor, bodies, creation.Value(), firstLock.GetBody(), secondNative, recorded != nullptr);
            if (nativeConstraint.HasError())
                return Prepared::Failure(nativeConstraint.ErrorValue());

            const JPH::BodyID secondNativeBody = second == nullptr ? JPH::BodyID{} : second->nativeBody;
            const ConstraintHandle identity{owner, {canonical.scene.nextConstraintSlot++, 1}};
            return Prepared::Success({.handle = identity,
                                      .constraint = nativeConstraint.Value(),
                                      .first = descriptor.first.body,
                                      .second = secondBody != nullptr ? secondBody->body : BodyHandle{},
                                      .firstBody = first->nativeBody,
                                      .secondBody = secondNativeBody,
                                      .collisionPolicy = descriptor.collisionPolicy,
                                      .creation = creation.Value(),
                                      .descriptor = descriptor});
        }
    }  // namespace

    namespace {
        Result<CanonicalConstraintBodies> AdmitConstraintRecord(const CanonicalWorldHandle world, const PhysicsWorldId owner,
                                                                const PhysicsConstraintDescriptor &descriptor,
                                                                const std::span<const CanonicalSceneBodyRecord> staged) {
            using Prepared = Result<CanonicalConstraintBodies>;
            if (world.value == nullptr || !owner.IsValid())
                return Prepared::Failure(MakeError(PhysicsErrors::WorldInvalid));
            const auto &canonical = *static_cast<CanonicalWorld *>(world.value);
            if (!HasCanonicalConstraintCapacity(canonical))
                return Prepared::Failure(MakeError(PhysicsErrors::CapacityExceeded));
            if (std::holds_alternative<PhysicsBodyAnchor>(descriptor.second) &&
                descriptor.collisionPolicy == PhysicsJointCollisionPolicy::AllowBetweenBodies)
                return Prepared::Failure(MakeError(PhysicsErrors::OperationUnsupported));
            const Result<CanonicalConstraintBodies> bodies = ResolveCanonicalConstraintBodies(canonical, owner, descriptor, staged);
            if (bodies.HasError())
                return Prepared::Failure(bodies.ErrorValue());
            return bodies;
        }
    }  // namespace

    /** @copydoc PrepareCanonicalConstraintRecord */
    Result<CanonicalSceneConstraintRecord> PrepareCanonicalConstraintRecord(const CanonicalWorldHandle world, const PhysicsWorldId owner,
                                                                            const PhysicsConstraintDescriptor &descriptor,
                                                                            const std::span<const CanonicalSceneBodyRecord> staged) {
        auto bodies = AdmitConstraintRecord(world, owner, descriptor, staged);
        if (bodies.HasError())
            return Result<CanonicalSceneConstraintRecord>::Failure(bodies.ErrorValue());
        return PrepareLockedConstraint(*static_cast<CanonicalWorld *>(world.value), owner, descriptor, bodies.Value());
    }

    /** @copydoc CreateCanonicalSceneConstraint */
    Result<ConstraintHandle> CreateCanonicalSceneConstraint(const CanonicalWorldHandle world, const PhysicsWorldId owner,
                                                            const PhysicsConstraintDescriptor &descriptor) {
        auto prepared = PrepareCanonicalConstraintRecord(world, owner, descriptor, {});
        if (prepared.HasError())
            return Result<ConstraintHandle>::Failure(prepared.ErrorValue());
        return PublishCanonicalConstraintRecord(*static_cast<CanonicalWorld *>(world.value), std::move(prepared).Value());
    }

    /** @copydoc PublishCanonicalConstraintRecord */
    Result<ConstraintHandle> PublishCanonicalConstraintRecord(CanonicalWorld &canonical, CanonicalSceneConstraintRecord record) {
        if (const auto *second = std::get_if<PhysicsBodyAnchor>(&record.descriptor.second);
            second != nullptr && record.descriptor.collisionPolicy == PhysicsJointCollisionPolicy::DisableBetweenBodies) {
            const std::uint64_t key = CollisionPairKey(record.firstBody, record.secondBody);
            const auto insertion = std::ranges::lower_bound(canonical.scene.disabledJointCollisionPairs, key);
            if (insertion == canonical.scene.disabledJointCollisionPairs.end() || *insertion != key)
                canonical.scene.disabledJointCollisionPairs.insert(insertion, key);
        }
        canonical.native.system->AddConstraint(record.constraint.GetPtr());
        const auto identity = record.handle;
        canonical.scene.constraints.push_back(record);
        return Result<ConstraintHandle>::Success(identity);
    }

    /** @brief Cold unpublished receiver only; shares complete admission, exact endpoint locks and the canonical typed builder. */
    Result<CanonicalSceneConstraintRecord> PrepareCanonicalRestartConstraintRecord(CanonicalWorldHandle world, PhysicsWorldId owner,
                                                                                   const PhysicsConstraintDescriptor &descriptor,
                                                                                   const CanonicalConstraintCreation &creation) {
        auto bodies = AdmitConstraintRecord(world, owner, descriptor, {});
        if (bodies.HasError())
            return Result<CanonicalSceneConstraintRecord>::Failure(bodies.ErrorValue());
        return PrepareLockedConstraint(*static_cast<CanonicalWorld *>(world.value), owner, descriptor, bodies.Value(), &creation);
    }

    /** @copydoc DestroyCanonicalSceneConstraint */
    Result<void> DestroyCanonicalSceneConstraint(const CanonicalWorldHandle world, const ConstraintHandle constraint) {
        if (world.value == nullptr)
            return Result<void>::Failure(MakeError(PhysicsErrors::WorldInvalid));
        auto &canonical = *static_cast<CanonicalWorld *>(world.value);
        const auto found = std::ranges::find_if(canonical.scene.constraints, [constraint](const auto &record) {
            return record.handle == constraint;
        });
        if (found == canonical.scene.constraints.end())
            return Result<void>::Failure(MakeError(PhysicsErrors::HandleStale));
        const JPH::BodyID firstBody = found->firstBody;
        const JPH::BodyID secondBody = found->secondBody;
        const bool disabledCollision =
            secondBody.IsInvalid() == false && found->collisionPolicy == PhysicsJointCollisionPolicy::DisableBetweenBodies;
        canonical.native.system->RemoveConstraint(found->constraint.GetPtr());
        canonical.scene.constraints.erase(found);
        if (disabledCollision) {
            const bool stillDisabled = std::ranges::any_of(canonical.scene.constraints, [firstBody, secondBody](const auto &record) {
                return !record.secondBody.IsInvalid() && record.collisionPolicy == PhysicsJointCollisionPolicy::DisableBetweenBodies &&
                       CollisionPairKey(record.firstBody, record.secondBody) == CollisionPairKey(firstBody, secondBody);
            });
            if (!stillDisabled) {
                const std::uint64_t key = CollisionPairKey(firstBody, secondBody);
                const auto pair = std::ranges::lower_bound(canonical.scene.disabledJointCollisionPairs, key);
                if (pair != canonical.scene.disabledJointCollisionPairs.end() && *pair == key)
                    canonical.scene.disabledJointCollisionPairs.erase(pair);
            }
        }
        return Result<void>::Success();
    }

}  // namespace Horo::Physics::Detail
