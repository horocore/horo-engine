#include "CanonicalPhysicsRuntimeInternal.h"

#include <cassert>

namespace Horo::Physics::Detail {
    /** @brief Detached native storage; the weak world registration lets teardown invalidate retained candidates safely. */
    struct CanonicalSceneBodyBatchState final {
        ~CanonicalSceneBodyBatchState() {
            Abort();
        }

        void Abort() noexcept {
            if (world == nullptr)
                return;
            auto &bodies = world->native.system->GetBodyInterface();
            if (broadphasePrepared)
                bodies.AddBodiesAbort(nativeBodies.data(), static_cast<int>(nativeBodies.size()), addState);
            constraints.clear();
            nativeConstraints.clear();
            for (const auto &record : records)
                bodies.DestroyBody(record.nativeBody);
            shapes.clear();
            world->pendingBodyBatch.reset();
            world = nullptr;
        }

        CanonicalWorld *world{};
        PhysicsWorldId owner;
        std::vector<BodyHandle> retiredBodies;
        std::vector<ShapeHandle> retiredShapes;
        std::vector<ConstraintHandle> retiredConstraints;
        std::vector<JPH::BodyID> retiredNativeBodies;
        std::vector<JPH::Constraint *> retiredNativeConstraints;
        bool retirementPrepared{};
        bool retirementFailed{};
        std::vector<CanonicalSceneShapeRecord> shapes;
        std::vector<ShapeHandle> shapeHandles;
        std::vector<CanonicalSceneConstraintRecord> constraints;
        std::vector<ConstraintHandle> constraintHandles;
        std::vector<JPH::Constraint *> nativeConstraints;
        std::vector<std::uint64_t> collisionPairs;
        bool constraintsPrepared{};
        bool constraintsFailed{};
        std::vector<CanonicalSceneBodyRecord> records;
        std::vector<BodyHandle> handles;
        std::vector<JPH::BodyID> nativeBodies;
        std::vector<JPH::BodyID> awakeBodies;
        JPH::BodyInterface::AddState addState{};
        bool broadphasePrepared{};
    };

    /** @copydoc CanonicalSceneBodyBatch::CanonicalSceneBodyBatch */
    CanonicalSceneBodyBatch::CanonicalSceneBodyBatch(std::shared_ptr<CanonicalSceneBodyBatchState> state) noexcept
        : state_(std::move(state)) {}

    CanonicalSceneBodyBatch::~CanonicalSceneBodyBatch() = default;
    CanonicalSceneBodyBatch::CanonicalSceneBodyBatch(CanonicalSceneBodyBatch &&) noexcept = default;
    CanonicalSceneBodyBatch &CanonicalSceneBodyBatch::operator=(CanonicalSceneBodyBatch &&) noexcept = default;

    /** @copydoc CanonicalSceneBodyBatch::Handles */
    std::span<const BodyHandle> CanonicalSceneBodyBatch::Handles() const noexcept {
        return state_ ? std::span<const BodyHandle>{state_->handles} : std::span<const BodyHandle>{};
    }

    /** @copydoc CanonicalSceneBodyBatch::Shapes */
    std::span<const ShapeHandle> CanonicalSceneBodyBatch::Shapes() const noexcept {
        return state_ ? std::span<const ShapeHandle>{state_->shapeHandles} : std::span<const ShapeHandle>{};
    }

    /** @copydoc CanonicalSceneBodyBatch::Constraints */
    std::span<const ConstraintHandle> CanonicalSceneBodyBatch::Constraints() const noexcept {
        return state_ ? std::span<const ConstraintHandle>{state_->constraintHandles} : std::span<const ConstraintHandle>{};
    }

    /** @copydoc CanonicalSceneBodyBatch::RetiredConstraints */
    std::span<const ConstraintHandle> CanonicalSceneBodyBatch::RetiredConstraints() const noexcept {
        return state_ ? std::span<const ConstraintHandle>{state_->retiredConstraints} : std::span<const ConstraintHandle>{};
    }

