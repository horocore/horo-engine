#include "CanonicalPhysicsStateBodyControl.h"
#include "CanonicalPhysicsStateShapeCreation.h"
#include "CanonicalPhysicsStateShapeRecipe.h"
#include "CanonicalSceneBodyBatchState.h"
#include "PhysicsStateConstructionNative.h"

namespace Horo::Physics::Detail {
    namespace {
        /** @brief Creates a detached compound from already staged children, preserving
         * DAG and pose validation. */
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
                                  batch.shapes[child.shape].shape.GetPtr(), static_cast<JPH::uint32>(&child - children.data() + 1));
            }
            auto created = settings.Create();
            if (created.HasError())
                return Result<JPH::Ref<JPH::Shape>>::Failure(MakeError(PhysicsErrors::ShapeArtifactInvalid));
            return Result<JPH::Ref<JPH::Shape>>::Success(created.Get());
        }

        /** @brief Retains exact source transforms before native compound COM
         * normalization or child reordering. */
        std::vector<PhysicsSceneShapeInstance> CaptureChildren(const CanonicalSceneBodyBatchState &batch,
                                                               const PhysicsSceneGroupShape &input) {
            std::vector<PhysicsSceneShapeInstance> poses;
            const auto *children = std::get_if<std::vector<PhysicsSceneGroupShape::Child>>(&input.geometry);
            if (!children)
                return poses;
            poses.reserve(children->size());
            for (const auto &child : *children)
                poses.push_back({batch.shapes[child.shape].handle, child.localPose});
            return poses;
        }

        /** @brief The actual ordered native DAG inputs are hashed before a child record
         * can be independently retired. */
        Sha256Digest GroupShapeRecipe(const CanonicalSceneBodyBatchState &batch, const PhysicsSceneGroupShape &input) noexcept {
            if (const auto *analytic = std::get_if<PhysicsShapeDescriptor>(&input.geometry))
                return CanonicalPrimitiveRecipe(*analytic);
            const auto &children = std::get<std::vector<PhysicsSceneGroupShape::Child>>(input.geometry);
            auto hash = BeginCanonicalCompoundRecipe(children.size());
            for (const auto &child : children) {
                const auto &recipe = batch.shapes[child.shape].recipe;
                if (!HasCanonicalShapeRecipe(recipe))
                    return {};
                hash.Digest(recipe);
                hash.Pose(child.localPose);
            }
            return hash.Finish();
        }

        /** @brief Existing native builders remain the creation authority; the observer
         * records actual owner-thread geometry requests. */
        Result<CanonicalPhysicsStateCreatedShape> PrepareGroupNativeShape(CanonicalSceneBodyBatchState &batch,
                                                                          const PhysicsSceneGroupShape &input) {
            using Outcome = Result<CanonicalPhysicsStateCreatedShape>;
            if (const auto *analytic = std::get_if<PhysicsShapeDescriptor>(&input.geometry)) {
                if (const auto valid = ValidatePhysicsShapeDescriptor(*analytic); valid.HasError())
                    return Outcome::Failure(valid.ErrorValue());
                return ObserveCanonicalPhysicsShapeCreation([&] {
                    return CreateNativeShape(*analytic);
                });
            }
            const auto &children = std::get<std::vector<PhysicsSceneGroupShape::Child>>(input.geometry);
            auto created = ObserveCanonicalPhysicsShapeCreation([&] {
                return PrepareCompoundShape(batch, children);
            });
            if (created.HasError())
                return created;
            auto retained = std::move(created).Value();
            for (const auto &child : children)
                if (!AddCanonicalPhysicsShapeReservation(batch.shapes[child.shape].nativeRequestedBytes, retained.nativeRequestedBytes))
                    return Outcome::Failure(MakeError(PhysicsErrors::CapacityExceeded));
            return Outcome::Success(std::move(retained));
        }

        /** @brief Validates recipe provenance before native construction and retains only actual matching evidence. */
        Result<CanonicalSceneShapeRecord> PrepareGroupShape(CanonicalSceneBodyBatchState &batch, PhysicsWorldId owner,
                                                            const PhysicsSceneGroupShape &input,
                                                            const StateDetail::ConstructionRecipeLease *recipe) {
            using Outcome = Result<CanonicalSceneShapeRecord>;
            auto *pool = batch.world->owner.constructionRecipes;
            if (auto admitted = ValidateCanonicalRecipeAdmission(pool, recipe); admitted.HasError())
                return Outcome::Failure(admitted.ErrorValue());
            auto created = PrepareGroupNativeShape(batch, input);
            if (created.HasError())
                return Outcome::Failure(created.ErrorValue());
            auto retained = std::move(created).Value();
            const auto digest = BindCanonicalShapeCreationRecipe(GroupShapeRecipe(batch, input), retained.creationControl);
            auto witness = ValidateCanonicalCreatedRecipe(pool, recipe, retained.creationControl, digest);
            if (witness.HasError())
                return Outcome::Failure(witness.ErrorValue());
            const ShapeHandle handle{owner, {batch.world->nextResourceSlot++, 1}};
            const auto *analytic = std::get_if<PhysicsShapeDescriptor>(&input.geometry);
            return Outcome::Success({.handle = handle,
                                     .shape = std::move(retained.shape),
                                     .nativeRequestedBytes = retained.nativeRequestedBytes,
                                     .construction = std::move(witness).Value(),
                                     .recipe = digest,
                                     .analytic = analytic ? std::optional{*analytic} : std::nullopt,
                                     .compoundChildren = CaptureChildren(batch, input)});
        }

        /** @brief Constructs a private shape DAG without inserting any shape into the active world table. */
        Result<void> PrepareGroupShapes(CanonicalSceneBodyBatchState &batch, const PhysicsWorldId owner,
                                        const std::span<const PhysicsSceneGroupShape> inputs,
                                        const std::span<const StateDetail::ConstructionRecipeLease> recipes) {
            if (!recipes.empty() && recipes.size() != inputs.size())
                return Result<void>::Failure(MakeError(PhysicsErrors::DescriptorInvalid));
            batch.shapes.reserve(inputs.size());
            batch.shapeHandles.reserve(inputs.size());
            for (std::size_t index = 0; index < inputs.size(); ++index) {
                const auto *recipe = recipes.empty() ? nullptr : &recipes[index];
                auto created = PrepareGroupShape(batch, owner, inputs[index], recipe);
                if (created.HasError())
                    return Result<void>::Failure(created.ErrorValue());
                auto retained = std::move(created).Value();
                batch.shapeHandles.push_back(retained.handle);
                batch.shapes.push_back(std::move(retained));
            }
            return Result<void>::Success();
        }

        /** @brief Tests exact rest without squaring away subnormal velocity components.
         */
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

        /** @brief Validates bounded copied collider evidence without allocating native
         * resources. */
        [[nodiscard]] Result<void> ValidateColliderEvidence(const PhysicsSceneCollisionBinding &binding) {
            if (binding.colliders.size() > 256)
                return Result<void>::Failure(MakeError(PhysicsErrors::CapacityExceeded));
            for (const auto &collider : binding.colliders) {
                if (collider.subshape && !collider.subshape->IsValid())
                    return Result<void>::Failure(MakeError(PhysicsErrors::DescriptorInvalid));
                if (const auto pose = ValidatePhysicsPose(collider.localPose); pose.HasError())
                    return Result<void>::Failure(pose.ErrorValue());
                if (collider.materials.size() > 64)
                    return Result<void>::Failure(MakeError(PhysicsErrors::CapacityExceeded));
                for (const auto &material : collider.materials)
                    if (!material.asset.IsValid() || material.assetGeneration == 0 || !material.slot.IsValid())
                        return Result<void>::Failure(MakeError(PhysicsErrors::MaterialDescriptorInvalid));
            }
            return Result<void>::Success();
        }

        /** @brief Compound metadata must describe the exact admitted native
         * contributors, never a guessed first child. */
        Result<void> ValidateColliderTopology(const PhysicsSceneCollisionBinding &binding, const JPH::Shape &shape) {
            if (binding.colliders.empty())
                return Result<void>::Success();
            const auto count =
                shape.GetType() == JPH::EShapeType::Compound ? static_cast<const JPH::CompoundShape &>(shape).GetNumSubShapes() : 1;
            if (count != binding.colliders.size())
                return Result<void>::Failure(MakeError(PhysicsErrors::DescriptorInvalid));
            return Result<void>::Success();
        }

        /** @brief Explicit authored compound identities require the exact construction
         * pose and unique owner slot. */
        Result<void> ValidateAuthoredCompoundMapping(const PhysicsSceneCollisionBinding &binding, const CanonicalSceneShapeRecord &shape) {
            if (shape.compoundChildren.empty())
                return Result<void>::Success();
            if (shape.compoundChildren.size() != binding.colliders.size())
                return Result<void>::Failure(MakeError(PhysicsErrors::DescriptorInvalid));
            for (std::size_t index = 0; index < binding.colliders.size(); ++index) {
                const auto &collider = binding.colliders[index];
                if (collider.collider == 0)
                    continue;
                if (collider.localPose != shape.compoundChildren[index].localPose)
                    return Result<void>::Failure(MakeError(PhysicsErrors::DescriptorInvalid));
                for (std::size_t previous = 0; previous < index; ++previous)
                    if (binding.colliders[previous].collider == collider.collider)
                        return Result<void>::Failure(MakeError(PhysicsErrors::DescriptorInvalid));
            }
            return Result<void>::Success();
        }

        /** @brief Validate copied semantic evidence against the captured world before
         * allocating native bodies. */
        Result<void> ValidateColliderSurfaces(const PhysicsSceneCollisionBinding &collision, const PhysicsSurfaceWorldBinding &binding) {
            for (const auto &collider : collision.colliders) {
                if (!collider.surface)
                    continue;
                if (collider.materials.size() != 1 || collider.surface->collider != collider.collider ||
                    !MatchesPhysicsSurfaceEvidence(binding, *collider.surface))
                    return Result<void>::Failure(MakeError(SurfaceErrors::Stale));
            }
            return Result<void>::Success();
        }

        /** @brief Complete copied collider admission before any native shape/body
         * publication. */
        Result<void> ValidateSceneColliderMetadata(const CanonicalWorld &world, const PhysicsSceneCollisionBinding &binding,
                                                   const CanonicalSceneShapeRecord &shape) {
            if (auto physical = ValidateColliderEvidence(binding); physical.HasError())
                return physical;
            if (auto topology = ValidateColliderTopology(binding, *shape.shape.GetPtr()); topology.HasError())
                return topology;
            if (auto authored = ValidateAuthoredCompoundMapping(binding, shape); authored.HasError())
                return authored;
            return ValidateColliderSurfaces(binding, world.simulation.binding.surfaces);
        }

        /** @brief Resolves one exact captured profile and its motion eligibility before
         * native allocation. */
        [[nodiscard]] Result<JPH::ObjectLayer> ResolveSceneObjectLayer(const CanonicalWorld &world,
                                                                       const PhysicsSceneBodyDescriptor &descriptor,
                                                                       const CanonicalSceneShapeRecord &shape) {
            if (!descriptor.collision.has_value()) {
                if (const auto requested = descriptor.body.continuousCollision.mode.value_or(world.continuousCollision.policy.defaultMode);
                    requested == PhysicsDefaultMotionQuality::LinearCast && descriptor.body.motion == PhysicsMotionType::Dynamic &&
                    !descriptor.sensor)
                    return Result<JPH::ObjectLayer>::Failure(
                        MakeError(PhysicsErrors::CapabilityUnavailable,
                                  "LinearCast requires an installed collision schema and an exact body profile."));
                return Result<JPH::ObjectLayer>::Success(JPH::ObjectLayer{0});
            }
            if (auto metadata = ValidateSceneColliderMetadata(world, *descriptor.collision, shape); metadata.HasError())
                return Result<JPH::ObjectLayer>::Failure(std::move(metadata).ErrorValue());
            if (!world.simulation.binding.schema)
                return Result<JPH::ObjectLayer>::Failure(MakeError(PhysicsErrors::CapabilityUnavailable));
            const auto profile = world.simulation.binding.schema->ResolveProfile(descriptor.collision->profile);
            if (profile.HasError())
                return Result<JPH::ObjectLayer>::Failure(profile.ErrorValue());
            const auto layers = std::span{world.simulation.layers.data(), world.simulation.layerCount};
            const auto layer = std::ranges::find(layers, profile.Value()->layer, &CollisionLayerDefinition::id);
            if (layer == layers.end())
                return Result<JPH::ObjectLayer>::Failure(MakeError(PhysicsErrors::DescriptorInvalid));
            if (const bool admitted = CanonicalLayerAdmitsMotion(*layer, descriptor.body.motion);
                !admitted || (descriptor.sensor && !layer->admitsOverlap))
                return Result<JPH::ObjectLayer>::Failure(MakeError(PhysicsErrors::OperationUnsupported));
            if (!profile.Value()->simulationEnabled) {
                if (const auto requested = descriptor.body.continuousCollision.mode.value_or(world.continuousCollision.policy.defaultMode);
                    requested == PhysicsDefaultMotionQuality::LinearCast && descriptor.body.motion == PhysicsMotionType::Dynamic &&
                    !descriptor.sensor)
                    return Result<JPH::ObjectLayer>::Failure(MakeError(PhysicsErrors::CapabilityUnavailable));
                return Result<JPH::ObjectLayer>::Success(JPH::ObjectLayer{0});
            }
            return Result<JPH::ObjectLayer>::Success(static_cast<JPH::ObjectLayer>(std::distance(layers.begin(), layer) + 1));
        }

        /** @brief Prepares native admission settings without allocating or activating a
         * body. */
        [[nodiscard]] Result<JPH::BodyCreationSettings> PrepareSceneBodySettings(const PhysicsSceneBodyDescriptor &descriptor,
                                                                                 const CanonicalSceneShapeRecord &shape,
                                                                                 const CanonicalWorld &world) {
            if (shape.shape->MustBeStatic() && descriptor.body.motion != PhysicsMotionType::Static)
                return Result<JPH::BodyCreationSettings>::Failure(MakeError(PhysicsErrors::OperationUnsupported));
            const auto quality =
                ResolveCanonicalMotionQuality(world.continuousCollision.policy, descriptor.body, *shape.shape, descriptor.sensor);
            if (quality.HasError())
                return Result<JPH::BodyCreationSettings>::Failure(quality.ErrorValue());
            const auto layer = ResolveSceneObjectLayer(world, descriptor, shape);
            if (layer.HasError())
                return Result<JPH::BodyCreationSettings>::Failure(layer.ErrorValue());
            JPH::BodyCreationSettings settings(shape.shape.GetPtr(), ToNativePoint(descriptor.body.pose.translation),
                                               ToNative(descriptor.body.pose.rotation), ToNativeMotion(descriptor.body.motion),
                                               layer.Value());
            settings.mLinearVelocity = ToNative(descriptor.body.linearVelocity);
            settings.mAngularVelocity = ToNative(descriptor.body.angularVelocity);
            settings.mLinearDamping = descriptor.body.motionSafety.linearDampingPerSecond;
            settings.mAngularDamping = descriptor.body.motionSafety.angularDampingPerSecond;
            settings.mMaxLinearVelocity = descriptor.body.motionSafety.maximumLinearSpeed;
            settings.mMaxAngularVelocity = descriptor.body.motionSafety.maximumAngularSpeed;
            settings.mAllowedDOFs = ToNativeAllowedDOFs(descriptor.body.motionSafety.lockedAxes);
            settings.mIsSensor = descriptor.sensor;
            settings.mMotionQuality = quality.Value();
            // Reserve native motion storage for future static -> moving safe-point
            // transitions.
            settings.mAllowDynamicOrKinematic = !shape.shape->MustBeStatic();
            if (const Result<void> mass = ApplyCanonicalMassPolicy(settings, descriptor.body.mass); mass.HasError())
                return Result<JPH::BodyCreationSettings>::Failure(mass.ErrorValue());
            return Result<JPH::BodyCreationSettings>::Success(std::move(settings));
        }

        /** @brief Same ordered descriptor/activity admission for resident and detached
         * body paths. */
        Result<void> ValidateSceneBodyRequest(const CanonicalWorld &world, PhysicsWorldId owner,
                                              const PhysicsSceneBodyDescriptor &descriptor) {
            if (auto valid = ValidatePhysicsBodyDescriptor(descriptor.body, owner); valid.HasError())
                return valid;
            return ValidateInitialBodyActivity(descriptor, world.native.system->GetPhysicsSettings().mAllowSleeping);
        }

        /** @brief Private staged shapes take precedence over the resident table exactly
         * as the original batch path. */
        const CanonicalSceneShapeRecord *DetachedShape(const CanonicalSceneBodyBatchState &batch, ShapeHandle handle) noexcept {
            const auto staged = std::ranges::find(batch.shapes, handle, &CanonicalSceneShapeRecord::handle);
            if (staged != batch.shapes.end())
                return std::to_address(staged);
            const auto &resident = batch.world->scene.shapes;
            const auto found = std::ranges::find(resident, handle, &CanonicalSceneShapeRecord::handle);
            return found != resident.end() ? std::to_address(found) : nullptr;
        }

        /** @brief Captures the actual admitted policy and retained shape witness once
         * for both body construction paths. */
        CanonicalSceneBodyRecord BodyRecord(const PhysicsSceneBodyDescriptor &descriptor, const CanonicalSceneShapeRecord &shape,
                                            BodyHandle identity, JPH::BodyID native, const JPH::BodyCreationSettings &settings,
                                            std::optional<PhysicsSceneCollisionBinding> collision,
                                            std::optional<PhysicsStateFpControl> control) {
            return {.handle = identity,
                    .nativeBody = native,
                    .pose = descriptor.body.pose,
                    .policy = descriptor.body,
                    .sensor = descriptor.sensor,
                    .motionStorageReserved = descriptor.body.motion != PhysicsMotionType::Static || settings.mAllowDynamicOrKinematic,
                    .sceneEntity = descriptor.sceneEntity,
                    .collision = std::move(collision),
                    .shapeEvidence = shape.shape,
                    .shapeNativeRequestedBytes = shape.nativeRequestedBytes,
                    .shapeRecipe = shape.recipe,
                    .shapeConstruction = shape.construction,
                    .creationControl = control,
                    .stateArithmeticControl = control,
                    .massArithmeticSubprofile = CanonicalBodyMassSubprofile(control)};
        }

        /** @brief Static bodies never enter the active list, preserving the existing
         * initial activity policy. */
        bool InitiallyAwake(const PhysicsSceneBodyDescriptor &descriptor) noexcept {
            return descriptor.body.motion != PhysicsMotionType::Static && descriptor.initialActivity == PhysicsInitialBodyActivity::Awake;
        }

        struct ResidentBodyCreation final {
            const CanonicalSceneShapeRecord *shape{};
            JPH::BodyCreationSettings settings;
            std::optional<PhysicsSceneCollisionBinding> collision;
            std::optional<PhysicsStateFpControl> beforeControl;
        };

        /** @brief All resident admission/copies finish before any native body is
         * allocated or added. */
        Result<ResidentBodyCreation> PrepareResidentBody(CanonicalWorld &world, PhysicsWorldId owner,
                                                         const PhysicsSceneBodyDescriptor &descriptor) {
            using Outcome = Result<ResidentBodyCreation>;
            if (!world.pendingBodyBatch.expired())
                return Outcome::Failure(MakeError(PhysicsErrors::InvalidState));
            if (world.nextResourceSlot == std::numeric_limits<std::uint32_t>::max() ||
                world.scene.bodies.size() >= world.scene.maximumBodies)
                return Outcome::Failure(MakeError(PhysicsErrors::CapacityExceeded));
            if (auto valid = ValidateSceneBodyRequest(world, owner, descriptor); valid.HasError())
                return Outcome::Failure(valid.ErrorValue());
            const auto shape = std::ranges::find(world.scene.shapes, descriptor.body.shape, &CanonicalSceneShapeRecord::handle);
            if (shape == world.scene.shapes.end())
                return Outcome::Failure(MakeError(PhysicsErrors::HandleStale));
            const auto before = ReadCanonicalBodyControl();
            if (auto admitted = AdmitCanonicalBodyControl(world.owner.constructionRecipes, before); admitted.HasError())
                return Outcome::Failure(admitted.ErrorValue());
            auto settings = PrepareSceneBodySettings(descriptor, *shape, world);
            if (settings.HasError())
                return Outcome::Failure(settings.ErrorValue());
            return Outcome::Success({std::to_address(shape), std::move(settings).Value(), descriptor.collision, before});
        }

        struct DetachedBodyCreation final {
            JPH::BodyID body;
            JPH::BodyCreationSettings settings;
            std::optional<PhysicsStateFpControl> control;
        };

        /** @brief All control/settings admission precedes allocation; rejected private bodies are destroyed before publication. */
        Result<DetachedBodyCreation> CreateDetachedBody(CanonicalWorld &world, const PhysicsSceneBodyDescriptor &descriptor,
                                                        const CanonicalSceneShapeRecord &shape) {
            using Outcome = Result<DetachedBodyCreation>;
            const auto before = ReadCanonicalBodyControl();
            if (auto admitted = AdmitCanonicalBodyControl(world.owner.constructionRecipes, before); admitted.HasError())
                return Outcome::Failure(admitted.ErrorValue());
            auto settings = PrepareSceneBodySettings(descriptor, shape, world);
            if (settings.HasError())
                return Outcome::Failure(settings.ErrorValue());
            auto &bodies = world.native.system->GetBodyInterface();
            const JPH::Body *body = bodies.CreateBody(settings.Value());
            const auto control = CompleteCanonicalBodyControl(before, ReadCanonicalBodyControl());
            if (!body)
                return Outcome::Failure(MakeError(PhysicsErrors::CapacityExceeded));
            auto admitted = AdmitCanonicalBodyControl(world.owner.constructionRecipes, control);
            if (body->GetID().GetIndex() >= world.query.nativeFixtureIndices.size() || admitted.HasError()) {
                const auto failure = admitted.HasError() ? admitted.ErrorValue() : MakeError(PhysicsErrors::CapacityExceeded);
                bodies.DestroyBody(body->GetID());
                return Outcome::Failure(failure);
            }
            return Outcome::Success({body->GetID(), std::move(settings).Value(), control});
        }

        /** @brief Allocates detached bodies against staged or resident shapes; batch abort owns every admitted body. */
        Result<void> PrepareDetachedBodies(CanonicalSceneBodyBatchState &batch,
                                           const std::span<const PhysicsSceneBodyDescriptor> descriptors) {
            auto &canonical = *batch.world;
            for (const auto &descriptor : descriptors) {
                if (auto valid = ValidateSceneBodyRequest(canonical, batch.owner, descriptor); valid.HasError())
                    return valid;
                const auto *shape = DetachedShape(batch, descriptor.body.shape);
                if (!shape)
                    return Result<void>::Failure(MakeError(PhysicsErrors::HandleStale));
                auto ownedCollision = descriptor.collision;
                auto created = CreateDetachedBody(canonical, descriptor, *shape);
                if (created.HasError())
                    return Result<void>::Failure(created.ErrorValue());
                auto admitted = std::move(created).Value();
                const BodyHandle identity{batch.owner, {canonical.nextResourceSlot++, 1}};
                batch.records.push_back(BodyRecord(descriptor, *shape, identity, admitted.body, admitted.settings,
                                                   std::move(ownedCollision), admitted.control));
                batch.handles.push_back(identity);
                batch.nativeBodies.push_back(admitted.body);
                if (InitiallyAwake(descriptor))
                    batch.awakeBodies.push_back(admitted.body);
            }
            return Result<void>::Success();
        }

    }  // namespace

    /** @copydoc ResolveCanonicalMotionQuality */
    Result<JPH::EMotionQuality> ResolveCanonicalMotionQuality(const PhysicsContinuousCollisionPolicy &policy,
                                                              const PhysicsBodyDescriptor &body, const JPH::Shape &shape,
                                                              const bool sensor) {
        const bool eligible =
            body.motion == PhysicsMotionType::Dynamic && !sensor && std::isfinite(shape.GetInnerRadius()) && shape.GetInnerRadius() > 0.0F;
        const auto requested =
            body.continuousCollision.mode.value_or(eligible ? policy.defaultMode : PhysicsDefaultMotionQuality::Discrete);
        if (requested > PhysicsDefaultMotionQuality::LinearCast || (requested == PhysicsDefaultMotionQuality::LinearCast && !eligible))
            return Result<JPH::EMotionQuality>::Failure(MakeError(PhysicsErrors::OperationUnsupported,
                                                                  "Linear CCD requires a non-sensor dynamic shape with "
                                                                  "positive inner radius."));
        return Result<JPH::EMotionQuality>::Success(requested == PhysicsDefaultMotionQuality::LinearCast ? JPH::EMotionQuality::LinearCast
                                                                                                         : JPH::EMotionQuality::Discrete);
    }

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
        auto prepared = PrepareResidentBody(canonical, owner, descriptor);
        if (prepared.HasError())
            return Result<BodyHandle>::Failure(prepared.ErrorValue());
        auto admitted = std::move(prepared).Value();
        const auto activation = InitiallyAwake(descriptor) ? JPH::EActivation::Activate : JPH::EActivation::DontActivate;
        const JPH::BodyID nativeBody = canonical.native.system->GetBodyInterface().CreateAndAddBody(admitted.settings, activation);
        const auto control = CompleteCanonicalBodyControl(admitted.beforeControl, ReadCanonicalBodyControl());
        if (nativeBody.IsInvalid())
            return Result<BodyHandle>::Failure(
                MakeError(PhysicsErrors::CapacityExceeded, "Canonical solver rejected the scene body admission."));
        const auto controlAdmission = AdmitCanonicalBodyControl(canonical.owner.constructionRecipes, control);
        if (nativeBody.GetIndex() >= canonical.query.nativeFixtureIndices.size() || controlAdmission.HasError()) {
            canonical.native.system->GetBodyInterface().RemoveBody(nativeBody);
            canonical.native.system->GetBodyInterface().DestroyBody(nativeBody);
            return Result<BodyHandle>::Failure(controlAdmission.HasError() ? controlAdmission.ErrorValue()
                                                                           : MakeError(PhysicsErrors::CapacityExceeded));
        }

        const std::uint32_t slot = canonical.nextResourceSlot++;
        const BodyHandle identity{owner, {slot, 1}};
        canonical.scene.bodies.emplace_back(
            BodyRecord(descriptor, *admitted.shape, identity, nativeBody, admitted.settings, std::move(admitted.collision), control));
        canonical.scene.nativeBodyIndices[nativeBody.GetIndex()] = canonical.scene.bodies.size() - 1;
        return Result<BodyHandle>::Success(identity);
    }

    namespace {
        /** @brief Complete declared capacities are checked before reserving private batch storage. */
        Result<void> AdmitBodyBatch(const CanonicalWorld &world, std::size_t bodyCount, std::size_t shapeCount,
                                    bool contradictoryDescriptors) {
            if (bodyCount > 256 || shapeCount > 1024 || !world.pendingBodyBatch.expired() || contradictoryDescriptors)
                return Result<void>::Failure(MakeError(PhysicsErrors::InvalidState));
            const auto slots = std::numeric_limits<std::uint32_t>::max() - world.nextResourceSlot;
            if (bodyCount > world.scene.maximumBodies - world.scene.bodies.size() ||
                shapeCount > world.scene.maximumShapes - world.scene.shapes.size() || shapeCount > slots || bodyCount > slots - shapeCount)
                return Result<void>::Failure(MakeError(PhysicsErrors::CapacityExceeded));
            return Result<void>::Success();
        }

        /** @brief Copies only admitted group descriptors and substitutes actual detached native-shape handles. */
        Result<void> ResolveGroupBodyDescriptors(const CanonicalSceneBodyBatchState &batch, std::span<const PhysicsSceneGroupBody> group,
                                                 std::vector<PhysicsSceneBodyDescriptor> &resolved) {
            resolved.reserve(group.size());
            for (const auto &body : group) {
                if (body.shape >= batch.shapes.size())
                    return Result<void>::Failure(MakeError(PhysicsErrors::DescriptorInvalid));
                resolved.push_back(body.descriptor);
                resolved.back().body.shape = batch.shapes[body.shape].handle;
            }
            return Result<void>::Success();
        }

        /** @brief Sole private batch owns every admitted native allocation, including all failure paths. */
        std::shared_ptr<CanonicalSceneBodyBatchState> AllocateBodyBatch(CanonicalWorld &world, PhysicsWorldId owner,
                                                                        std::size_t bodyCount) {
            auto batch = std::make_shared<CanonicalSceneBodyBatchState>();
            batch->records.reserve(bodyCount);
            batch->handles.reserve(bodyCount);
            batch->nativeBodies.reserve(bodyCount);
            batch->awakeBodies.reserve(bodyCount);
            batch->world = &world;
            batch->owner = owner;
            world.pendingBodyBatch = batch;
            return batch;
        }

        /** @brief Empty private batches require no native broadphase insertion. */
        void PrepareBatchBroadphase(CanonicalSceneBodyBatchState &batch) {
            if (batch.nativeBodies.empty())
                return;
            batch.addState = batch.world->native.system->GetBodyInterface().AddBodiesPrepare(batch.nativeBodies.data(),
                                                                                             static_cast<int>(batch.nativeBodies.size()));
            batch.broadphasePrepared = true;
        }
    }  // namespace

    /** @copydoc PrepareCanonicalSceneBodies */
    Result<CanonicalSceneBodyBatch> PrepareCanonicalSceneBodies(const CanonicalWorldHandle world, const PhysicsWorldId owner,
                                                                std::span<const PhysicsSceneBodyDescriptor> descriptors,
                                                                const std::span<const PhysicsSceneGroupShape> shapes,
                                                                const std::span<const PhysicsSceneGroupBody> groupBodies,
                                                                const std::span<const StateDetail::ConstructionRecipeLease> recipes) {
        using BatchResult = Result<CanonicalSceneBodyBatch>;
        if (!world.value || !owner.IsValid())
            return BatchResult::Failure(MakeError(PhysicsErrors::WorldInvalid));
        auto &canonical = *static_cast<CanonicalWorld *>(world.value);
        const auto bodyCount = groupBodies.empty() ? descriptors.size() : groupBodies.size();
        if (auto capacity = AdmitBodyBatch(canonical, bodyCount, shapes.size(), !groupBodies.empty() && !descriptors.empty());
            capacity.HasError())
            return BatchResult::Failure(capacity.ErrorValue());
        auto prepared = AllocateBodyBatch(canonical, owner, bodyCount);
        if (const auto ready = PrepareGroupShapes(*prepared, owner, shapes, recipes); ready.HasError())
            return BatchResult::Failure(ready.ErrorValue());
        std::vector<PhysicsSceneBodyDescriptor> resolved;
        if (!groupBodies.empty()) {
            if (auto group = ResolveGroupBodyDescriptors(*prepared, groupBodies, resolved); group.HasError())
                return BatchResult::Failure(group.ErrorValue());
            descriptors = resolved;
        }
        if (const auto ready = PrepareDetachedBodies(*prepared, descriptors); ready.HasError())
            return BatchResult::Failure(ready.ErrorValue());
        PrepareBatchBroadphase(*prepared);
        return BatchResult::Success(CanonicalSceneBodyBatch{std::move(prepared)});
    }

}  // namespace Horo::Physics::Detail
