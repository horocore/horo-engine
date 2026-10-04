#include "CanonicalSceneBodyBatchState.h"

namespace Horo::Physics::Detail {
    namespace {
        /** @brief Creates a detached compound from already staged children, preserving DAG and pose validation. */
        Result<JPH::Ref<JPH::Shape>> PrepareCompoundShape(const CanonicalSceneBodyBatchState &batch,
                                                          const std::vector<PhysicsSceneGroupShape::Child> &children) {
            if (children.empty() || children.size() > 256)
                return Result<JPH::Ref<JPH::Shape>>::Failure(MakeError(PhysicsErrors::DescriptorInvalid));
            JPH::StaticCompoundShapeSettings settings;
            for (const auto &child : children) {
                if (child.shape >= batch.shapes.size())
                    return Result<JPH::Ref<JPH::Shape>>::Failure(MakeError(PhysicsErrors::DescriptorInvalid));
                if (const auto valid = ValidatePhysicsPose(child.localPose); valid.HasError())
                    return Result<JPH::Ref<JPH::Shape>>::Failure(valid.ErrorValue());
                settings.AddShape(ToNative(child.localPose.translation), ToNative(child.localPose.rotation),
                                  batch.shapes[child.shape].shape.GetPtr());
            }
            auto created = settings.Create();
            if (created.HasError())
                return Result<JPH::Ref<JPH::Shape>>::Failure(MakeError(PhysicsErrors::ShapeArtifactInvalid));
            return Result<JPH::Ref<JPH::Shape>>::Success(created.Get());
        }

        /** @brief Constructs a private shape DAG without inserting any shape into the active world table. */
        [[nodiscard]] Result<void> PrepareGroupShapes(CanonicalSceneBodyBatchState &batch, const PhysicsWorldId owner,
                                                      const std::span<const PhysicsSceneGroupShape> inputs) {
            batch.shapes.reserve(inputs.size());
            batch.shapeHandles.reserve(inputs.size());
            for (const auto &input : inputs) {
                JPH::Ref<JPH::Shape> native;
                if (const auto *analytic = std::get_if<PhysicsShapeDescriptor>(&input.geometry)) {
                    if (const auto valid = ValidatePhysicsShapeDescriptor(*analytic); valid.HasError())
                        return valid;
                    auto created = CreateNativeShape(*analytic);
                    if (created.HasError())
                        return Result<void>::Failure(created.ErrorValue());
                    native = std::move(created).Value();
                } else {
                    auto created = PrepareCompoundShape(batch, std::get<std::vector<PhysicsSceneGroupShape::Child>>(input.geometry));
                    if (created.HasError())
                        return Result<void>::Failure(created.ErrorValue());
                    native = std::move(created).Value();
                }
                const ShapeHandle handle{owner, {batch.world->scene.nextShapeSlot++, 1}};
                batch.shapes.push_back({.handle = handle, .shape = std::move(native)});
                batch.shapeHandles.push_back(handle);
            }
            return Result<void>::Success();
        }

        /** @brief Tests exact rest without squaring away subnormal velocity components. */
        [[nodiscard]] bool IsZeroVelocity(const Math::Vec3 velocity) noexcept {
            return velocity.x == 0.0F && velocity.y == 0.0F && velocity.z == 0.0F;
        }

        /** @brief Rejects ambiguous dormant creation before any native allocation. */
        [[nodiscard]] Result<void> ValidateInitialBodyActivity(const PhysicsSceneBodyDescriptor &descriptor, const bool sleepingEnabled) {
            if (descriptor.initialActivity > PhysicsInitialBodyActivity::Sleeping)
                return Result<void>::Failure(MakeError(PhysicsErrors::OperationUnsupported, "Unknown initial body activity."));
            if (descriptor.initialActivity == PhysicsInitialBodyActivity::Sleeping) {
                if (descriptor.body.motion == PhysicsMotionType::Static)
                    return Result<void>::Failure(MakeError(PhysicsErrors::OperationUnsupported, "Static bodies cannot start sleeping."));
                if (!sleepingEnabled)
                    return Result<void>::Failure(MakeError(PhysicsErrors::OperationUnsupported, "World sleeping is disabled."));
                if (!IsZeroVelocity(descriptor.body.linearVelocity) || !IsZeroVelocity(descriptor.body.angularVelocity))
                    return Result<void>::Failure(
                        MakeError(PhysicsErrors::DescriptorInvalid, "A sleeping body must start with zero linear and angular velocity."));
            }
            return Result<void>::Success();
        }