    /** @copydoc CanonicalSceneBodyBatch::PrepareRetirement */
    Result<void> CanonicalSceneBodyBatch::PrepareRetirement(const std::span<const BodyHandle> bodies,
                                                            const std::span<const ShapeHandle> shapes,
                                                            const std::span<const ConstraintHandle> constraints) {
        if (!IsPending() || state_->retirementPrepared || state_->retirementFailed || state_->constraintsPrepared ||
            state_->constraintsFailed)
            return Result<void>::Failure(MakeError(PhysicsErrors::InvalidState));
        state_->retirementFailed = true;
        auto &world = *state_->world;
        if (bodies.size() > world.scene.maximumBodies || shapes.size() > world.scene.maximumShapes ||
            constraints.size() > world.scene.maximumConstraints)
            return Result<void>::Failure(MakeError(PhysicsErrors::CapacityExceeded));
        auto retiringBodies = std::vector<BodyHandle>{bodies.begin(), bodies.end()};
        auto retiringShapes = std::vector<ShapeHandle>{shapes.begin(), shapes.end()};
        auto retiringConstraints = std::vector<ConstraintHandle>{constraints.begin(), constraints.end()};
        std::vector<JPH::BodyID> nativeBodies;
        std::vector<JPH::Constraint *> nativeConstraints;
        nativeBodies.reserve(bodies.size());
        nativeConstraints.reserve(world.scene.constraints.size());
        retiringShapes.reserve(shapes.size() + bodies.size());
        retiringConstraints.reserve(world.scene.constraints.size());
        const auto unique = [](const auto &handles) {
            for (std::size_t index = 0; index < handles.size(); ++index)
                if (std::find(handles.begin(), handles.begin() + index, handles[index]) != handles.begin() + index)
                    return false;
            return true;
        };
        if (!unique(retiringBodies) || !unique(retiringShapes) || !unique(retiringConstraints))
            return Result<void>::Failure(MakeError(PhysicsErrors::DescriptorInvalid));
        for (const auto handle : bodies) {
            const auto found = std::ranges::find(world.scene.bodies, handle, &CanonicalSceneBodyRecord::handle);
            if (found == world.scene.bodies.end())
                return Result<void>::Failure(MakeError(PhysicsErrors::HandleStale));
            nativeBodies.push_back(found->nativeBody);
            const auto root = found->policy.shape;
            const bool shared = std::ranges::any_of(world.scene.bodies, [&](const auto &body) {
                return body.policy.shape == root && std::ranges::find(retiringBodies, body.handle) == retiringBodies.end();
            });
            if (!shared && std::ranges::find(retiringShapes, root) == retiringShapes.end())
                retiringShapes.push_back(root);
        }
        for (const auto handle : retiringShapes) {
            if (std::ranges::find(world.scene.shapes, handle, &CanonicalSceneShapeRecord::handle) == world.scene.shapes.end() ||
                std::ranges::any_of(world.scene.bodies, [&](const auto &body) {
                return body.policy.shape == handle && std::ranges::find(retiringBodies, body.handle) == retiringBodies.end();
            }))
                return Result<void>::Failure(MakeError(PhysicsErrors::HandleStale));
        }
        for (const auto &constraint : world.scene.constraints) {
            if ((std::ranges::find(retiringBodies, constraint.first) != retiringBodies.end() ||
                 std::ranges::find(retiringBodies, constraint.second) != retiringBodies.end()) &&
                std::ranges::find(retiringConstraints, constraint.handle) == retiringConstraints.end())
                retiringConstraints.push_back(constraint.handle);
        }
        for (const auto handle : retiringConstraints) {
            const auto found = std::ranges::find(world.scene.constraints, handle, &CanonicalSceneConstraintRecord::handle);
            if (found == world.scene.constraints.end())
                return Result<void>::Failure(MakeError(PhysicsErrors::HandleStale));
            nativeConstraints.push_back(found->constraint.GetPtr());
        }
        std::vector<std::uint64_t> pairs;
        pairs.reserve(world.scene.maximumConstraints);
        for (const auto &record : world.scene.constraints) {
            if (std::ranges::find(retiringConstraints, record.handle) != retiringConstraints.end() || record.secondBody.IsInvalid() ||
                record.collisionPolicy != PhysicsJointCollisionPolicy::DisableBetweenBodies)
                continue;
            const auto first = record.firstBody.GetIndexAndSequenceNumber();
            const auto second = record.secondBody.GetIndexAndSequenceNumber();
            const auto key = (static_cast<std::uint64_t>(std::min(first, second)) << 32U) | std::max(first, second);
            const auto position = std::ranges::lower_bound(pairs, key);
            if (position == pairs.end() || *position != key)
                pairs.insert(position, key);
        }
        state_->retiredBodies = std::move(retiringBodies);
        state_->retiredShapes = std::move(retiringShapes);
        state_->retiredConstraints = std::move(retiringConstraints);
        state_->retiredNativeBodies = std::move(nativeBodies);
        state_->retiredNativeConstraints = std::move(nativeConstraints);
        state_->collisionPairs = std::move(pairs);
        state_->retirementPrepared = true;
        state_->retirementFailed = false;
        return Result<void>::Success();
    }

