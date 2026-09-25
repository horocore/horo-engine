#include "CanonicalPhysicsRuntimeInternal.h"

#include <Jolt/Physics/Constraints/DistanceConstraint.h>
#include <Jolt/Physics/Constraints/FixedConstraint.h>
#include <limits>

namespace Horo::Physics::Detail {
    namespace {
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
                settings.AddShape(ToNative(instance.localPose.translation), ToNative(instance.localPose.rotation), child->shape.GetPtr());
            }
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ApplyCanonicalMassPolicy(JPH::BodyCreationSettings &settings, const PhysicsMassPolicy &mass) {
            if (const auto *explicitMass = std::get_if<PhysicsMass>(&mass); explicitMass != nullptr) {
                settings.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
                settings.mMassPropertiesOverride.mMass = explicitMass->kilograms;
                return Result<void>::Success();
            }
            const auto *density = std::get_if<PhysicsDensity>(&mass);
            if (density == nullptr)
                return Result<void>::Success();
            const float defaultMass = settings.GetMassProperties().mMass;
            if (!std::isfinite(defaultMass) || defaultMass <= 0.0F)
                return Result<void>::Failure(
                    MakeError(PhysicsErrors::DescriptorInvalid, "Canonical scene density could not derive a finite body mass."));
            settings.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
            settings.mMassPropertiesOverride.mMass = defaultMass * (density->kilogramsPerCubicMeter / 1'000.0F);
            return Result<void>::Success();
        }

        [[nodiscard]] Result<CanonicalConstraintBodies> ResolveCanonicalConstraintBodies(const CanonicalWorld &world,
                                                                                         const PhysicsWorldId owner,
                                                                                         const PhysicsConstraintDescriptor &descriptor) {
            if (const Result<void> valid = ValidatePhysicsConstraintDescriptor(descriptor, owner); valid.HasError())
                return Result<CanonicalConstraintBodies>::Failure(valid.ErrorValue());
            const auto *first = FindSceneBody(world, descriptor.first.body);
            if (first == nullptr)
                return Result<CanonicalConstraintBodies>::Failure(MakeError(PhysicsErrors::HandleStale));
            const auto *secondBody = std::get_if<PhysicsBodyAnchor>(&descriptor.second);
            const auto *second = secondBody == nullptr ? nullptr : FindSceneBody(world, secondBody->body);
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

        [[nodiscard]] Result<JPH::Ref<JPH::Constraint>> CreateNativeSceneConstraint(
            const JPH::BodyLockWrite &firstLock, const JPH::BodyLockWrite &secondLock, const CanonicalSceneBodyRecord *second,
            const PhysicsPose &firstFrame, const PhysicsPose &secondFrame,
            const std::variant<PhysicsFixedConstraint, PhysicsDistanceConstraint> &parameters) {
            JPH::Body &body1 = firstLock.GetBody();
            JPH::Body &body2 = second != nullptr ? secondLock.GetBody() : JPH::Body::sFixedToWorld;
            JPH::Ref<JPH::Constraint> nativeConstraint = std::visit([&]<typename Parameter>(const Parameter &value) {
                using ParameterType = std::decay_t<Parameter>;
                if constexpr (std::is_same_v<ParameterType, PhysicsFixedConstraint>)
                    return CreateNativeFixedConstraint(body1, body2, firstFrame, secondFrame);
                else
                    return CreateNativeDistanceConstraint(body1, body2, firstFrame, secondFrame, value);
            }, parameters);
            if (nativeConstraint == nullptr)
                return Result<JPH::Ref<JPH::Constraint>>::Failure(
                    MakeError(PhysicsErrors::OperationUnsupported, "Canonical solver rejected the scene constraint admission."));
            return Result<JPH::Ref<JPH::Constraint>>::Success(std::move(nativeConstraint));
        }
    }  // namespace

    /** @copydoc CreateCanonicalSceneShape */
    Result<ShapeHandle> CreateCanonicalSceneShape(const CanonicalWorldHandle world, const PhysicsWorldId owner,
                                                  const PhysicsShapeDescriptor &descriptor) {
        if (world.value == nullptr || !owner.IsValid())
            return Result<ShapeHandle>::Failure(MakeError(PhysicsErrors::WorldInvalid));
        auto &canonical = *static_cast<CanonicalWorld *>(world.value);
        if (canonical.scene.nextShapeSlot == std::numeric_limits<std::uint32_t>::max() ||
            canonical.scene.shapes.size() >= canonical.scene.maximumShapes)
            return Result<ShapeHandle>::Failure(MakeError(PhysicsErrors::CapacityExceeded));
        if (const Result<void> valid = ValidatePhysicsShapeDescriptor(descriptor); valid.HasError())
            return Result<ShapeHandle>::Failure(valid.ErrorValue());
        const Result<JPH::Ref<JPH::Shape>> nativeShape = CreateNativeShape(descriptor);
        if (nativeShape.HasError())
            return Result<ShapeHandle>::Failure(nativeShape.ErrorValue());

        const std::uint32_t slot = canonical.scene.nextShapeSlot++;
        const ShapeHandle identity{owner, {slot, 1}};
        canonical.scene.shapes.emplace_back(CanonicalSceneShapeRecord{.handle = identity, .shape = nativeShape.Value()});
        return Result<ShapeHandle>::Success(identity);
    }

    /** @copydoc CreateCanonicalSceneCompoundShape */
    Result<ShapeHandle> CreateCanonicalSceneCompoundShape(const CanonicalWorldHandle world, const PhysicsWorldId owner,
                                                          const std::span<const PhysicsSceneShapeInstance> instances) {
        if (world.value == nullptr || !owner.IsValid())
            return Result<ShapeHandle>::Failure(MakeError(PhysicsErrors::WorldInvalid));
        if (instances.empty())
            return Result<ShapeHandle>::Failure(
                MakeError(PhysicsErrors::DescriptorInvalid, "A canonical scene compound requires at least one child shape."));
        auto &canonical = *static_cast<CanonicalWorld *>(world.value);
        if (canonical.scene.nextShapeSlot == std::numeric_limits<std::uint32_t>::max() ||
            canonical.scene.shapes.size() >= canonical.scene.maximumShapes)
            return Result<ShapeHandle>::Failure(MakeError(PhysicsErrors::CapacityExceeded));

        JPH::StaticCompoundShapeSettings settings;
        if (const Result<void> children = AddCanonicalCompoundChildren(canonical, owner, instances, settings); children.HasError())
            return Result<ShapeHandle>::Failure(children.ErrorValue());
        const JPH::ShapeSettings::ShapeResult created = settings.Create();
        if (created.HasError())
            return Result<ShapeHandle>::Failure(
                MakeError(PhysicsErrors::ShapeArtifactInvalid, "Canonical solver rejected the scene compound shape."));

        const std::uint32_t slot = canonical.scene.nextShapeSlot++;
        const ShapeHandle identity{owner, {slot, 1}};
        canonical.scene.shapes.emplace_back(CanonicalSceneShapeRecord{.handle = identity, .shape = created.Get()});
        return Result<ShapeHandle>::Success(identity);
    }

    /** @copydoc CreateCanonicalSceneBody */
    Result<BodyHandle> CreateCanonicalSceneBody(const CanonicalWorldHandle world, const PhysicsWorldId owner,
                                                const PhysicsSceneBodyDescriptor &descriptor) {
        if (world.value == nullptr || !owner.IsValid())
            return Result<BodyHandle>::Failure(MakeError(PhysicsErrors::WorldInvalid));
        auto &canonical = *static_cast<CanonicalWorld *>(world.value);
        if (canonical.scene.nextBodySlot == std::numeric_limits<std::uint32_t>::max() ||
            canonical.scene.bodies.size() >= canonical.scene.maximumBodies)
            return Result<BodyHandle>::Failure(MakeError(PhysicsErrors::CapacityExceeded));
        if (const Result<void> valid = ValidatePhysicsBodyDescriptor(descriptor.body, owner); valid.HasError())
            return Result<BodyHandle>::Failure(valid.ErrorValue());
        const auto *shape = FindSceneShape(canonical, descriptor.body.shape);
        if (shape == nullptr)
            return Result<BodyHandle>::Failure(MakeError(PhysicsErrors::HandleStale));

        JPH::BodyCreationSettings settings(shape->shape.GetPtr(), ToNativePoint(descriptor.body.pose.translation),
                                           ToNative(descriptor.body.pose.rotation), ToNativeMotion(descriptor.body.motion),
                                           JPH::ObjectLayer{0});
        settings.mLinearVelocity = ToNative(descriptor.body.linearVelocity);
        settings.mAngularVelocity = ToNative(descriptor.body.angularVelocity);
        settings.mLinearDamping = descriptor.body.motionSafety.linearDampingPerSecond;
        settings.mAngularDamping = descriptor.body.motionSafety.angularDampingPerSecond;
        settings.mMaxLinearVelocity = descriptor.body.motionSafety.maximumLinearSpeed;
        settings.mMaxAngularVelocity = descriptor.body.motionSafety.maximumAngularSpeed;
        settings.mAllowedDOFs = ToNativeAllowedDOFs(descriptor.body.motionSafety.lockedAxes);
        settings.mIsSensor = descriptor.sensor;

        if (const Result<void> mass = ApplyCanonicalMassPolicy(settings, descriptor.body.mass); mass.HasError())
            return Result<BodyHandle>::Failure(mass.ErrorValue());

        const JPH::BodyID nativeBody =
            canonical.native.system->GetBodyInterface().CreateAndAddBody(settings, JPH::EActivation::DontActivate);
        if (nativeBody.IsInvalid())
            return Result<BodyHandle>::Failure(
                MakeError(PhysicsErrors::CapacityExceeded, "Canonical solver rejected the scene body admission."));
        if (nativeBody.GetIndex() >= canonical.query.nativeFixtureIndices.size()) {
            canonical.native.system->GetBodyInterface().RemoveBody(nativeBody);
            canonical.native.system->GetBodyInterface().DestroyBody(nativeBody);
            return Result<BodyHandle>::Failure(MakeError(PhysicsErrors::CapacityExceeded));
        }

        const std::uint32_t slot = canonical.scene.nextBodySlot++;
        const BodyHandle identity{owner, {slot, 1}};
        canonical.scene.bodies.emplace_back(
            CanonicalSceneBodyRecord{.handle = identity, .nativeBody = nativeBody, .pose = descriptor.body.pose});
        return Result<BodyHandle>::Success(identity);
    }

    /** @copydoc CreateCanonicalSceneConstraint */
    Result<ConstraintHandle> CreateCanonicalSceneConstraint(const CanonicalWorldHandle world, const PhysicsWorldId owner,
                                                            const PhysicsConstraintDescriptor &descriptor) {
        if (world.value == nullptr || !owner.IsValid())
            return Result<ConstraintHandle>::Failure(MakeError(PhysicsErrors::WorldInvalid));
        auto &canonical = *static_cast<CanonicalWorld *>(world.value);
        if (!HasCanonicalConstraintCapacity(canonical))
            return Result<ConstraintHandle>::Failure(MakeError(PhysicsErrors::CapacityExceeded));
        const Result<CanonicalConstraintBodies> bodies = ResolveCanonicalConstraintBodies(canonical, owner, descriptor);
        if (bodies.HasError())
            return Result<ConstraintHandle>::Failure(bodies.ErrorValue());
        const CanonicalSceneBodyRecord *first = bodies.Value().first;
        const CanonicalSceneBodyRecord *second = bodies.Value().second;
        const auto *secondBody = std::get_if<PhysicsBodyAnchor>(&descriptor.second);

        JPH::BodyLockWrite firstLock(canonical.native.system->GetBodyLockInterfaceNoLock(), first->nativeBody);
        JPH::BodyLockWrite secondLock(canonical.native.system->GetBodyLockInterfaceNoLock(),
                                      second != nullptr ? second->nativeBody : JPH::BodyID{});
        if (const Result<void> locks = ValidateCanonicalConstraintLocks(firstLock, secondLock, second); locks.HasError())
            return Result<ConstraintHandle>::Failure(locks.ErrorValue());

        const PhysicsPose firstFrame = ComposePhysicsPose(first->pose, descriptor.first.localFrame);
        const PhysicsPose secondFrame = second != nullptr ? ComposePhysicsPose(second->pose, secondBody->localFrame)
                                                          : std::get<PhysicsWorldAnchor>(descriptor.second).frame;
        const Result<JPH::Ref<JPH::Constraint>> nativeConstraint =
            CreateNativeSceneConstraint(firstLock, secondLock, second, firstFrame, secondFrame, descriptor.parameters);
        if (nativeConstraint.HasError())
            return Result<ConstraintHandle>::Failure(nativeConstraint.ErrorValue());

        canonical.native.system->AddConstraint(nativeConstraint.Value().GetPtr());
        const std::uint32_t slot = canonical.scene.nextConstraintSlot++;
        const ConstraintHandle identity{owner, {slot, 1}};
        canonical.scene.constraints.emplace_back(
            CanonicalSceneConstraintRecord{.handle = identity, .constraint = nativeConstraint.Value(), .descriptor = descriptor});
        return Result<ConstraintHandle>::Success(identity);
    }

    /** @copydoc ProjectCanonicalDebug */
    CanonicalDebugProjection ProjectCanonicalDebug(const CanonicalWorldHandle world, const PhysicsDebugBudget &budget) {
        CanonicalDebugProjection projected;
        if (world.value == nullptr)
            return projected;
        const auto &canonical = *static_cast<const CanonicalWorld *>(world.value);
        const auto capacity = [&budget](const PhysicsDebugCategory category) {
            const auto &limit = budget.categories[static_cast<std::size_t>(category)];
            return std::min<std::size_t>({limit.maximumRecords, limit.maximumPayloadBytes / sizeof(PhysicsDebugRecord),
                                          budget.maximumPayloadBytes / sizeof(PhysicsDebugRecord), MaximumPhysicsDebugRecords});
        };
        const std::size_t bodyLimit = capacity(PhysicsDebugCategory::Body);
        projected.bodies.reserve(std::min(bodyLimit, canonical.scene.bodies.size() + canonical.query.fixtures.size()));
        for (std::size_t index = 0; index < canonical.scene.bodies.size() && projected.bodies.size() < bodyLimit; ++index)
            projected.bodies.emplace_back(PhysicsDebugBody{canonical.scene.bodies[index].handle});
        for (std::size_t index = 0; index < canonical.query.fixtures.size() && projected.bodies.size() < bodyLimit; ++index)
            projected.bodies.emplace_back(PhysicsDebugBody{canonical.query.fixtures[index].fixture.body});
        projected.truncatedBodies = canonical.scene.bodies.size() + canonical.query.fixtures.size() - projected.bodies.size();

        const std::size_t shapeLimit = capacity(PhysicsDebugCategory::Shape);
        projected.shapes.reserve(std::min(shapeLimit, canonical.scene.shapes.size() + canonical.query.fixtures.size()));
        for (std::size_t index = 0; index < canonical.scene.shapes.size() && projected.shapes.size() < shapeLimit; ++index)
            projected.shapes.emplace_back(PhysicsDebugShape{canonical.scene.shapes[index].handle, {}});
        for (std::size_t index = 0; index < canonical.query.fixtures.size() && projected.shapes.size() < shapeLimit; ++index)
            projected.shapes.emplace_back(
                PhysicsDebugShape{canonical.query.fixtures[index].fixture.shape, canonical.query.fixtures[index].fixture.body});
        projected.truncatedShapes = canonical.scene.shapes.size() + canonical.query.fixtures.size() - projected.shapes.size();

        const std::size_t constraintLimit = capacity(PhysicsDebugCategory::Constraint);
        projected.constraints.reserve(std::min(constraintLimit, canonical.scene.constraints.size()));
        for (std::size_t index = 0; index < canonical.scene.constraints.size() && projected.constraints.size() < constraintLimit; ++index) {
            const auto &record = canonical.scene.constraints[index];
            const auto *second = std::get_if<PhysicsBodyAnchor>(&record.descriptor.second);
            projected.constraints.emplace_back(
                PhysicsDebugConstraint{record.handle, record.descriptor.first.body, second != nullptr ? second->body : BodyHandle{}});
        }
        projected.truncatedConstraints = canonical.scene.constraints.size() - projected.constraints.size();
        return projected;
    }
}  // namespace Horo::Physics::Detail
