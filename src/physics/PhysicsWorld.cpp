#include "PhysicsWorldInternal.h"

#include <limits>

namespace Horo::Physics {
    namespace {
        /** @brief Forwards one copied native observation into the owner-thread bounded projection. */
        bool CaptureCanonicalContact(Detail::PhysicsEventProjection *context, const PhysicsContactObservation &observation) noexcept {
            return context != nullptr && context->TryCapture(observation);
        }

    }  // namespace

    namespace {
        /** @brief Runs the joined native step and translates callback diagnostics into world state. */
        [[nodiscard]] Result<Detail::CanonicalStepOutcome> StepCanonicalWorldForTick(auto &impl, const PhysicsFixedTickInput &input) {
            impl.events.BeginTick(input.simulationTick);
            const Detail::CanonicalContactSink contactSink{.context = &impl.events, .append = CaptureCanonicalContact};
            const auto stepped = Detail::StepCanonicalWorld(impl.native, static_cast<float>(impl.settings.Values().world.fixedDeltaSeconds),
                                                            input.simulationTick, contactSink);
            if (stepped.HasError()) {
                impl.events.AbortTick();
                impl.Fail(stepped.ErrorValue(), input.sceneGeneration, input.simulationTick);
                return Result<Detail::CanonicalStepOutcome>::Failure(stepped.ErrorValue());
            }
            if (stepped.Value().diagnostic.has_value())
                impl.RecordDiagnostic(*stepped.Value().diagnostic, input.sceneGeneration, input.simulationTick);
            return stepped;
        }
    }  // namespace

    /** @copydoc PhysicsRuntime::Create */
    Result<std::unique_ptr<PhysicsRuntime>> PhysicsRuntime::Create(const PhysicsRuntimeMode mode, JobSystem *solverJobs) {
        if (mode > PhysicsRuntimeMode::Null)
            return Result<std::unique_ptr<PhysicsRuntime>>::Failure(
                MakeError(PhysicsErrors::OperationUnsupported, "Unknown Physics runtime composition."));
        try {
            auto impl = std::make_shared<Impl>(mode, solverJobs);
            if (mode == PhysicsRuntimeMode::Canonical) {
                const auto native = Detail::CreateCanonicalRuntime();
                if (native.HasError()) {
                    impl->state = PhysicsRuntimeState::Failed;
                    return Result<std::unique_ptr<PhysicsRuntime>>::Failure(native.ErrorValue());
                }
                impl->native = native.Value();
            }
            return Result<std::unique_ptr<PhysicsRuntime>>::Success(std::unique_ptr<PhysicsRuntime>{new PhysicsRuntime(std::move(impl))});
        } catch (const std::bad_alloc &) {
            return Result<std::unique_ptr<PhysicsRuntime>>::Failure(
                MakeError(PhysicsErrors::CapacityExceeded, "Unable to allocate Physics runtime ownership state."));
        }
    }

    /** @copydoc PhysicsRuntime::~PhysicsRuntime */
    PhysicsRuntime::~PhysicsRuntime() {
        Shutdown();
    }

    /** @copydoc PhysicsRuntime::PrepareWorld */
    Result<std::unique_ptr<PhysicsWorld>> PhysicsRuntime::PrepareWorld(const PhysicsWorldSettings &settings) {
        if (impl_->ownerThread != std::this_thread::get_id())
            return Result<std::unique_ptr<PhysicsWorld>>::Failure(MakeError(PhysicsErrors::ThreadAffinityViolation));
        if (const std::array admissionConditions{
                impl_->state == PhysicsRuntimeState::Ready,
            };
            !std::ranges::all_of(admissionConditions, std::identity{}))
            return Result<std::unique_ptr<PhysicsWorld>>::Failure(MakeError(PhysicsErrors::InvalidState));
        try {
            using enum PhysicsWorldState;
            auto worldImpl = std::make_unique<PhysicsWorld::Impl>(impl_, settings);
            if (impl_->mode == PhysicsRuntimeMode::Canonical) {
                const auto created = Detail::CreateCanonicalWorld(impl_->native, settings);
                if (created.HasError()) {
                    worldImpl->state = Failed;
                    return Result<std::unique_ptr<PhysicsWorld>>::Failure(created.ErrorValue());
                }
                worldImpl->native = created.Value();
                worldImpl->state = PreparedSolver;
            } else {
                worldImpl->state = PreparedNull;
            }
            return Result<std::unique_ptr<PhysicsWorld>>::Success(std::unique_ptr<PhysicsWorld>{new PhysicsWorld(std::move(worldImpl))});
        } catch (const std::bad_alloc &) {
            return Result<std::unique_ptr<PhysicsWorld>>::Failure(
                MakeError(PhysicsErrors::CapacityExceeded, "Unable to allocate Physics world ownership state."));
        }
    }