    /** @copydoc CanonicalSceneBodyBatch::PrepareConstraints */
    Result<void> CanonicalSceneBodyBatch::PrepareConstraints(const std::span<const PhysicsConstraintDescriptor> descriptors) {
        if (!IsPending() || state_->retirementFailed || state_->constraintsPrepared || state_->constraintsFailed)
            return Result<void>::Failure(MakeError(PhysicsErrors::InvalidState));
        state_->constraintsFailed = true;
        auto &world = *state_->world;
        if (descriptors.size() > 256 || descriptors.size() > world.scene.maximumConstraints - world.scene.constraints.size() ||
            descriptors.size() > std::numeric_limits<std::uint32_t>::max() - world.scene.nextConstraintSlot)
            return Result<void>::Failure(MakeError(PhysicsErrors::CapacityExceeded));
        std::vector<CanonicalSceneConstraintRecord> records;
        std::vector<ConstraintHandle> handles;
        std::vector<JPH::Constraint *> native;
        records.reserve(descriptors.size());
        handles.reserve(descriptors.size());
        native.reserve(descriptors.size());
        std::vector<std::uint64_t> pairs;
        pairs.reserve(world.scene.maximumConstraints);
        for (const auto &record : world.scene.constraints) {
            if (std::ranges::find(state_->retiredConstraints, record.handle) != state_->retiredConstraints.end() ||
                record.secondBody.IsInvalid() || record.collisionPolicy != PhysicsJointCollisionPolicy::DisableBetweenBodies)
                continue;
            const auto first = record.firstBody.GetIndexAndSequenceNumber();
            const auto second = record.secondBody.GetIndexAndSequenceNumber();
            const auto key = (static_cast<std::uint64_t>(std::min(first, second)) << 32U) | std::max(first, second);
            const auto position = std::ranges::lower_bound(pairs, key);
            if (position == pairs.end() || *position != key)
                pairs.insert(position, key);
        }
        for (const auto &descriptor : descriptors) {
            if (std::ranges::find(state_->retiredBodies, descriptor.first.body) != state_->retiredBodies.end() ||
                (std::holds_alternative<PhysicsBodyAnchor>(descriptor.second) &&
                 std::ranges::find(state_->retiredBodies, std::get<PhysicsBodyAnchor>(descriptor.second).body) !=
                     state_->retiredBodies.end()))
                return Result<void>::Failure(MakeError(PhysicsErrors::HandleStale));
            auto prepared = PrepareCanonicalConstraintRecord({&world}, state_->owner, descriptor, state_->records);
            if (prepared.HasError())
                return Result<void>::Failure(prepared.ErrorValue());
            auto &record = prepared.Value();
            if (!record.secondBody.IsInvalid() && record.collisionPolicy == PhysicsJointCollisionPolicy::DisableBetweenBodies) {
                const auto first = record.firstBody.GetIndexAndSequenceNumber();
                const auto second = record.secondBody.GetIndexAndSequenceNumber();
                const auto key = (static_cast<std::uint64_t>(std::min(first, second)) << 32U) | std::max(first, second);
                const auto position = std::ranges::lower_bound(pairs, key);
                if (position == pairs.end() || *position != key)
                    pairs.insert(position, key);
            }
            handles.push_back(record.handle);
            native.push_back(record.constraint.GetPtr());
            records.push_back(std::move(record));
        }
        // Supported capacity-only backend patch: no constraint has been registered or assigned a native index.
        const auto before = world.native.system->GetConstraints();
        std::vector<bool> enabled;
        enabled.reserve(before.size());
        for (const auto &constraint : before)
            enabled.push_back(constraint->GetEnabled());
        world.native.system->ReserveConstraints(static_cast<std::uint32_t>(world.scene.constraints.size() + records.size()));
        const auto after = world.native.system->GetConstraints();
        assert(before.size() == after.size());
        for (std::size_t index = 0; index < before.size(); ++index) {
            assert(before[index].GetPtr() == after[index].GetPtr());
            assert(before[index]->GetEnabled() == enabled[index]);
        }
        state_->constraints = std::move(records);
        state_->constraintHandles = std::move(handles);
        state_->nativeConstraints = std::move(native);
        state_->collisionPairs = std::move(pairs);
        state_->constraintsPrepared = true;
        state_->constraintsFailed = false;
        return Result<void>::Success();
    }