        /** @brief Prepares native admission settings without allocating or activating a body. */
        [[nodiscard]] Result<JPH::BodyCreationSettings> PrepareSceneBodySettings(const PhysicsSceneBodyDescriptor &descriptor,
                                                                                 const CanonicalSceneShapeRecord &shape) {
            JPH::BodyCreationSettings settings(shape.shape.GetPtr(), ToNativePoint(descriptor.body.pose.translation),
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
            // Reserve native motion storage for future static -> moving safe-point transitions.
            settings.mAllowDynamicOrKinematic = !shape.shape->MustBeStatic();
            if (const Result<void> mass = ApplyCanonicalMassPolicy(settings, descriptor.body.mass); mass.HasError())
                return Result<JPH::BodyCreationSettings>::Failure(mass.ErrorValue());
            return Result<JPH::BodyCreationSettings>::Success(std::move(settings));
        }

        /** @brief Allocates detached bodies against staged or resident shapes; batch abort owns every admitted body. */
        Result<void> PrepareDetachedBodies(CanonicalSceneBodyBatchState &batch,
                                           const std::span<const PhysicsSceneBodyDescriptor> descriptors) {
            auto &canonical = *batch.world;
            for (const auto &descriptor : descriptors) {
                if (auto valid = ValidatePhysicsBodyDescriptor(descriptor.body, batch.owner); valid.HasError())
                    return Result<void>::Failure(valid.ErrorValue());
                if (auto valid = ValidateInitialBodyActivity(descriptor, canonical.native.system->GetPhysicsSettings().mAllowSleeping);
                    valid.HasError())
                    return Result<void>::Failure(valid.ErrorValue());
                const auto stagedShape = std::ranges::find(batch.shapes, descriptor.body.shape, &CanonicalSceneShapeRecord::handle);
                const auto residentShape =
                    std::ranges::find(canonical.scene.shapes, descriptor.body.shape, &CanonicalSceneShapeRecord::handle);
                const CanonicalSceneShapeRecord *shape{};
                if (stagedShape != batch.shapes.end())
                    shape = std::to_address(stagedShape);
                else if (residentShape != canonical.scene.shapes.end())
                    shape = std::to_address(residentShape);
                if (!shape)
                    return Result<void>::Failure(MakeError(PhysicsErrors::HandleStale));
                auto settings = PrepareSceneBodySettings(descriptor, *shape);
                if (settings.HasError())
                    return Result<void>::Failure(settings.ErrorValue());
                auto &bodyInterface = canonical.native.system->GetBodyInterface();
                const JPH::Body *body = bodyInterface.CreateBody(settings.Value());
                if (!body)
                    return Result<void>::Failure(MakeError(PhysicsErrors::CapacityExceeded));
                const auto nativeId = body->GetID();
                if (nativeId.GetIndex() >= canonical.query.nativeFixtureIndices.size()) {
                    bodyInterface.DestroyBody(nativeId);
                    return Result<void>::Failure(MakeError(PhysicsErrors::CapacityExceeded));
                }
                const BodyHandle identity{batch.owner, {canonical.scene.nextBodySlot++, 1}};
                batch.records.push_back({.handle = identity,
                                         .nativeBody = nativeId,
                                         .pose = descriptor.body.pose,
                                         .policy = descriptor.body,
                                         .motionStorageReserved = descriptor.body.motion != PhysicsMotionType::Static ||
                                                                  settings.Value().mAllowDynamicOrKinematic,
                                         .sceneEntity = descriptor.sceneEntity});
                batch.handles.push_back(identity);
                batch.nativeBodies.push_back(nativeId);
                if (descriptor.body.motion != PhysicsMotionType::Static && descriptor.initialActivity == PhysicsInitialBodyActivity::Awake)
                    batch.awakeBodies.push_back(nativeId);
            }
            return Result<void>::Success();
        }

    }  // namespace

    /** @copydoc ApplyCanonicalMassPolicy */
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