    /** @copydoc PhysicsRuntime::IssueWorldIdentity */
    Result<PhysicsWorldId> PhysicsRuntime::IssueWorldIdentity() {  // NOSONAR: issuing an identity mutates the runtime authority behind its
                                                                   // pimpl.
        if (impl_->ownerThread != std::this_thread::get_id())
            return Result<PhysicsWorldId>::Failure(MakeError(PhysicsErrors::ThreadAffinityViolation));
        if (impl_->state != PhysicsRuntimeState::Ready)
            return Result<PhysicsWorldId>::Failure(MakeError(PhysicsErrors::InvalidState));
        if (impl_->nextWorldIdentity == 0)
            return Result<PhysicsWorldId>::Failure(MakeError(PhysicsErrors::GenerationExhausted));
        const std::uint64_t issued = impl_->nextWorldIdentity;
        impl_->nextWorldIdentity = issued == std::numeric_limits<std::uint64_t>::max() ? 0 : issued + 1;
        return PhysicsWorldId::Create(issued);
    }

    /** @copydoc PhysicsRuntime::Shutdown */
    void PhysicsRuntime::Shutdown() noexcept {
        impl_->state = PhysicsRuntimeState::Stopped;
        impl_->ReleaseNativeWhenIdle();
    }

    /** @copydoc PhysicsRuntime::Mode */
    PhysicsRuntimeMode PhysicsRuntime::Mode() const noexcept {
        return impl_->mode;
    }

    /** @copydoc PhysicsRuntime::State */
    PhysicsRuntimeState PhysicsRuntime::State() const noexcept {
        return impl_->state;
    }

    /** @copydoc PhysicsRuntime::Availability */
    PhysicsAvailability PhysicsRuntime::Availability() const noexcept {
        const auto canonical = static_cast<std::uint8_t>(impl_->mode == PhysicsRuntimeMode::Canonical);
        const auto ready = static_cast<std::uint8_t>(impl_->state == PhysicsRuntimeState::Ready);
        return static_cast<PhysicsAvailability>(canonical * (static_cast<std::uint8_t>(PhysicsAvailability::Unavailable) + ready));
    }

    /** @copydoc PhysicsRuntime::Capability */
    PhysicsCapabilitySupport PhysicsRuntime::Capability(const PhysicsCapability capability) const noexcept {
        using enum PhysicsCapability;
        if (capability >= Count)
            return PhysicsCapabilitySupport::Unknown;
        const auto canonicalWorld =
            static_cast<std::uint8_t>(impl_->mode == PhysicsRuntimeMode::Canonical) *
            static_cast<std::uint8_t>(capability == WorldCreation || capability == RigidBodies || capability == ImmutableShapes ||
                                      capability == Constraints || capability == ImmediateQueries);
        const auto ready = static_cast<std::uint8_t>(impl_->state == PhysicsRuntimeState::Ready);
        return static_cast<PhysicsCapabilitySupport>(static_cast<std::uint8_t>(PhysicsCapabilitySupport::Unsupported) +
                                                     canonicalWorld * (1U + ready));
    }

