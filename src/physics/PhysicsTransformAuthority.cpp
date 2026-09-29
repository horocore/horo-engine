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

        /** @brief Returns the first body record whose handle is not less than the requested handle. */
        template <typename Records> [[nodiscard]] auto LowerBoundBody(Records &records, const BodyHandle &body) {
            return std::ranges::lower_bound(records, body, [](const BodyHandle &left, const BodyHandle &right) {
                return left < right;
            }, [](const auto &record) {
                return record.registration.body;
            });
        }

        /** @brief Returns the body record for an exact registered handle from the sorted body table. */
        template <typename Records> [[nodiscard]] auto FindBody(Records &records, const BodyHandle &body) {
            const auto found = LowerBoundBody(records, body);
            return found != records.end() && found->registration.body == body ? found : records.end();
        }

    }  // namespace

    /** @brief Target-private storage for one detached transform-authority candidate. */
    struct PhysicsBodyTransformAuthority::Impl final {
        struct BodyRecord final {
            PhysicsBodyTransformRegistration registration;
            PhysicsPose runtimePose;
            PhysicsBodyState dynamicState;
            PhysicsPose previousPose;
            PhysicsPose currentPose;
            std::uint64_t previousTick{};
            std::uint64_t currentTick{};
            std::uint64_t lastAppliedTick{};
            std::uint64_t lastPublishedTick{};
            bool hasDynamicSnapshot{};
            bool hasCommittedPose{};
            bool hasPreviousTick{};
            bool discontinuous{};
        };

        explicit Impl(const PhysicsTransformAuthorityDescriptor &authorityDescriptor) : descriptor(authorityDescriptor) {
            bodies.reserve(descriptor.maximumBodies);
            commands.reserve(descriptor.maximumPendingCommands);
        }

        PhysicsTransformAuthorityDescriptor descriptor;
        PhysicsTransformAuthorityState state{PhysicsTransformAuthorityState::Prepared};
        std::thread::id ownerThread{std::this_thread::get_id()};
        std::vector<BodyRecord> bodies;
        std::vector<PhysicsTransformCommand> commands;
        std::uint64_t lastAppliedTick{};
        std::uint64_t lastInterpolationTick{};
        std::uint64_t originGeneration{1};
        bool applying{};
    };

    namespace {
        /** @brief Applies one validated command without allocating or touching authored scene storage. */
        void ApplyTransformCommand(const PhysicsTransformCommand &command, auto &record, PhysicsTransformTickResult &result) noexcept {
            record.lastAppliedTick = CommandIdentity(command).simulationTick;
            std::visit([&record, &result]<typename Type>(const Type &value) {  // NOSONAR(cpp:S1188) exhaustive closed-command visitor.
                if constexpr (std::is_same_v<Type, PhysicsStaticTransformCommand>) {
                    record.registration.authoredPose = value.authoredPose;
                    record.runtimePose = value.authoredPose;
                    record.discontinuous = true;
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
                    record.discontinuous = true;
                    ++result.dynamicControls;
                }
            }, command);
            ++result.appliedCommands;
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

        /** @brief Checks one closed command alternative against one resolved transform authority. */
        [[nodiscard]] bool MatchesTransformAuthority(const PhysicsTransformCommand &command, const PhysicsTransformAuthority authority) {
            using enum PhysicsTransformAuthority;
            return std::visit([authority]<typename Type>(const Type &) {
                if constexpr (std::is_same_v<Type, PhysicsStaticTransformCommand>)
                    return authority == StaticScene;
                if constexpr (std::is_same_v<Type, PhysicsKinematicTargetCommand>)
                    return authority == KinematicTarget;
                return authority == DynamicSolver;
            }, command);
        }

        /** @brief Checks one command's body mode against its selected authority operation. */
        Result<void> ValidateCommandAuthority(const PhysicsTransformCommand &command, const PhysicsMotionType motion) {
            const auto authority = ResolvePhysicsTransformAuthority(motion);
            if (authority.HasError())
                return Result<void>::Failure(authority.ErrorValue());
            if (!MatchesTransformAuthority(command, authority.Value()))
                return Result<void>::Failure(MakeError(PhysicsErrors::TransformAuthorityViolation,
                                                       "The transform command does not match the registered body's transform authority."));
            return Result<void>::Success();
        }

        /** @brief Validates one closed command alternative against one receiving admission frame. */
        Result<void> ValidateQueuedTransformCommand(const PhysicsTransformCommand &command, const PhysicsWorldId expectedWorld,
                                                    const std::uint64_t expectedSceneGeneration) {
            return std::visit(
                [expectedWorld, expectedSceneGeneration,
                 simulationTick = CommandIdentity(command).simulationTick]<typename Type>(const Type &value) -> Result<void> {
                if constexpr (std::is_same_v<Type, PhysicsStaticTransformCommand>)
                    return ValidatePhysicsStaticTransformCommand(value, expectedWorld, expectedSceneGeneration, simulationTick);
                else if constexpr (std::is_same_v<Type, PhysicsKinematicTargetCommand>)
                    return ValidatePhysicsKinematicTargetCommand(value, expectedWorld, expectedSceneGeneration, simulationTick);
                else
                    return ValidatePhysicsDynamicTransformCommand(value, expectedWorld, expectedSceneGeneration, simulationTick);
            },
                command);
        }

        /** @brief Requires the owner thread and the pre-activation lifecycle state. */
        [[nodiscard]] Result<void> RequirePrepared(auto &impl) {
            if (const Result<void> owner = RequireOwner(impl.ownerThread); owner.HasError())
                return owner;
            if (impl.state != PhysicsTransformAuthorityState::Prepared)
                return Result<void>::Failure(MakeError(PhysicsErrors::InvalidState));
            return Result<void>::Success();
        }

        /** @brief Requires the owner thread and an idle active lifecycle state. */
        [[nodiscard]] Result<void> RequireActive(auto &impl) {
            if (const Result<void> owner = RequireOwner(impl.ownerThread); owner.HasError())
                return owner;
            if (impl.state != PhysicsTransformAuthorityState::Active || impl.applying)
                return Result<void>::Failure(MakeError(PhysicsErrors::InvalidState));
            return Result<void>::Success();
        }
    }  // namespace

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

    namespace {
        /** @brief Inserts a command into the reserved buffer while preserving canonical admission order. */
        void InsertTransformCommand(std::vector<PhysicsTransformCommand> &commands, const PhysicsTransformCommand &command) {
            const auto insertionPoint = std::ranges::lower_bound(commands, command, PhysicsTransformCommandLess);
            commands.insert(insertionPoint, command);
        }
    }  // namespace

    /** @copydoc PhysicsBodyTransformAuthority::Prepare */
    Result<std::unique_ptr<PhysicsBodyTransformAuthority>> PhysicsBodyTransformAuthority::Prepare(
        const PhysicsTransformAuthorityDescriptor &descriptor) {
        if (const Result<void> valid = ValidateAuthorityDescriptor(descriptor); valid.HasError())
            return Result<std::unique_ptr<PhysicsBodyTransformAuthority>>::Failure(valid.ErrorValue());
        try {
            auto impl = std::make_unique<Impl>(descriptor);
            // The private factory constructor cannot be called through std::make_unique.
            auto authority =
                std::unique_ptr<PhysicsBodyTransformAuthority>{new PhysicsBodyTransformAuthority(std::move(impl))};  // NOSONAR(cpp:S5950)
            return Result<std::unique_ptr<PhysicsBodyTransformAuthority>>::Success(std::move(authority));
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
        if (const Result<void> prepared = RequirePrepared(*impl_); prepared.HasError())
            return prepared;
        impl_->state = PhysicsTransformAuthorityState::Active;
        return Result<void>::Success();
    }

    /** @copydoc PhysicsBodyTransformAuthority::RegisterBody */
    Result<void> PhysicsBodyTransformAuthority::RegisterBody(const PhysicsBodyTransformRegistration &registration) {
        if (const Result<void> prepared = RequirePrepared(*impl_); prepared.HasError())
            return prepared;
        if (const Result<void> handle = ValidatePhysicsHandleOwner(registration.body, impl_->descriptor.world); handle.HasError())
            return handle;
        if (const Result<void> pose = ValidatePhysicsPose(registration.authoredPose); pose.HasError())
            return pose;
        if (const Result<PhysicsTransformAuthority> authority = ResolvePhysicsTransformAuthority(registration.motion); authority.HasError())
            return Result<void>::Failure(authority.ErrorValue());
        const auto insertionPoint = LowerBoundBody(impl_->bodies, registration.body);
        if (insertionPoint != impl_->bodies.end() && insertionPoint->registration.body == registration.body)
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
        impl_->bodies.insert(insertionPoint, std::move(record));
        return Result<void>::Success();
    }

    /** @copydoc PhysicsBodyTransformAuthority::UnregisterBody */
    Result<void> PhysicsBodyTransformAuthority::UnregisterBody(const BodyHandle &body) {
        if (const Result<void> prepared = RequirePrepared(*impl_); prepared.HasError())
            return prepared;
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
        if (const Result<void> active = RequireActive(*impl_); active.HasError())
            return Result<PhysicsTransformCommandAdmission>::Failure(active.ErrorValue());

        const auto &identity = CommandIdentity(command);
        if (const Result<void> valid = ValidateQueuedTransformCommand(command, impl_->descriptor.world, impl_->descriptor.sceneGeneration);
            valid.HasError())
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

        InsertTransformCommand(impl_->commands, command);
        return Result<PhysicsTransformCommandAdmission>::Success(
            {PhysicsTransformCommandAdmissionStatus::Deferred, static_cast<std::uint32_t>(impl_->commands.size())});
    }

    /** @copydoc PhysicsBodyTransformAuthority::ApplyPreStep */
    Result<PhysicsTransformTickResult> PhysicsBodyTransformAuthority::ApplyPreStep(const std::uint64_t simulationTick) {
        if (const Result<void> active = RequireActive(*impl_); active.HasError())
            return Result<PhysicsTransformTickResult>::Failure(active.ErrorValue());
        if (simulationTick == 0 || simulationTick != impl_->lastAppliedTick + 1)
            return Result<PhysicsTransformTickResult>::Failure(
                MakeError(PhysicsErrors::CommandOrderInvalid, "Transform pre-step requires the next one-based tick."));

        impl_->applying = true;

        struct ApplyingGuard final {
            explicit ApplyingGuard(bool &reference) noexcept : value(reference) {}

            ApplyingGuard(const ApplyingGuard &) = delete;
            ApplyingGuard &operator=(const ApplyingGuard &) = delete;
            ApplyingGuard(ApplyingGuard &&) = delete;
            ApplyingGuard &operator=(ApplyingGuard &&) = delete;

            bool &value;

            ~ApplyingGuard() {
                value = false;
            }
        };

        ApplyingGuard guard{impl_->applying};

        if (!impl_->commands.empty() && CommandIdentity(impl_->commands.front()).simulationTick < simulationTick)
            return Result<PhysicsTransformTickResult>::Failure(
                MakeError(PhysicsErrors::CommandOrderInvalid, "A transform command was retained past its consuming tick."));

        PhysicsTransformTickResult result{.simulationTick = simulationTick};
        auto currentEnd = impl_->commands.begin();
        while (currentEnd != impl_->commands.end() && CommandIdentity(*currentEnd).simulationTick == simulationTick) {
            const auto &command = *currentEnd;
            const auto body = FindBody(impl_->bodies, CommandIdentity(command).body);
            if (body == impl_->bodies.end())
                return Result<PhysicsTransformTickResult>::Failure(MakeError(PhysicsErrors::HandleStale));
            ApplyTransformCommand(command, *body, result);
            ++currentEnd;
        }
        impl_->lastAppliedTick = simulationTick;
        impl_->commands.erase(impl_->commands.begin(), currentEnd);
        return Result<PhysicsTransformTickResult>::Success(result);
    }

    /** @copydoc PhysicsBodyTransformAuthority::PublishDynamicSnapshot */
    Result<void> PhysicsBodyTransformAuthority::PublishDynamicSnapshot(const PhysicsDynamicTransformSnapshot &snapshot) {
        if (const Result<void> active = RequireActive(*impl_); active.HasError())
            return active;
        if (snapshot.completedTick <= impl_->lastInterpolationTick)
            return Result<void>::Failure(MakeError(PhysicsErrors::CommandOrderInvalid, "Completed interpolation ticks are immutable."));
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

    /** @copydoc PhysicsBodyTransformAuthority::CommitInterpolationTick */
    Result<void> PhysicsBodyTransformAuthority::CommitInterpolationTick(const std::uint64_t completedTick) {
        if (const auto active = RequireActive(*impl_); active.HasError())
            return active;
        if (completedTick == 0 || completedTick != impl_->lastAppliedTick || completedTick <= impl_->lastInterpolationTick)
            return Result<void>::Failure(
                MakeError(PhysicsErrors::CommandOrderInvalid, "Interpolation requires the latest uncommitted applied tick."));
        for (const auto &body : impl_->bodies) {
            if (body.registration.motion == PhysicsMotionType::Dynamic &&
                (!body.hasDynamicSnapshot || body.lastPublishedTick != completedTick))
                return Result<void>::Failure(MakeError(PhysicsErrors::QuerySnapshotStale,
                                                       "Every dynamic body requires completed solver evidence before pose publication."));
            if (const auto pose = ValidatePhysicsPose(body.runtimePose); pose.HasError())
                return pose;
        }
        for (auto &body : impl_->bodies) {
            body.hasPreviousTick = body.hasCommittedPose && !body.discontinuous && body.currentTick + 1 == completedTick;
            body.previousPose = body.hasPreviousTick ? body.currentPose : body.runtimePose;
            body.previousTick = body.hasPreviousTick ? body.currentTick : completedTick;
            body.currentPose = body.runtimePose;
            body.currentTick = completedTick;
            body.hasCommittedPose = true;
            body.discontinuous = false;
        }
        impl_->lastInterpolationTick = completedTick;
        return Result<void>::Success();
    }

    /** @copydoc PhysicsBodyTransformAuthority::InterpolationEndpoints */
    Result<PhysicsInterpolationEndpoints> PhysicsBodyTransformAuthority::InterpolationEndpoints(const BodyHandle &body) const {
        if (const auto active = RequireActive(*impl_); active.HasError())
            return Result<PhysicsInterpolationEndpoints>::Failure(active.ErrorValue());
        if (const auto handle = ValidatePhysicsHandleOwner(body, impl_->descriptor.world); handle.HasError())
            return Result<PhysicsInterpolationEndpoints>::Failure(handle.ErrorValue());
        const auto found = FindBody(impl_->bodies, body);
        if (found == impl_->bodies.end())
            return Result<PhysicsInterpolationEndpoints>::Failure(MakeError(PhysicsErrors::HandleStale));
        if (!found->hasCommittedPose)
            return Result<PhysicsInterpolationEndpoints>::Failure(MakeError(PhysicsErrors::QuerySnapshotStale));
        return Result<PhysicsInterpolationEndpoints>::Success({.body = body,
                                                               .sceneGeneration = impl_->descriptor.sceneGeneration,
                                                               .originGeneration = impl_->originGeneration,
                                                               .previousPose = found->previousPose,
                                                               .currentPose = found->currentPose,
                                                               .previousTick = found->previousTick,
                                                               .currentTick = found->currentTick,
                                                               .hasPreviousTick = found->hasPreviousTick});
    }

    /** @copydoc PhysicsBodyTransformAuthority::ResetInterpolationHistory */
    Result<void> PhysicsBodyTransformAuthority::ResetInterpolationHistory(const PhysicsInterpolationResetReason reason) {
        using enum PhysicsInterpolationResetReason;
        if (const auto active = RequireActive(*impl_); active.HasError())
            return active;
        if (impl_->lastAppliedTick != impl_->lastInterpolationTick)
            return Result<void>::Failure(
                MakeError(PhysicsErrors::InvalidState, "Interpolation reset requires a completed-tick safe point."));
        if (reason != Teleport && reason != Restore && reason != Reload)
            return Result<void>::Failure(MakeError(PhysicsErrors::OperationUnsupported, "Unknown interpolation reset reason."));
        for (auto &body : impl_->bodies) {
            body.previousPose = body.currentPose;
            body.previousTick = body.currentTick;
            body.hasPreviousTick = false;
            body.hasCommittedPose = false;
            body.discontinuous = true;
        }
        return Result<void>::Success();
    }

    /** @copydoc PhysicsBodyTransformAuthority::RebaseInterpolationHistory */
    Result<void> PhysicsBodyTransformAuthority::RebaseInterpolationHistory(const Math::Vec3 delta,
                                                                           const std::uint64_t newOriginGeneration) {
        if (const auto active = RequireActive(*impl_); active.HasError())
            return active;
        if (impl_->lastAppliedTick != impl_->lastInterpolationTick || !impl_->commands.empty())
            return Result<void>::Failure(
                MakeError(PhysicsErrors::InvalidState, "Origin rebase requires a completed tick and no queued transform commands."));
        if (!Math::IsFinite(delta) || newOriginGeneration <= impl_->originGeneration)
            return Result<void>::Failure(
                MakeError(PhysicsErrors::DescriptorInvalid, "Origin rebase requires finite delta and a new generation."));
        for (const auto &body : impl_->bodies) {
            if (!Math::IsFinite(body.runtimePose.translation - delta) || !Math::IsFinite(body.dynamicState.pose.translation - delta) ||
                (body.hasCommittedPose &&
                 (!Math::IsFinite(body.previousPose.translation - delta) || !Math::IsFinite(body.currentPose.translation - delta))))
                return Result<void>::Failure(MakeError(PhysicsErrors::DescriptorInvalid, "Origin rebase would produce a non-finite pose."));
        }
        for (auto &body : impl_->bodies) {
            body.runtimePose.translation = body.runtimePose.translation - delta;
            body.dynamicState.pose.translation = body.dynamicState.pose.translation - delta;
            if (body.hasCommittedPose) {
                body.previousPose.translation = body.previousPose.translation - delta;
                body.currentPose.translation = body.currentPose.translation - delta;
            }
        }
        impl_->originGeneration = newOriginGeneration;
        return Result<void>::Success();
    }

    /** @copydoc PhysicsBodyTransformAuthority::WriteHostTransform */
    Result<void> PhysicsBodyTransformAuthority::WriteHostTransform(const PhysicsDirectTransformWrite &write) {
        if (const Result<void> active = RequireActive(*impl_); active.HasError())
            return active;
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