    /** @copydoc CanonicalSceneBodyBatch::ValidatePublication */
    Result<void> CanonicalSceneBodyBatch::ValidatePublication() const {
        if (!IsPending() || state_->constraintsFailed || state_->retirementFailed)
            return Result<void>::Failure(MakeError(PhysicsErrors::InvalidState));
        return Result<void>::Success();
    }

    /** @copydoc CanonicalSceneBodyBatch::IsPending */
    bool CanonicalSceneBodyBatch::IsPending() const noexcept {
        return state_ && state_->world && state_->world->pendingBodyBatch.lock() == state_;
    }

    /** @copydoc CanonicalSceneBodyBatch::Publish */
    void CanonicalSceneBodyBatch::Publish() noexcept {
        if (!IsPending() || state_->constraintsFailed || state_->retirementFailed)
            return;
        auto &world = *state_->world;
        auto &bodies = world.native.system->GetBodyInterface();
        if (!state_->retiredNativeConstraints.empty())
            world.native.system->RemoveConstraints(state_->retiredNativeConstraints.data(),
                                                   static_cast<int>(state_->retiredNativeConstraints.size()));
        std::erase_if(world.scene.constraints, [&](const auto &record) {
            return std::ranges::find(state_->retiredConstraints, record.handle) != state_->retiredConstraints.end();
        });
        if (!state_->retiredNativeBodies.empty()) {
            bodies.RemoveBodies(state_->retiredNativeBodies.data(), static_cast<int>(state_->retiredNativeBodies.size()));
            bodies.DestroyBodies(state_->retiredNativeBodies.data(), static_cast<int>(state_->retiredNativeBodies.size()));
        }
        std::erase_if(world.scene.bodies, [&](const auto &record) {
            return std::ranges::find(state_->retiredBodies, record.handle) != state_->retiredBodies.end();
        });
        std::erase_if(world.scene.shapes, [&](const auto &record) {
            return std::ranges::find(state_->retiredShapes, record.handle) != state_->retiredShapes.end();
        });
        if (state_->broadphasePrepared)
            bodies.AddBodiesFinalize(state_->nativeBodies.data(), static_cast<int>(state_->nativeBodies.size()), state_->addState,
                                     JPH::EActivation::DontActivate);
        for (auto &shape : state_->shapes)
            world.scene.shapes.push_back(std::move(shape));
        for (auto &record : state_->records)
            world.scene.bodies.push_back(std::move(record));
        if (state_->constraintsPrepared) {
            [[maybe_unused]] const auto capacity = world.native.system->GetConstraintCapacity();
            if (!state_->nativeConstraints.empty())
                world.native.system->AddConstraints(state_->nativeConstraints.data(), static_cast<int>(state_->nativeConstraints.size()));
            assert(world.native.system->GetConstraintCapacity() == capacity);
            for (auto &constraint : state_->constraints)
                world.scene.constraints.push_back(std::move(constraint));
        }
        if (state_->constraintsPrepared || state_->retirementPrepared)
            world.scene.disabledJointCollisionPairs.swap(state_->collisionPairs);
        if (!state_->awakeBodies.empty())
            bodies.ActivateBodies(state_->awakeBodies.data(), static_cast<int>(state_->awakeBodies.size()));
        world.pendingBodyBatch.reset();
        state_->world = nullptr;
        state_->broadphasePrepared = false;
    }

    /** @copydoc HasPendingCanonicalSceneBodies */
    bool HasPendingCanonicalSceneBodies(const CanonicalWorldHandle world) noexcept {
        return world.value && !static_cast<CanonicalWorld *>(world.value)->pendingBodyBatch.expired();
    }