    /** @copydoc PhysicsWorld::PhysicsWorld */
    PhysicsWorld::PhysicsWorld(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

    /** @copydoc PhysicsWorld::~PhysicsWorld */
    PhysicsWorld::~PhysicsWorld() = default;

    /** @copydoc PhysicsWorld::Activate */
    Result<void> PhysicsWorld::Activate(const PhysicsWorldId identity) {
        const auto state = static_cast<std::uint8_t>(impl_->state);
        const auto firstPrepared = static_cast<std::uint8_t>(PhysicsWorldState::PreparedSolver);
        const auto prepared =
            static_cast<std::uint8_t>(state - firstPrepared) <= static_cast<std::uint8_t>(PhysicsWorldState::PreparedNull) - firstPrepared;
        if (impl_->runtime->ownerThread != std::this_thread::get_id())
            return Result<void>::Failure(MakeError(PhysicsErrors::ThreadAffinityViolation));
        if (const std::array admissionConditions{prepared, impl_->runtime->state == PhysicsRuntimeState::Ready};
            !std::ranges::all_of(admissionConditions, std::identity{}))
            return Result<void>::Failure(MakeError(PhysicsErrors::InvalidState));
        if (!identity.IsValid())
            return Result<void>::Failure(MakeError(PhysicsErrors::WorldInvalid));
        if (std::ranges::any_of(impl_->runtime->identities, [identity](const auto *existing) {
            return *existing == identity;
        }))
            return Result<void>::Failure(
                MakeError(PhysicsErrors::WorldInvalid, "The world generation is already active in this Physics runtime."));
        impl_->identity = identity;
        impl_->state = static_cast<PhysicsWorldState>(state + 2U);
        return Result<void>::Success();
    }

    /** @copydoc PhysicsWorld::Reset */
    Result<void> PhysicsWorld::Reset() {
        using enum PhysicsWorldState;
        if (impl_->runtime->ownerThread != std::this_thread::get_id())
            return Result<void>::Failure(MakeError(PhysicsErrors::ThreadAffinityViolation));
        if (impl_->runtime->state != PhysicsRuntimeState::Ready || impl_->state == Destroyed || impl_->stepping)
            return Result<void>::Failure(MakeError(PhysicsErrors::InvalidState));
        if (impl_->state == PreparedSolver || impl_->state == PreparedNull) {
            impl_->lifecycleCause = PhysicsWorldLifecycleCause::Reset;
            impl_->lastFailure.reset();
            impl_->lastDiagnostic.reset();
            return Result<void>::Success();
        }

        return impl_->Reinitialize();
    }

    /** @copydoc PhysicsWorld::UnloadScene */
    Result<void> PhysicsWorld::UnloadScene() {
        if (impl_->runtime->ownerThread != std::this_thread::get_id())
            return Result<void>::Failure(MakeError(PhysicsErrors::ThreadAffinityViolation));
        impl_->Retire(PhysicsWorldLifecycleCause::SceneUnload);
        return Result<void>::Success();
    }

    /** @copydoc PhysicsWorld::Shutdown */
    void PhysicsWorld::Shutdown() noexcept {
        impl_->Retire(PhysicsWorldLifecycleCause::ProcessShutdown);
    }

    /** @copydoc PhysicsWorld::State */
    PhysicsWorldState PhysicsWorld::State() const noexcept {
        return impl_->state;
    }

    /** @copydoc PhysicsWorld::Identity */
    PhysicsWorldId PhysicsWorld::Identity() const noexcept {
        return impl_->identity;
    }

    /** @copydoc PhysicsWorld::Settings */
    const PhysicsWorldSettings &PhysicsWorld::Settings() const noexcept {
        return impl_->settings;
    }

    /** @copydoc PhysicsWorld::LifecycleCause */
    PhysicsWorldLifecycleCause PhysicsWorld::LifecycleCause() const noexcept {
        return impl_->lifecycleCause;
    }

    /** @copydoc PhysicsWorld::LastFailure */
    const std::optional<Error> &PhysicsWorld::LastFailure() const noexcept {
        return impl_->lastFailure;
    }

    /** @copydoc PhysicsWorld::LastDiagnostic */
    const std::optional<PhysicsDiagnosticRecord> &PhysicsWorld::LastDiagnostic() const noexcept {
        return impl_->lastDiagnostic;
    }

    /** @copydoc PhysicsWorld::QueueStructuralCommand */
    Result<PhysicsCommandAdmission> PhysicsWorld::QueueStructuralCommand(const PhysicsStructuralCommand &command) {
        if (impl_->runtime->ownerThread != std::this_thread::get_id())
            return Result<PhysicsCommandAdmission>::Failure(MakeError(PhysicsErrors::ThreadAffinityViolation));
        if (impl_->state == PhysicsWorldState::ActiveNull)
            return Result<PhysicsCommandAdmission>::Failure(MakeError(PhysicsErrors::CapabilityUnavailable));
        if (impl_->state != PhysicsWorldState::ActiveSolver)
            return Result<PhysicsCommandAdmission>::Failure(MakeError(PhysicsErrors::InvalidState));
        if (const Result<void> valid = ValidatePhysicsCommandOrderKey(command.order); valid.HasError())
            return Result<PhysicsCommandAdmission>::Failure(valid.ErrorValue());
        if (const std::uint64_t completedOrActiveTick = impl_->stepping ? impl_->activeTick : impl_->published.completedTick;
            command.order.simulationTick <= completedOrActiveTick || command.order.worldGeneration != impl_->identity.Value())
            return Result<PhysicsCommandAdmission>::Failure(
                MakeError(PhysicsErrors::CommandOrderInvalid,
                          "Physics commands cannot target a completed tick or another world generation."));
        const auto capacity = static_cast<std::uint32_t>(impl_->commands.size());
        if (capacity == 0)
            return Result<PhysicsCommandAdmission>::Failure(
                MakeError(PhysicsErrors::InvalidState, "Validated Physics command storage is unexpectedly unavailable."));
        const bool destruction = command.order.commandKind == PhysicsStructuralCommandKind::Destroy;
        if (const auto ordinaryLimit = capacity - 1; impl_->commandCount >= (destruction ? capacity : ordinaryLimit))
            return Result<PhysicsCommandAdmission>::Success(Detail::RejectFullCommand(*impl_, destruction));

        const auto tail = (impl_->commandHead + impl_->commandCount) % capacity;
        impl_->commands[tail] = command;
        ++impl_->commandCount;
        impl_->commandOrderDirty = true;
        ++impl_->statistics.admittedCommands;
        impl_->statistics.pendingCommands = impl_->commandCount;
        impl_->statistics.maximumCommandDepth = std::max(impl_->statistics.maximumCommandDepth, impl_->commandCount);
        return Result<PhysicsCommandAdmission>::Success({PhysicsCommandAdmissionStatus::Deferred, impl_->commandCount});
    }

    /** @copydoc PhysicsWorld::CreateQueryFixture */
    Result<PhysicsQueryFixture> PhysicsWorld::CreateQueryFixture(const PhysicsQueryFixtureDescriptor &fixture) const {
        if (impl_->runtime->ownerThread != std::this_thread::get_id())
            return Result<PhysicsQueryFixture>::Failure(MakeError(PhysicsErrors::ThreadAffinityViolation));
        if (impl_->state == PhysicsWorldState::ActiveNull)
            return Result<PhysicsQueryFixture>::Failure(MakeError(PhysicsErrors::CapabilityUnavailable));
        if (impl_->state != PhysicsWorldState::ActiveSolver || impl_->stepping)
            return Result<PhysicsQueryFixture>::Failure(MakeError(PhysicsErrors::InvalidState));
        if (const Result<void> valid = ValidatePhysicsQueryFixtureDescriptor(fixture, impl_->identity); valid.HasError())
            return Result<PhysicsQueryFixture>::Failure(valid.ErrorValue());
        return Detail::CreateCanonicalQueryFixture(impl_->native, impl_->identity, fixture);
    }

    /** @copydoc PhysicsWorld::DestroyQueryFixture */
    Result<void> PhysicsWorld::DestroyQueryFixture(const PhysicsQueryFixture &fixture) const {
        if (impl_->runtime->ownerThread != std::this_thread::get_id())
            return Result<void>::Failure(MakeError(PhysicsErrors::ThreadAffinityViolation));
        if (impl_->state == PhysicsWorldState::ActiveNull)
            return Result<void>::Failure(MakeError(PhysicsErrors::CapabilityUnavailable));
        if (impl_->state != PhysicsWorldState::ActiveSolver || impl_->stepping)
            return Result<void>::Failure(MakeError(PhysicsErrors::InvalidState));
        if (const auto body = ValidatePhysicsHandleOwner(fixture.body, impl_->identity); body.HasError())
            return body;
        if (const auto shape = ValidatePhysicsHandleOwner(fixture.shape, impl_->identity); shape.HasError())
            return shape;
        return Detail::DestroyCanonicalQueryFixture(impl_->native, fixture);
    }

    /** @copydoc PhysicsWorld::Query */
    Result<PhysicsQueryResult> PhysicsWorld::Query(const PhysicsQueryDescriptor &descriptor, const std::span<PhysicsQueryHit> hits) const {
        if (impl_->runtime->ownerThread != std::this_thread::get_id())
            return Result<PhysicsQueryResult>::Failure(MakeError(PhysicsErrors::ThreadAffinityViolation));
        if (impl_->state == PhysicsWorldState::ActiveNull)
            return Result<PhysicsQueryResult>::Failure(MakeError(PhysicsErrors::CapabilityUnavailable));
        if (impl_->state != PhysicsWorldState::ActiveSolver || impl_->stepping)
            return Result<PhysicsQueryResult>::Failure(MakeError(PhysicsErrors::InvalidState));
        if (const Result<void> valid = ValidatePhysicsQueryDescriptor(descriptor, impl_->identity, impl_->querySceneGeneration);
            valid.HasError())
            return Result<PhysicsQueryResult>::Failure(valid.ErrorValue());
        if (hits.size() > MaximumPhysicsQueryHits)
            return Result<PhysicsQueryResult>::Failure(MakeError(PhysicsErrors::CapacityExceeded));
        return Detail::ExecuteCanonicalQuery(impl_->native, descriptor, hits);
    }

    /** @copydoc PhysicsWorld::AdvanceFixedTick */
    Result<void> PhysicsWorld::AdvanceFixedTick(const PhysicsFixedTickInput &input) {
        using enum PhysicsTickPhase;
        if (const Result<void> ready = Detail::CheckReadyForTick(*impl_); ready.HasError())
            return ready;
        if (const Result<void> validInput = Detail::ValidateTickInput(*impl_, input); validInput.HasError())
            return validInput;
        if (const Result<void> jobs = Detail::ValidateSolverJobs(impl_->runtime->solverJobs, input.solverJobs); jobs.HasError())
            return jobs;

        impl_->stepping = true;
        const Detail::BooleanResetGuard stepGuard{impl_->stepping};
        impl_->activeTick = input.simulationTick;

        Detail::CanonicalizeCommands(*impl_);
        const Result<std::uint32_t> frame = Detail::ValidateCommandFrame(*impl_, input);
        if (frame.HasError())
            return Result<void>::Failure(frame.ErrorValue());
        const std::uint32_t eligible = frame.Value();
        std::uint32_t applied{};
        Detail::ObservePhase(input, ApplyDeferredPreStep);
        Detail::ObserveCommands(*impl_, input, eligible, PhysicsStructuralCommandKind::Create, PhysicsCommandSafePoint::PreStep, applied);
        Detail::ObservePhase(input, CopyKinematicTargets);
        Detail::ObservePhase(input, ApplyDynamicInputs);
        Detail::ObservePhase(input, BroadPhase);
        Detail::ObservePhase(input, ContactGeneration);
        Detail::ObservePhase(input, ConstraintSolve);

        if (const Result<void> jobs = Detail::RunInjectedSolverJobs(*impl_, input); jobs.HasError())
            return jobs;

        if (const auto stepped = StepCanonicalWorldForTick(*impl_, input); stepped.HasError())
            return Result<void>::Failure(stepped.ErrorValue());

        Detail::ObservePhase(input, IntegrateBodies);
        Detail::ObservePhase(input, WriteRuntimeTransforms);
        const Result<Detail::PhysicsEventProjectionResult> eventResult = Detail::CompleteEventProjection(*impl_, input);
        if (eventResult.HasError()) {
            impl_->Fail(eventResult.ErrorValue(), input.sceneGeneration, input.simulationTick);
            return Result<void>::Failure(eventResult.ErrorValue());
        }
        Detail::ObservePhase(input, ProduceEvents);
        Detail::ObservePhase(input, ApplyDeferredPostStep);
        Detail::ObserveCommands(*impl_, input, eligible, PhysicsStructuralCommandKind::Destroy, PhysicsCommandSafePoint::PostStep, applied);

        impl_->DiscardCommands(eligible);
        impl_->querySceneGeneration = input.sceneGeneration;
        Detail::CommitPublishedTick(*impl_, input.simulationTick, applied, eventResult.Value());
        impl_->statistics.completedTicks = input.simulationTick;
        Detail::ObservePhase(input, PublishCompletedTick);
        return Result<void>::Success();
    }

    /** @copydoc PhysicsWorld::PublishedTick */
    PhysicsPublishedTick PhysicsWorld::PublishedTick() const noexcept {
        Detail::PublicationGuard publicationGuard{impl_->publicationLock};
        return impl_->published;
    }

    /** @copydoc PhysicsWorld::CaptureDebugSnapshot */
    Result<std::shared_ptr<const PhysicsDebugSnapshot>> PhysicsWorld::CaptureDebugSnapshot(const PhysicsDebugBudget &budget) const {
        using SnapshotResult = Result<std::shared_ptr<const PhysicsDebugSnapshot>>;
        if (impl_->runtime->ownerThread != std::this_thread::get_id())
            return SnapshotResult::Failure(MakeError(PhysicsErrors::ThreadAffinityViolation));
        if (impl_->state != PhysicsWorldState::ActiveSolver || impl_->stepping || impl_->runtime->state != PhysicsRuntimeState::Ready)
            return SnapshotResult::Failure(MakeError(PhysicsErrors::InvalidState));
        if (impl_->published.completedTick == 0)
            return SnapshotResult::Failure(MakeError(PhysicsErrors::QuerySnapshotStale));
        try {
            const Detail::CanonicalDebugProjection projected = Detail::ProjectCanonicalDebug(impl_->native, budget);
            const auto events = impl_->events.PublishedEvents();
            const auto &contactLimit = budget.categories[static_cast<std::size_t>(PhysicsDebugCategory::Contact)];
            const std::size_t contactCapacity = std::min<std::size_t>(
                {events.size(), contactLimit.maximumRecords, contactLimit.maximumPayloadBytes / sizeof(PhysicsDebugRecord),
                 budget.maximumPayloadBytes / sizeof(PhysicsDebugRecord), MaximumPhysicsDebugRecords});
            std::vector<PhysicsDebugRecord> contacts;
            contacts.reserve(contactCapacity);
            for (std::size_t index = 0; index < contactCapacity; ++index)
                contacts.emplace_back(PhysicsDebugContact{events[index]});
            const std::array<PhysicsDebugRecord, 1> pipeline{
                PhysicsDebugPipeline{impl_->published.appliedCommands, impl_->published.eventCount, impl_->published.droppedEventCount}};
            PhysicsDebugSource source{.world = impl_->identity,
                                      .simulationTick = impl_->published.completedTick,
                                      .publicationRevision = impl_->published.publicationRevision};
            source.categories[static_cast<std::size_t>(PhysicsDebugCategory::Body)] = {.availability = PhysicsDebugAvailability::Available,
                                                                                       .records = projected.bodies,
                                                                                       .truncatedBeforeCapture = projected.truncatedBodies};
            source.categories[static_cast<std::size_t>(PhysicsDebugCategory::Shape)] = {.availability = PhysicsDebugAvailability::Available,
                                                                                        .records = projected.shapes,
                                                                                        .truncatedBeforeCapture =
                                                                                            projected.truncatedShapes};
            source.categories[static_cast<std::size_t>(PhysicsDebugCategory::Contact)] = {.availability =
                                                                                              PhysicsDebugAvailability::Available,
                                                                                          .records = contacts,
                                                                                          .truncatedBeforeCapture =
                                                                                              events.size() - contacts.size(),
                                                                                          .droppedBeforeCapture =
                                                                                              impl_->published.droppedEventCount};
            source.categories[static_cast<std::size_t>(PhysicsDebugCategory::Constraint)] = {.availability =
                                                                                                 PhysicsDebugAvailability::Available,
                                                                                             .records = projected.constraints,
                                                                                             .truncatedBeforeCapture =
                                                                                                 projected.truncatedConstraints};
            source.categories[static_cast<std::size_t>(PhysicsDebugCategory::Pipeline)] = {.availability =
                                                                                               PhysicsDebugAvailability::Available,
                                                                                           .records = pipeline};
            return CapturePhysicsDebugSnapshot(source, impl_->published, budget);
        } catch (const std::bad_alloc &) {
            return SnapshotResult::Failure(MakeError(PhysicsErrors::CapacityExceeded, "Unable to project bounded Physics debug evidence."));
        }
    }

    /** @copydoc PhysicsWorld::TickStatistics */
    PhysicsTickStatistics PhysicsWorld::TickStatistics() const noexcept {
        return impl_->statistics;
    }
}  // namespace Horo::Physics