    /** @copydoc CreateCanonicalSceneBody */
    Result<BodyHandle> CreateCanonicalSceneBody(const CanonicalWorldHandle world, const PhysicsWorldId owner,
                                                const PhysicsSceneBodyDescriptor &descriptor) {
        if (world.value == nullptr || !owner.IsValid())
            return Result<BodyHandle>::Failure(MakeError(PhysicsErrors::WorldInvalid));
        auto &canonical = *static_cast<CanonicalWorld *>(world.value);
        if (!canonical.pendingBodyBatch.expired())
            return Result<BodyHandle>::Failure(MakeError(PhysicsErrors::InvalidState));
        if (canonical.scene.nextBodySlot == std::numeric_limits<std::uint32_t>::max() ||
            canonical.scene.bodies.size() >= canonical.scene.maximumBodies)
            return Result<BodyHandle>::Failure(MakeError(PhysicsErrors::CapacityExceeded));
        if (const Result<void> valid = ValidatePhysicsBodyDescriptor(descriptor.body, owner); valid.HasError())
            return Result<BodyHandle>::Failure(valid.ErrorValue());
        if (const auto activity = ValidateInitialBodyActivity(descriptor, canonical.native.system->GetPhysicsSettings().mAllowSleeping);
            activity.HasError())
            return Result<BodyHandle>::Failure(activity.ErrorValue());
        const auto shape = std::ranges::find_if(canonical.scene.shapes, [&descriptor](const auto &candidate) {
            return candidate.handle == descriptor.body.shape;
        });
        if (shape == canonical.scene.shapes.end())
            return Result<BodyHandle>::Failure(MakeError(PhysicsErrors::HandleStale));
        auto prepared = PrepareSceneBodySettings(descriptor, *shape);
        if (prepared.HasError())
            return Result<BodyHandle>::Failure(prepared.ErrorValue());
        const auto &settings = prepared.Value();

        const bool awake =
            descriptor.body.motion != PhysicsMotionType::Static && descriptor.initialActivity == PhysicsInitialBodyActivity::Awake;
        const JPH::EActivation activation = awake ? JPH::EActivation::Activate : JPH::EActivation::DontActivate;
        const JPH::BodyID nativeBody = canonical.native.system->GetBodyInterface().CreateAndAddBody(settings, activation);
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
            CanonicalSceneBodyRecord{.handle = identity,
                                     .nativeBody = nativeBody,
                                     .pose = descriptor.body.pose,
                                     .policy = descriptor.body,
                                     .motionStorageReserved =
                                         descriptor.body.motion != PhysicsMotionType::Static || settings.mAllowDynamicOrKinematic});
        return Result<BodyHandle>::Success(identity);
    }

    /** @copydoc PrepareCanonicalSceneBodies */
    Result<CanonicalSceneBodyBatch> PrepareCanonicalSceneBodies(const CanonicalWorldHandle world, const PhysicsWorldId owner,
                                                                std::span<const PhysicsSceneBodyDescriptor> descriptors,
                                                                const std::span<const PhysicsSceneGroupShape> shapes,
                                                                const std::span<const PhysicsSceneGroupBody> groupBodies) {
        using BatchResult = Result<CanonicalSceneBodyBatch>;
        if (!world.value || !owner.IsValid())
            return BatchResult::Failure(MakeError(PhysicsErrors::WorldInvalid));
        auto &canonical = *static_cast<CanonicalWorld *>(world.value);
        const auto bodyCount = groupBodies.empty() ? descriptors.size() : groupBodies.size();
        if (bodyCount > 256 || shapes.size() > 1024 || !canonical.pendingBodyBatch.expired() ||
            (!groupBodies.empty() && !descriptors.empty()))
            return BatchResult::Failure(MakeError(PhysicsErrors::InvalidState));
        if (bodyCount > canonical.scene.maximumBodies - canonical.scene.bodies.size() ||
            bodyCount > std::numeric_limits<std::uint32_t>::max() - canonical.scene.nextBodySlot ||
            shapes.size() > canonical.scene.maximumShapes - canonical.scene.shapes.size() ||
            shapes.size() > std::numeric_limits<std::uint32_t>::max() - canonical.scene.nextShapeSlot)
            return BatchResult::Failure(MakeError(PhysicsErrors::CapacityExceeded));
        auto prepared = std::make_shared<CanonicalSceneBodyBatchState>();
        prepared->records.reserve(bodyCount);
        prepared->handles.reserve(bodyCount);
        prepared->nativeBodies.reserve(bodyCount);
        prepared->awakeBodies.reserve(bodyCount);
        prepared->world = &canonical;
        prepared->owner = owner;
        canonical.pendingBodyBatch = prepared;
        if (const auto ready = PrepareGroupShapes(*prepared, owner, shapes); ready.HasError())
            return BatchResult::Failure(ready.ErrorValue());
        std::vector<PhysicsSceneBodyDescriptor> resolved;
        if (!groupBodies.empty()) {
            resolved.reserve(groupBodies.size());
            for (const auto &body : groupBodies) {
                if (body.shape >= prepared->shapes.size())
                    return BatchResult::Failure(MakeError(PhysicsErrors::DescriptorInvalid));
                resolved.push_back(body.descriptor);
                resolved.back().body.shape = prepared->shapes[body.shape].handle;
            }
            descriptors = resolved;
        }
        if (const auto ready = PrepareDetachedBodies(*prepared, descriptors); ready.HasError())
            return BatchResult::Failure(ready.ErrorValue());
        if (!descriptors.empty()) {
            prepared->addState = canonical.native.system->GetBodyInterface().AddBodiesPrepare(prepared->nativeBodies.data(),
                                                                                              static_cast<int>(descriptors.size()));
            prepared->broadphasePrepared = true;
        }
        return BatchResult::Success(CanonicalSceneBodyBatch{std::move(prepared)});
    }

}  // namespace Horo::Physics::Detail