    /** @copydoc CancelPendingCanonicalSceneBodies */
    void CancelPendingCanonicalSceneBodies(const CanonicalWorldHandle world) noexcept {
        if (world.value) {
            if (auto pending = static_cast<CanonicalWorld *>(world.value)->pendingBodyBatch.lock())
                pending->Abort();
        }
    }

    namespace {
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
                    const auto &children = std::get<std::vector<PhysicsSceneGroupShape::Child>>(input.geometry);
                    if (children.empty() || children.size() > 256)
                        return Result<void>::Failure(MakeError(PhysicsErrors::DescriptorInvalid));
                    JPH::StaticCompoundShapeSettings settings;
                    for (const auto &child : children) {
                        if (child.shape >= batch.shapes.size())
                            return Result<void>::Failure(MakeError(PhysicsErrors::DescriptorInvalid));
                        if (const auto valid = ValidatePhysicsPose(child.localPose); valid.HasError())
                            return valid;
                        settings.AddShape(ToNative(child.localPose.translation), ToNative(child.localPose.rotation),
                                          batch.shapes[child.shape].shape.GetPtr());
                    }
                    auto created = settings.Create();
                    if (created.HasError())
                        return Result<void>::Failure(MakeError(PhysicsErrors::ShapeArtifactInvalid));
                    native = created.Get();
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
        for (const auto &descriptor : descriptors) {
            if (auto valid = ValidatePhysicsBodyDescriptor(descriptor.body, owner); valid.HasError())
                return BatchResult::Failure(valid.ErrorValue());
            if (auto valid = ValidateInitialBodyActivity(descriptor, canonical.native.system->GetPhysicsSettings().mAllowSleeping);
                valid.HasError())
                return BatchResult::Failure(valid.ErrorValue());
            const auto stagedShape = std::ranges::find(prepared->shapes, descriptor.body.shape, &CanonicalSceneShapeRecord::handle);
            const auto residentShape = std::ranges::find(canonical.scene.shapes, descriptor.body.shape, &CanonicalSceneShapeRecord::handle);
            const auto *shape = stagedShape != prepared->shapes.end()           ? std::to_address(stagedShape)
                                : residentShape != canonical.scene.shapes.end() ? std::to_address(residentShape)
                                                                                : nullptr;
            if (!shape)
                return BatchResult::Failure(MakeError(PhysicsErrors::HandleStale));
            auto settings = PrepareSceneBodySettings(descriptor, *shape);
            if (settings.HasError())
                return BatchResult::Failure(settings.ErrorValue());
            auto &bodyInterface = canonical.native.system->GetBodyInterface();
            JPH::Body *body = bodyInterface.CreateBody(settings.Value());
            if (!body)
                return BatchResult::Failure(MakeError(PhysicsErrors::CapacityExceeded));
            const auto nativeId = body->GetID();
            if (nativeId.GetIndex() >= canonical.query.nativeFixtureIndices.size()) {
                bodyInterface.DestroyBody(nativeId);
                return BatchResult::Failure(MakeError(PhysicsErrors::CapacityExceeded));
            }
            const BodyHandle identity{owner, {canonical.scene.nextBodySlot++, 1}};
            prepared->records.push_back(
                {.handle = identity,
                 .nativeBody = nativeId,
                 .pose = descriptor.body.pose,
                 .policy = descriptor.body,
                 .motionStorageReserved = descriptor.body.motion != PhysicsMotionType::Static || settings.Value().mAllowDynamicOrKinematic,
                 .sceneEntity = descriptor.sceneEntity});
            prepared->handles.push_back(identity);
            prepared->nativeBodies.push_back(nativeId);
            if (descriptor.body.motion != PhysicsMotionType::Static && descriptor.initialActivity == PhysicsInitialBodyActivity::Awake)
                prepared->awakeBodies.push_back(nativeId);
        }
        if (!descriptors.empty()) {
            prepared->addState = canonical.native.system->GetBodyInterface().AddBodiesPrepare(prepared->nativeBodies.data(),
                                                                                              static_cast<int>(descriptors.size()));
            prepared->broadphasePrepared = true;
        }
        return BatchResult::Success(CanonicalSceneBodyBatch{std::move(prepared)});
    }

}  // namespace Horo::Physics::Detail
