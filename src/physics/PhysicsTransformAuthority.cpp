#include "Horo/Physics/PhysicsTransformAuthority.h"

#include "Horo/Physics/PhysicsErrors.h"

#include <algorithm>
#include <new>
#include <ranges>
#include <thread>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace Horo::Physics {
    namespace {
        /** @brief Returns the common identity from any closed transform command alternative. */
        [[nodiscard]] const PhysicsTransformCommandIdentity &CommandIdentity(const PhysicsTransformCommand &command) noexcept {
            return std::visit([](const auto &value) -> const PhysicsTransformCommandIdentity & {
                return value.identity;
            }, command);
        }

        /** @brief Checks one closed static-update policy. */
        [[nodiscard]] bool IsKnownStaticPolicy(const PhysicsStaticTransformUpdatePolicy policy) noexcept {
            return policy == PhysicsStaticTransformUpdatePolicy::UpdateBroadphase || policy == PhysicsStaticTransformUpdatePolicy::Rebuild;
        }

        /** @brief Checks one closed dynamic operation. */
        [[nodiscard]] bool IsKnownDynamicOperation(const PhysicsDynamicTransformOperation operation) noexcept {
            return operation == PhysicsDynamicTransformOperation::Teleport || operation == PhysicsDynamicTransformOperation::Reset;
        }

        /** @brief Checks one closed teleport velocity policy. */
        [[nodiscard]] bool IsKnownVelocityPolicy(const PhysicsTeleportVelocityPolicy policy) noexcept {
            return policy == PhysicsTeleportVelocityPolicy::Preserve || policy == PhysicsTeleportVelocityPolicy::Reset;
        }

        /** @brief Compares only target identity and consuming tick for conflict detection. */
        [[nodiscard]] bool SameBodyTick(const PhysicsTransformCommand &left, const PhysicsTransformCommand &right) noexcept {
            const auto &leftIdentity = CommandIdentity(left);
            const auto &rightIdentity = CommandIdentity(right);
            return leftIdentity.simulationTick == rightIdentity.simulationTick && leftIdentity.body == rightIdentity.body;
        }

        /** @brief Returns a non-throwing owner-thread failure for mutable authority operations. */
        [[nodiscard]] Result<void> RequireOwner(const std::thread::id ownerThread) {
            if (ownerThread != std::this_thread::get_id())
                return Result<void>::Failure(MakeError(PhysicsErrors::ThreadAffinityViolation));
            return Result<void>::Success();
        }

        /** @brief Returns the body record for an exact registered handle. */
        template <typename Records> [[nodiscard]] auto FindBody(Records &records, const BodyHandle &body) {
            return std::ranges::find_if(records, [body](const auto &record) {
                return record.registration.body == body;
            });
        }

    }  // namespace

    /** @brief Target-private storage for one detached transform-authority candidate. */
    struct PhysicsBodyTransformAuthority::Impl final {
        struct BodyRecord final {
            PhysicsBodyTransformRegistration registration;
            PhysicsPose runtimePose;
            PhysicsBodyState dynamicState;
            std::uint64_t lastAppliedTick{};
            std::uint64_t lastPublishedTick{};
            bool hasDynamicSnapshot{};
        };

        explicit Impl(const PhysicsTransformAuthorityDescriptor &authorityDescriptor)
            : descriptor(authorityDescriptor), ownerThread(std::this_thread::get_id()) {
            bodies.reserve(descriptor.maximumBodies);
            commands.reserve(descriptor.maximumPendingCommands);
        }

        PhysicsTransformAuthorityDescriptor descriptor;
        PhysicsTransformAuthorityState state{PhysicsTransformAuthorityState::Prepared};
        std::thread::id ownerThread;
        std::vector<BodyRecord> bodies;
        std::vector<PhysicsTransformCommand> commands;
        std::uint64_t lastAppliedTick{};
        bool applying{};
    };

    namespace {
        /** @brief Applies one validated command without allocating or touching authored scene storage. */
        void ApplyTransformCommand(const PhysicsTransformCommand &command, auto &record, PhysicsTransformTickResult &result) noexcept {
            record.lastAppliedTick = CommandIdentity(command).simulationTick;
            std::visit([&record, &result](const auto &value) {
                using Type = std::decay_t<decltype(value)>;
                if constexpr (std::is_same_v<Type, PhysicsStaticTransformCommand>) {
                    record.registration.authoredPose = value.authoredPose;
                    record.runtimePose = value.authoredPose;
                    if (value.updatePolicy == PhysicsStaticTransformUpdatePolicy::Rebuild)
                        ++result.staticRebuilds;
                    else
                        ++result.staticBroadphaseUpdates;
                } else if constexpr (std::is_same_v<Type, PhysicsKinematicTargetCommand>) {
                    record.runtimePose = value.targetPose;
                    ++result.kinematicTargets;
                } else {
                    record.runtimePose = value.targetPose;
                    record.dynamicState.pose = value.targetPose;
                    if (value.velocityPolicy == PhysicsTeleportVelocityPolicy::Reset) {
                        record.dynamicState.linearVelocity = {};
                        record.dynamicState.angularVelocity = {};
                    }
                    record.dynamicState.activity = PhysicsBodyActivity::Awake;
                    record.hasDynamicSnapshot = false;
                    record.lastPublishedTick = 0;
                    ++result.dynamicControls;
                }
                ++result.appliedCommands;
            }, command);
        }

        /** @brief Confirms a typed command's shared identity and exact consuming frame. */
        Result<void> ValidateCommandIdentity(const PhysicsTransformCommandIdentity &identity, const PhysicsWorldId expectedWorld,
                                             const std::uint64_t expectedSceneGeneration, const std::uint64_t expectedSimulationTick) {
            if (expectedSceneGeneration == 0 || expectedSimulationTick == 0)
                return Result<void>::Failure(MakeError(PhysicsErrors::DescriptorInvalid, "Transform admission frame is invalid."));
            if (identity.protocolVersion != PhysicsTransformAuthorityProtocolVersion || identity.simulationTick == 0 ||
                identity.sceneGeneration == 0 || identity.sourceSequence == 0 || !identity.source.IsValid())
                return Result<void>::Failure(
                    MakeError(PhysicsErrors::CommandOrderInvalid, "Transform command identity or ordering evidence is incomplete."));
            if (const Result<void> owner = ValidatePhysicsHandleOwner(identity.body, expectedWorld); owner.HasError())
                return owner;
            if (identity.sceneGeneration != expectedSceneGeneration || identity.simulationTick != expectedSimulationTick)
                return Result<void>::Failure(
                    MakeError(PhysicsErrors::CommandOrderInvalid, "Transform command targets another fixed-tick admission frame."));
            return Result<void>::Success();
        }

        /** @brief Validates detached authority capacity and owner generations before allocation. */
        Result<void> ValidateAuthorityDescriptor(const PhysicsTransformAuthorityDescriptor &descriptor) {
            if (!descriptor.world.IsValid())
                return Result<void>::Failure(MakeError(PhysicsErrors::WorldInvalid));
            if (descriptor.sceneGeneration == 0)
                return Result<void>::Failure(
                    MakeError(PhysicsErrors::DescriptorInvalid, "Transform authority requires a scene generation."));
            if (descriptor.maximumBodies == 0 || descriptor.maximumBodies > MaximumPhysicsTransformAuthorityBodies ||
                descriptor.maximumPendingCommands == 0 || descriptor.maximumPendingCommands > MaximumPhysicsTransformAuthorityCommands)
                return Result<void>::Failure(
                    MakeError(PhysicsErrors::CapacityExceeded, "Transform authority reservations exceed the bounded Physics profile."));
            return Result<void>::Success();
        }

        /** @brief Checks one command's body mode against its selected authority operation. */
        Result<void> ValidateCommandAuthority(const PhysicsTransformCommand &command, const PhysicsMotionType motion) {
            const auto authority = ResolvePhysicsTransformAuthority(motion);
            if (authority.HasError())
                return Result<void>::Failure(authority.ErrorValue());
            const bool valid = std::visit([authority = authority.Value()](const auto &value) {
                using Type = std::decay_t<decltype(value)>;
                if constexpr (std::is_same_v<Type, PhysicsStaticTransformCommand>)
                    return authority == PhysicsTransformAuthority::StaticScene;
                if constexpr (std::is_same_v<Type, PhysicsKinematicTargetCommand>)
                    return authority == PhysicsTransformAuthority::KinematicTarget;
                return authority == PhysicsTransformAuthority::DynamicSolver;
            }, command);
            if (!valid)
                return Result<void>::Failure(MakeError(PhysicsErrors::TransformAuthorityViolation,
                                                       "The transform command does not match the registered body's transform authority."));
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc ResolvePhysicsTransformAuthority */
    Result<PhysicsTransformAuthority> ResolvePhysicsTransformAuthority(const PhysicsMotionType motion) {
        switch (motion) {
            case PhysicsMotionType::Static:
                return Result<PhysicsTransformAuthority>::Success(PhysicsTransformAuthority::StaticScene);
            case PhysicsMotionType::Kinematic:
                return Result<PhysicsTransformAuthority>::Success(PhysicsTransformAuthority::KinematicTarget);
            case PhysicsMotionType::Dynamic:
                return Result<PhysicsTransformAuthority>::Success(PhysicsTransformAuthority::DynamicSolver);
        }
        return Result<PhysicsTransformAuthority>::Failure(
            MakeError(PhysicsErrors::OperationUnsupported, "Unknown body motion mode has no transform authority."));
    }

    /** @copydoc ValidatePhysicsTransformCommandIdentity */
    Result<void> ValidatePhysicsTransformCommandIdentity(const PhysicsTransformCommandIdentity &identity,
                                                         const PhysicsWorldId expectedWorld, const std::uint64_t expectedSceneGeneration,
                                                         const std::uint64_t expectedSimulationTick) {
        return ValidateCommandIdentity(identity, expectedWorld, expectedSceneGeneration, expectedSimulationTick);
    }

    /** @copydoc ValidatePhysicsStaticTransformCommand */
    Result<void> ValidatePhysicsStaticTransformCommand(const PhysicsStaticTransformCommand &command, const PhysicsWorldId expectedWorld,
                                                       const std::uint64_t expectedSceneGeneration,
                                                       const std::uint64_t expectedSimulationTick) {
        if (const Result<void> identity =
                ValidateCommandIdentity(command.identity, expectedWorld, expectedSceneGeneration, expectedSimulationTick);
            identity.HasError())
            return identity;
        if (const Result<void> pose = ValidatePhysicsPose(command.authoredPose); pose.HasError())
            return pose;
        if (!IsKnownStaticPolicy(command.updatePolicy))
            return Result<void>::Failure(MakeError(PhysicsErrors::OperationUnsupported, "Unknown static transform update policy."));
        return Result<void>::Success();
    }

    /** @copydoc ValidatePhysicsKinematicTargetCommand */
    Result<void> ValidatePhysicsKinematicTargetCommand(const PhysicsKinematicTargetCommand &command, const PhysicsWorldId expectedWorld,
                                                       const std::uint64_t expectedSceneGeneration,
                                                       const std::uint64_t expectedSimulationTick) {
        if (const Result<void> identity =
                ValidateCommandIdentity(command.identity, expectedWorld, expectedSceneGeneration, expectedSimulationTick);
            identity.HasError())
            return identity;
        return ValidatePhysicsPose(command.targetPose);
    }

    /** @copydoc ValidatePhysicsDynamicTransformCommand */
    Result<void> ValidatePhysicsDynamicTransformCommand(const PhysicsDynamicTransformCommand &command, const PhysicsWorldId expectedWorld,
                                                        const std::uint64_t expectedSceneGeneration,
                                                        const std::uint64_t expectedSimulationTick) {
        if (const Result<void> identity =
                ValidateCommandIdentity(command.identity, expectedWorld, expectedSceneGeneration, expectedSimulationTick);
            identity.HasError())
            return identity;
        if (const Result<void> pose = ValidatePhysicsPose(command.targetPose); pose.HasError())
            return pose;
        if (!IsKnownDynamicOperation(command.operation) || !IsKnownVelocityPolicy(command.velocityPolicy))
            return Result<void>::Failure(
                MakeError(PhysicsErrors::OperationUnsupported, "Unknown dynamic transform operation or velocity policy."));
        return Result<void>::Success();
    }

    /** @copydoc ValidatePhysicsDynamicTransformSnapshot */
    Result<void> ValidatePhysicsDynamicTransformSnapshot(const PhysicsDynamicTransformSnapshot &snapshot,
                                                         const PhysicsWorldId expectedWorld, const std::uint64_t expectedSceneGeneration,
                                                         const std::uint64_t expectedCompletedTick) {
        if (expectedSceneGeneration == 0 || expectedCompletedTick == 0 ||
            snapshot.protocolVersion != PhysicsTransformAuthorityProtocolVersion || snapshot.completedTick == 0 ||
            snapshot.sceneGeneration == 0)
            return Result<void>::Failure(MakeError(PhysicsErrors::DescriptorInvalid, "Dynamic transform snapshot metadata is invalid."));
        if (const Result<void> owner = ValidatePhysicsHandleOwner(snapshot.state.body, expectedWorld); owner.HasError())
            return owner;
        if (snapshot.completedTick != expectedCompletedTick || snapshot.sceneGeneration != expectedSceneGeneration)
            return Result<void>::Failure(
                MakeError(PhysicsErrors::QuerySnapshotStale, "Dynamic transform snapshot is not from the completed admission tick."));
        return ValidatePhysicsBodyState(snapshot.state, expectedWorld);
    }

    /** @copydoc ValidatePhysicsDirectTransformWrite */
    Result<void> ValidatePhysicsDirectTransformWrite(const PhysicsDirectTransformWrite &write, const PhysicsWorldId expectedWorld,
                                                     const std::uint64_t expectedSceneGeneration) {
        if (expectedSceneGeneration == 0 || write.protocolVersion != PhysicsTransformAuthorityProtocolVersion || write.sceneGeneration == 0)
            return Result<void>::Failure(MakeError(PhysicsErrors::DescriptorInvalid, "Direct transform write metadata is invalid."));
        if (const Result<void> owner = ValidatePhysicsHandleOwner(write.body, expectedWorld); owner.HasError())
            return owner;
        if (write.sceneGeneration != expectedSceneGeneration)
            return Result<void>::Failure(
                MakeError(PhysicsErrors::CommandOrderInvalid, "Direct transform write targets another scene generation."));
        return ValidatePhysicsPose(write.pose);
    }

    /** @copydoc PhysicsTransformCommandLess */
    bool PhysicsTransformCommandLess(const PhysicsTransformCommand &left, const PhysicsTransformCommand &right) noexcept {
        const auto &leftIdentity = CommandIdentity(left);
        const auto &rightIdentity = CommandIdentity(right);
        return std::tuple{leftIdentity.simulationTick,
                          leftIdentity.body.world.Value(),
                          leftIdentity.body.slot.index,
                          leftIdentity.body.slot.generation,
                          left.index(),
                          leftIdentity.source.Value(),
                          leftIdentity.sourceSequence} < std::tuple{rightIdentity.simulationTick,
                                                                    rightIdentity.body.world.Value(),
                                                                    rightIdentity.body.slot.index,
                                                                    rightIdentity.body.slot.generation,
                                                                    right.index(),
                                                                    rightIdentity.source.Value(),
                                                                    rightIdentity.sourceSequence};
    }

    /** @copydoc PhysicsBodyTransformAuthority::Prepare */
    Result<std::unique_ptr<PhysicsBodyTransformAuthority>> PhysicsBodyTransformAuthority::Prepare(
        const PhysicsTransformAuthorityDescriptor &descriptor) {
        if (const Result<void> valid = ValidateAuthorityDescriptor(descriptor); valid.HasError())
            return Result<std::unique_ptr<PhysicsBodyTransformAuthority>>::Failure(valid.ErrorValue());
        try {
            auto impl = std::make_unique<Impl>(descriptor);
            return Result<std::unique_ptr<PhysicsBodyTransformAuthority>>::Success(
                std::unique_ptr<PhysicsBodyTransformAuthority>{new PhysicsBodyTransformAuthority(std::move(impl))});
        } catch (const std::bad_alloc &) {
            return Result<std::unique_ptr<PhysicsBodyTransformAuthority>>::Failure(
                MakeError(PhysicsErrors::CapacityExceeded, "Unable to allocate transform-authority candidate storage."));
        }
    }

    /** @copydoc PhysicsBodyTransformAuthority::PhysicsBodyTransformAuthority */
    PhysicsBodyTransformAuthority::PhysicsBodyTransformAuthority(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

    /** @copydoc PhysicsBodyTransformAuthority::~PhysicsBodyTransformAuthority */
    PhysicsBodyTransformAuthority::~PhysicsBodyTransformAuthority() {
        Shutdown();
    }

    /** @copydoc PhysicsBodyTransformAuthority::Activate */
    Result<void> PhysicsBodyTransformAuthority::Activate() {
        if (const Result<void> owner = RequireOwner(impl_->ownerThread); owner.HasError())
            return owner;
        if (impl_->state != PhysicsTransformAuthorityState::Prepared)
            return Result<void>::Failure(MakeError(PhysicsErrors::InvalidState));
        impl_->state = PhysicsTransformAuthorityState::Active;
        return Result<void>::Success();
    }

    /** @copydoc PhysicsBodyTransformAuthority::RegisterBody */
    Result<void> PhysicsBodyTransformAuthority::RegisterBody(const PhysicsBodyTransformRegistration &registration) {
        if (const Result<void> owner = RequireOwner(impl_->ownerThread); owner.HasError())
            return owner;
        if (impl_->state != PhysicsTransformAuthorityState::Prepared)
            return Result<void>::Failure(MakeError(PhysicsErrors::InvalidState));
        if (const Result<void> handle = ValidatePhysicsHandleOwner(registration.body, impl_->descriptor.world); handle.HasError())
            return handle;
        if (const Result<void> pose = ValidatePhysicsPose(registration.authoredPose); pose.HasError())
            return pose;
        if (const Result<PhysicsTransformAuthority> authority = ResolvePhysicsTransformAuthority(registration.motion); authority.HasError())
            return Result<void>::Failure(authority.ErrorValue());
        if (FindBody(impl_->bodies, registration.body) != impl_->bodies.end())
            return Result<void>::Failure(MakeError(PhysicsErrors::DescriptorInvalid, "A body is registered twice."));
        if (impl_->bodies.size() >= impl_->descriptor.maximumBodies)
            return Result<void>::Failure(MakeError(PhysicsErrors::CapacityExceeded, "Transform body capacity is full."));

        Impl::BodyRecord record{.registration = registration,
                                .runtimePose = registration.authoredPose,
                                .dynamicState = {.body = registration.body,
                                                 .pose = registration.authoredPose,
                                                 .linearVelocity = {},
                                                 .angularVelocity = {},
                                                 .activity = PhysicsBodyActivity::Awake}};
        impl_->bodies.push_back(std::move(record));
        return Result<void>::Success();
    }

    /** @copydoc PhysicsBodyTransformAuthority::UnregisterBody */
    Result<void> PhysicsBodyTransformAuthority::UnregisterBody(const BodyHandle &body) {
        if (const Result<void> owner = RequireOwner(impl_->ownerThread); owner.HasError())
            return owner;
        if (impl_->state != PhysicsTransformAuthorityState::Prepared)
            return Result<void>::Failure(MakeError(PhysicsErrors::InvalidState));
        if (const Result<void> handle = ValidatePhysicsHandleOwner(body, impl_->descriptor.world); handle.HasError())
            return handle;
        const auto found = FindBody(impl_->bodies, body);
        if (found == impl_->bodies.end())
            return Result<void>::Failure(MakeError(PhysicsErrors::HandleStale));
        impl_->bodies.erase(found);
        return Result<void>::Success();
    }

    /** @copydoc PhysicsBodyTransformAuthority::QueueTransformCommand */
    Result<PhysicsTransformCommandAdmission> PhysicsBodyTransformAuthority::QueueTransformCommand(const PhysicsTransformCommand &command) {
        if (const Result<void> owner = RequireOwner(impl_->ownerThread); owner.HasError())
            return Result<PhysicsTransformCommandAdmission>::Failure(owner.ErrorValue());
        if (impl_->state == PhysicsTransformAuthorityState::Destroyed || impl_->state == PhysicsTransformAuthorityState::Prepared)
            return Result<PhysicsTransformCommandAdmission>::Failure(MakeError(PhysicsErrors::InvalidState));
        if (impl_->applying)
            return Result<PhysicsTransformCommandAdmission>::Failure(MakeError(PhysicsErrors::InvalidState));

        const auto &identity = CommandIdentity(command);
        const Result<void> valid = std::visit([this, &identity](const auto &value) -> Result<void> {
            using Type = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<Type, PhysicsStaticTransformCommand>)
                return ValidatePhysicsStaticTransformCommand(value, impl_->descriptor.world, impl_->descriptor.sceneGeneration,
                                                             identity.simulationTick);
            else if constexpr (std::is_same_v<Type, PhysicsKinematicTargetCommand>)
                return ValidatePhysicsKinematicTargetCommand(value, impl_->descriptor.world, impl_->descriptor.sceneGeneration,
                                                             identity.simulationTick);
            else
                return ValidatePhysicsDynamicTransformCommand(value, impl_->descriptor.world, impl_->descriptor.sceneGeneration,
                                                              identity.simulationTick);
        }, command);
        if (valid.HasError())
            return Result<PhysicsTransformCommandAdmission>::Failure(valid.ErrorValue());
        if (identity.simulationTick <= impl_->lastAppliedTick)
            return Result<PhysicsTransformCommandAdmission>::Failure(
                MakeError(PhysicsErrors::CommandOrderInvalid, "Transform command targets an already applied tick."));
        const auto body = FindBody(impl_->bodies, identity.body);
        if (body == impl_->bodies.end())
            return Result<PhysicsTransformCommandAdmission>::Failure(MakeError(PhysicsErrors::HandleStale));
        if (const Result<void> authority = ValidateCommandAuthority(command, body->registration.motion); authority.HasError())
            return Result<PhysicsTransformCommandAdmission>::Failure(authority.ErrorValue());
        if (std::ranges::any_of(impl_->commands, [&command](const auto &pending) {
            return SameBodyTick(command, pending);
        }))
            return Result<PhysicsTransformCommandAdmission>::Failure(
                MakeError(PhysicsErrors::CommandOrderInvalid, "Only one transform command may target a body in one fixed tick."));
        if (impl_->commands.size() >= impl_->descriptor.maximumPendingCommands)
            return Result<PhysicsTransformCommandAdmission>::Success(
                {PhysicsTransformCommandAdmissionStatus::RejectedFull, static_cast<std::uint32_t>(impl_->commands.size())});

        impl_->commands.push_back(command);
        return Result<PhysicsTransformCommandAdmission>::Success(
            {PhysicsTransformCommandAdmissionStatus::Deferred, static_cast<std::uint32_t>(impl_->commands.size())});
    }

    /** @copydoc PhysicsBodyTransformAuthority::ApplyPreStep */
    Result<PhysicsTransformTickResult> PhysicsBodyTransformAuthority::ApplyPreStep(const std::uint64_t simulationTick) {
        if (const Result<void> owner = RequireOwner(impl_->ownerThread); owner.HasError())
            return Result<PhysicsTransformTickResult>::Failure(owner.ErrorValue());
        if (impl_->state != PhysicsTransformAuthorityState::Active || impl_->applying)
            return Result<PhysicsTransformTickResult>::Failure(MakeError(PhysicsErrors::InvalidState));
        if (simulationTick == 0 || simulationTick != impl_->lastAppliedTick + 1)
            return Result<PhysicsTransformTickResult>::Failure(
                MakeError(PhysicsErrors::CommandOrderInvalid, "Transform pre-step requires the next one-based tick."));

        impl_->applying = true;

        struct ApplyingGuard final {
            bool &value;

            ~ApplyingGuard() {
                value = false;
            }
        } guard{impl_->applying};

        std::ranges::sort(impl_->commands, PhysicsTransformCommandLess);
        if (std::ranges::any_of(impl_->commands, [simulationTick](const auto &command) {
            return CommandIdentity(command).simulationTick < simulationTick;
        }))
            return Result<PhysicsTransformTickResult>::Failure(
                MakeError(PhysicsErrors::CommandOrderInvalid, "A transform command was retained past its consuming tick."));

        PhysicsTransformTickResult result{.simulationTick = simulationTick};
        for (const auto &command : impl_->commands) {
            if (CommandIdentity(command).simulationTick != simulationTick)
                break;
            const auto body = FindBody(impl_->bodies, CommandIdentity(command).body);
            if (body == impl_->bodies.end())
                return Result<PhysicsTransformTickResult>::Failure(MakeError(PhysicsErrors::HandleStale));
            ApplyTransformCommand(command, *body, result);
        }
        impl_->lastAppliedTick = simulationTick;
        std::erase_if(impl_->commands, [simulationTick](const auto &command) {
            return CommandIdentity(command).simulationTick == simulationTick;
        });
        return Result<PhysicsTransformTickResult>::Success(result);
    }

    /** @copydoc PhysicsBodyTransformAuthority::PublishDynamicSnapshot */
    Result<void> PhysicsBodyTransformAuthority::PublishDynamicSnapshot(const PhysicsDynamicTransformSnapshot &snapshot) {
        if (const Result<void> owner = RequireOwner(impl_->ownerThread); owner.HasError())
            return owner;
        if (impl_->state != PhysicsTransformAuthorityState::Active || impl_->applying)
            return Result<void>::Failure(MakeError(PhysicsErrors::InvalidState));
        if (const Result<void> valid = ValidatePhysicsDynamicTransformSnapshot(snapshot, impl_->descriptor.world,
                                                                               impl_->descriptor.sceneGeneration, impl_->lastAppliedTick);
            valid.HasError())
            return valid;
        const auto body = FindBody(impl_->bodies, snapshot.state.body);
        if (body == impl_->bodies.end())
            return Result<void>::Failure(MakeError(PhysicsErrors::HandleStale));
        if (body->registration.motion != PhysicsMotionType::Dynamic)
            return Result<void>::Failure(
                MakeError(PhysicsErrors::TransformAuthorityViolation, "Only dynamic bodies accept solver transform snapshots."));
        if (body->hasDynamicSnapshot && snapshot.completedTick <= body->lastPublishedTick)
            return Result<void>::Failure(MakeError(PhysicsErrors::CommandOrderInvalid,
                                                   "A dynamic transform snapshot may be published only once per completed tick."));
        body->runtimePose = snapshot.state.pose;
        body->dynamicState = snapshot.state;
        body->lastPublishedTick = snapshot.completedTick;
        body->hasDynamicSnapshot = true;
        return Result<void>::Success();
    }

    /** @copydoc PhysicsBodyTransformAuthority::WriteHostTransform */
    Result<void> PhysicsBodyTransformAuthority::WriteHostTransform(const PhysicsDirectTransformWrite &write) {
        if (const Result<void> owner = RequireOwner(impl_->ownerThread); owner.HasError())
            return owner;
        if (impl_->state != PhysicsTransformAuthorityState::Active || impl_->applying)
            return Result<void>::Failure(MakeError(PhysicsErrors::InvalidState));
        if (const Result<void> valid =
                ValidatePhysicsDirectTransformWrite(write, impl_->descriptor.world, impl_->descriptor.sceneGeneration);
            valid.HasError())
            return valid;
        if (FindBody(impl_->bodies, write.body) == impl_->bodies.end())
            return Result<void>::Failure(MakeError(PhysicsErrors::HandleStale));
        return Result<void>::Failure(
            MakeError(PhysicsErrors::TransformAuthorityViolation,
                      "Direct host transform writes are forbidden; submit a safe static/kinematic command or explicit dynamic teleport."));
    }

    /** @copydoc PhysicsBodyTransformAuthority::BodyTransform */
    Result<PhysicsBodyTransformState> PhysicsBodyTransformAuthority::BodyTransform(const BodyHandle &body) const {
        if (const Result<void> owner = RequireOwner(impl_->ownerThread); owner.HasError())
            return Result<PhysicsBodyTransformState>::Failure(owner.ErrorValue());
        if (impl_->state == PhysicsTransformAuthorityState::Destroyed)
            return Result<PhysicsBodyTransformState>::Failure(MakeError(PhysicsErrors::InvalidState));
        if (const Result<void> handle = ValidatePhysicsHandleOwner(body, impl_->descriptor.world); handle.HasError())
            return Result<PhysicsBodyTransformState>::Failure(handle.ErrorValue());
        const auto found = FindBody(impl_->bodies, body);
        if (found == impl_->bodies.end())
            return Result<PhysicsBodyTransformState>::Failure(MakeError(PhysicsErrors::HandleStale));
        return Result<PhysicsBodyTransformState>::Success({.body = found->registration.body,
                                                           .motion = found->registration.motion,
                                                           .authoredPose = found->registration.authoredPose,
                                                           .runtimePose = found->runtimePose,
                                                           .lastAppliedTick = found->lastAppliedTick,
                                                           .lastPublishedTick = found->lastPublishedTick,
                                                           .hasPublishedSnapshot = found->hasDynamicSnapshot});
    }

    /** @copydoc PhysicsBodyTransformAuthority::DynamicSnapshot */
    Result<PhysicsBodyState> PhysicsBodyTransformAuthority::DynamicSnapshot(const BodyHandle &body) const {
        if (const Result<void> owner = RequireOwner(impl_->ownerThread); owner.HasError())
            return Result<PhysicsBodyState>::Failure(owner.ErrorValue());
        if (impl_->state == PhysicsTransformAuthorityState::Destroyed)
            return Result<PhysicsBodyState>::Failure(MakeError(PhysicsErrors::InvalidState));
        if (const Result<void> handle = ValidatePhysicsHandleOwner(body, impl_->descriptor.world); handle.HasError())
            return Result<PhysicsBodyState>::Failure(handle.ErrorValue());
        const auto found = FindBody(impl_->bodies, body);
        if (found == impl_->bodies.end())
            return Result<PhysicsBodyState>::Failure(MakeError(PhysicsErrors::HandleStale));
        if (found->registration.motion != PhysicsMotionType::Dynamic)
            return Result<PhysicsBodyState>::Failure(
                MakeError(PhysicsErrors::TransformAuthorityViolation, "Static and kinematic bodies do not publish dynamic snapshots."));
        if (!found->hasDynamicSnapshot)
            return Result<PhysicsBodyState>::Failure(MakeError(PhysicsErrors::QuerySnapshotStale));
        return Result<PhysicsBodyState>::Success(found->dynamicState);
    }

    /** @copydoc PhysicsBodyTransformAuthority::Shutdown */
    void PhysicsBodyTransformAuthority::Shutdown() noexcept {
        if (impl_->ownerThread != std::this_thread::get_id())
            return;
        if (impl_->state == PhysicsTransformAuthorityState::Destroyed)
            return;
        impl_->commands.clear();
        impl_->bodies.clear();
        impl_->lastAppliedTick = 0;
        impl_->applying = false;
        impl_->state = PhysicsTransformAuthorityState::Destroyed;
    }

    /** @copydoc PhysicsBodyTransformAuthority::State */
    PhysicsTransformAuthorityState PhysicsBodyTransformAuthority::State() const noexcept {
        return impl_->state;
    }

    /** @copydoc PhysicsBodyTransformAuthority::Descriptor */
    const PhysicsTransformAuthorityDescriptor &PhysicsBodyTransformAuthority::Descriptor() const noexcept {
        return impl_->descriptor;
    }

    /** @copydoc PhysicsBodyTransformAuthority::PendingCommandCount */
    std::uint32_t PhysicsBodyTransformAuthority::PendingCommandCount() const noexcept {
        return static_cast<std::uint32_t>(impl_->commands.size());
    }
}  // namespace Horo::Physics
