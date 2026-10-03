#include "PhysicsWorldContainmentInternal.h"
#include "PhysicsWorldInternal.h"

#include <limits>

namespace Horo::Physics {
    namespace {
        /** @brief Forwards one copied native observation into the owner-thread bounded projection. */
        bool CaptureCanonicalContact(Detail::PhysicsEventProjection *context, const PhysicsContactObservation &observation) noexcept {
            return context != nullptr && context->TryCapture(observation);
        }

        /** @brief Builds borrowed category views for the synchronous bounded copy. */
        [[nodiscard]] PhysicsDebugSource DebugSource(const PhysicsWorldId world, const PhysicsPublishedTick &published,
                                                     const Detail::CanonicalDebugProjection &projected,
                                                     const std::span<const PhysicsDebugRecord> contacts,
                                                     const std::span<const PhysicsDebugRecord> pipeline,
                                                     const std::size_t truncatedContacts) {
            using enum PhysicsDebugCategory;
            using enum PhysicsDebugAvailability;
            PhysicsDebugSource source{.world = world,
                                      .simulationTick = published.completedTick,
                                      .publicationRevision = published.publicationRevision};
            source.categories[static_cast<std::size_t>(Body)] = {.availability = Available,
                                                                 .records = projected.bodies,
                                                                 .truncatedBeforeCapture = projected.truncatedBodies};
            source.categories[static_cast<std::size_t>(Shape)] = {.availability = Available,
                                                                  .records = projected.shapes,
                                                                  .truncatedBeforeCapture = projected.truncatedShapes};
            source.categories[static_cast<std::size_t>(Contact)] = {.availability = Available,
                                                                    .records = contacts,
                                                                    .truncatedBeforeCapture = truncatedContacts,
                                                                    .droppedBeforeCapture = published.droppedEventCount};
            source.categories[static_cast<std::size_t>(Constraint)] = {.availability = Available,
                                                                       .records = projected.constraints,
                                                                       .truncatedBeforeCapture = projected.truncatedConstraints};
            source.categories[static_cast<std::size_t>(Pipeline)] = {.availability = Available, .records = pipeline};
            return source;
        }

    }  // namespace

    namespace {
        /** @brief Validates all tick admission gates before mutable safe-point work begins. */
        [[nodiscard]] Result<void> CheckTickAdmission(const auto &impl, const PhysicsFixedTickInput &input) {
            if (const auto ready = Detail::CheckReadyForTick(impl); ready.HasError())
                return ready;
            if (const auto capacity = impl.CheckPublicationRevisionCapacity(); capacity.HasError())
                return capacity;
            if (const auto valid = Detail::ValidateTickInput(impl, input); valid.HasError())
                return valid;
            return Detail::ValidateSolverJobs(impl.runtime->solverJobs, input.solverJobs);
        }

        /** @brief Runs the joined native step and translates callback diagnostics into world state. */
        [[nodiscard]] Result<Detail::CanonicalStepOutcome> StepCanonicalWorldForTick(auto &impl, const PhysicsFixedTickInput &input) {
            impl.queryEvents.events.BeginTick(input.simulationTick);
            const Detail::CanonicalContactSink contactSink{.context = &impl.queryEvents.events, .append = CaptureCanonicalContact};
            const auto stepped = Detail::StepCanonicalWorld(impl.native, static_cast<float>(impl.settings.Values().world.fixedDeltaSeconds),
                                                            input.simulationTick, contactSink);
            if (stepped.HasError()) {
                impl.queryEvents.events.AbortTick();
                impl.Fail(stepped.ErrorValue(), input.sceneGeneration, input.simulationTick);
                return Result<Detail::CanonicalStepOutcome>::Failure(stepped.ErrorValue());
            }
            if (stepped.Value().diagnostic.has_value())
                impl.RecordDiagnostic(*stepped.Value().diagnostic, input.sceneGeneration, input.simulationTick);
            return stepped;
        }

        /** @brief Rejects invalid or duplicate body mutations before queue ownership transfers. */
        [[nodiscard]] Result<void> ValidateBodyMutationAdmission(const auto &impl, const PhysicsStructuralCommand &command) {
            if (!command.bodyMutation)
                return Result<void>::Success();
            if (impl.stepping)
                return Result<void>::Failure(
                    MakeError(PhysicsErrors::InvalidState, "Body mutations must be admitted between fixed ticks."));
            if (const std::array matches{command.order.commandKind == PhysicsStructuralCommandKind::Change,
                                         command.order.targetKind == PhysicsCommandTargetKind::Body,
                                         command.order.targetIdentity ==
                                             static_cast<std::uint64_t>(command.bodyMutation->body.slot.index) + 1U};
                !std::ranges::all_of(matches, std::identity{}))
                return Result<void>::Failure(
                    MakeError(PhysicsErrors::CommandOrderInvalid, "Body mutation order key must name its exact body slot."));
            if (const auto resolved = Detail::ResolveCanonicalBodyMutation(impl.native, impl.identity, *command.bodyMutation);
                resolved.HasError())
                return Result<void>::Failure(resolved.ErrorValue());
            for (std::uint32_t index = 0; index < impl.commandCount; ++index) {
                const auto &existing = impl.CommandAt(index);
                if (existing.bodyMutation && existing.order.simulationTick == command.order.simulationTick &&
                    existing.bodyMutation->body == command.bodyMutation->body)
                    return Result<void>::Failure(
                        MakeError(PhysicsErrors::CommandOrderInvalid, "A body already has a mutation for this tick."));
            }
            return Result<void>::Success();
        }

        /** @brief Validates command identity and complete mutation intent before queue transfer. */
        [[nodiscard]] Result<void> ValidateQueuedCommand(const auto &impl, const PhysicsStructuralCommand &command) {
            if (const auto valid = ValidatePhysicsCommandOrderKey(command.order); valid.HasError())
                return valid;
            if (const auto mutation = ValidateBodyMutationAdmission(impl, command); mutation.HasError())
                return mutation;
            if (const auto completed = impl.stepping ? impl.activeTick : impl.publication.Snapshot().completedTick;
                command.order.simulationTick <= completed || command.order.worldGeneration != impl.identity.Value())
                return Result<void>::Failure(MakeError(PhysicsErrors::CommandOrderInvalid,
                                                       "Physics commands cannot target a completed tick or another world generation."));
            return Result<void>::Success();
        }

        /** @brief Revalidates the complete frame before applying ordered pre-step mutations. */
        [[nodiscard]] Result<void> RunCanonicalPreStep(auto &impl, const PhysicsFixedTickInput &input, const std::uint32_t eligible,
                                                       std::uint32_t &applied) {
            using enum PhysicsTickPhase;
            for (std::uint32_t index = 0; index < eligible; ++index) {
                const auto &command = impl.CommandAt(index);
                if (!command.bodyMutation)
                    continue;
                if (const auto resolved = Detail::ResolveCanonicalBodyMutation(impl.native, impl.identity, *command.bodyMutation);
                    resolved.HasError())
                    return Result<void>::Failure(resolved.ErrorValue());
            }
            Detail::ObservePhase(input, ApplyDeferredPreStep);
            for (std::uint32_t index = 0; index < eligible; ++index) {
                const auto &command = impl.CommandAt(index);
                if (!command.bodyMutation)
                    continue;
                if (const auto changed = Detail::ApplyCanonicalBodyMutation(impl.native, impl.identity, *command.bodyMutation);
                    changed.HasError()) {
                    impl.Fail(changed.ErrorValue(), input.sceneGeneration, input.simulationTick);
                    return changed;
                }
            }
            Detail::ObserveCommands(impl, input, eligible, PhysicsStructuralCommandKind::Create, PhysicsCommandSafePoint::PreStep, applied);
            Detail::ObservePhase(input, CopyKinematicTargets);
            Detail::ObservePhase(input, ApplyDynamicInputs);
            Detail::ObservePhase(input, BroadPhase);
            Detail::ObservePhase(input, ContactGeneration);
            Detail::ObservePhase(input, ConstraintSolve);
            return Result<void>::Success();
        }

        /** @brief Finishes event projection and publishes only a completed tick. */
        [[nodiscard]] Result<void> RunCanonicalPostStep(auto &impl, const PhysicsFixedTickInput &input, std::uint32_t &applied,
                                                        const std::span<const BodyHandle> quarantined) {
            using enum PhysicsTickPhase;
            Detail::ObservePhase(input, IntegrateBodies);
            Detail::ObservePhase(input, WriteRuntimeTransforms);
            const Result<Detail::PhysicsEventProjectionResult> eventResult = Detail::CompleteEventProjection(impl, input);
            if (eventResult.HasError()) {
                impl.Fail(eventResult.ErrorValue(), input.sceneGeneration, input.simulationTick);
                return Result<void>::Failure(eventResult.ErrorValue());
            }
            Detail::ObservePhase(input, ProduceEvents);
            Detail::ObservePhase(input, ApplyDeferredPostStep);
            Detail::SuppressQuarantinedCommands(impl, quarantined);
            const auto remainingFrame = Detail::ValidateCommandFrame(impl, input);
            if (remainingFrame.HasError())
                return Result<void>::Failure(remainingFrame.ErrorValue());
            Detail::ObserveCommands(impl, input, remainingFrame.Value(), PhysicsStructuralCommandKind::Destroy,
                                    PhysicsCommandSafePoint::PostStep, applied);
            impl.DiscardCommands(remainingFrame.Value());
            impl.querySceneGeneration = input.sceneGeneration;
            Detail::CommitPublishedTick(impl, input.simulationTick, applied, eventResult.Value());
            impl.statistics.completedTicks = input.simulationTick;
            Detail::ObservePhase(input, PublishCompletedTick);
            return Result<void>::Success();
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
                                      capability == Constraints || capability == ImmediateQueries || capability == BodyMutation);
        const auto ready = static_cast<std::uint8_t>(impl_->state == PhysicsRuntimeState::Ready);
        return static_cast<PhysicsCapabilitySupport>(static_cast<std::uint8_t>(PhysicsCapabilitySupport::Unsupported) +
                                                     canonicalWorld * (1U + ready));
    }

    /** @copydoc PhysicsWorld::PhysicsWorld */
    PhysicsWorld::PhysicsWorld(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

    /** @copydoc PhysicsWorld::SetQuarantineSink */
    void PhysicsWorld::SetQuarantineSink(QuarantineSink &sink) const noexcept {
        impl_->containment.quarantineSink = {&sink};
    }

    /** @copydoc PhysicsWorld::InjectNonFiniteBodyForTesting */
    bool PhysicsWorld::InjectNonFiniteBodyForTesting(const BodyHandle body, const float value, const std::uint8_t component,
                                                     const bool postStep) const noexcept {
        return impl_->state == PhysicsWorldState::ActiveSolver &&
               Detail::InjectCanonicalNonFiniteBodyForTesting(impl_->native, body, value, component, postStep);
    }

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
            impl_->diagnostics.lastFailure.reset();
            impl_->diagnostics.lastDiagnostic.reset();
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
        return impl_->diagnostics.lastFailure;
    }

    /** @copydoc PhysicsWorld::LastDiagnostic */
    const std::optional<PhysicsDiagnosticRecord> &PhysicsWorld::LastDiagnostic() const noexcept {
        return impl_->diagnostics.lastDiagnostic;
    }

    /** @copydoc PhysicsWorld::QueueStructuralCommand */
    Result<PhysicsCommandAdmission> PhysicsWorld::QueueStructuralCommand(const PhysicsStructuralCommand &command) {
        if (impl_->runtime->ownerThread != std::this_thread::get_id())
            return Result<PhysicsCommandAdmission>::Failure(MakeError(PhysicsErrors::ThreadAffinityViolation));
        if (impl_->state == PhysicsWorldState::ActiveNull)
            return Result<PhysicsCommandAdmission>::Failure(MakeError(PhysicsErrors::CapabilityUnavailable));
        if (impl_->state != PhysicsWorldState::ActiveSolver || impl_->runtime->state != PhysicsRuntimeState::Ready)
            return Result<PhysicsCommandAdmission>::Failure(MakeError(PhysicsErrors::InvalidState));
        if (const auto valid = ValidateQueuedCommand(*impl_, command); valid.HasError()) {
            impl_->RecordMutationDiagnostic(valid.ErrorValue(), command);
            return Result<PhysicsCommandAdmission>::Failure(valid.ErrorValue());
        }
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
        impl_->statistics.pendingCommands = impl_->commandCount - static_cast<std::uint32_t>(impl_->containment.retiredCommands.size());
        impl_->statistics.maximumCommandDepth = std::max(impl_->statistics.maximumCommandDepth, impl_->commandCount);
        return Result<PhysicsCommandAdmission>::Success({PhysicsCommandAdmissionStatus::Deferred, impl_->commandCount});
    }

    /** @copydoc PhysicsWorld::ReadSceneBodyPolicy */
    Result<PhysicsBodyDescriptor> PhysicsWorld::ReadSceneBodyPolicy(const BodyHandle body) const {
        if (impl_->runtime->ownerThread != std::this_thread::get_id())
            return Result<PhysicsBodyDescriptor>::Failure(MakeError(PhysicsErrors::ThreadAffinityViolation));
        if (impl_->state == PhysicsWorldState::ActiveNull)
            return Result<PhysicsBodyDescriptor>::Failure(MakeError(PhysicsErrors::CapabilityUnavailable));
        if (impl_->state != PhysicsWorldState::ActiveSolver || impl_->runtime->state != PhysicsRuntimeState::Ready || impl_->stepping)
            return Result<PhysicsBodyDescriptor>::Failure(MakeError(PhysicsErrors::InvalidState));
        return Detail::ReadCanonicalSceneBodyPolicy(impl_->native, impl_->identity, body);
    }

    /** @copydoc PhysicsWorld::ReadSceneBodyReconciliation */
    Result<PhysicsBodyReconciliation> PhysicsWorld::ReadSceneBodyReconciliation(const BodyHandle body) const {
        if (impl_->runtime->ownerThread != std::this_thread::get_id())
            return Result<PhysicsBodyReconciliation>::Failure(MakeError(PhysicsErrors::ThreadAffinityViolation));
        if (impl_->state == PhysicsWorldState::ActiveNull)
            return Result<PhysicsBodyReconciliation>::Failure(MakeError(PhysicsErrors::CapabilityUnavailable));
        if (impl_->state != PhysicsWorldState::ActiveSolver || impl_->runtime->state != PhysicsRuntimeState::Ready || impl_->stepping)
            return Result<PhysicsBodyReconciliation>::Failure(MakeError(PhysicsErrors::InvalidState));
        return Detail::ReadCanonicalSceneBodyReconciliation(impl_->native, impl_->identity, body);
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
        if (const auto capacity = impl_->CheckPublicationRevisionCapacity(); capacity.HasError())
            return Result<PhysicsQueryFixture>::Failure(capacity.ErrorValue());
        auto created = Detail::CreateCanonicalQueryFixture(impl_->native, impl_->identity, fixture);
        if (created.HasValue())
            impl_->InvalidateQueryEventPublication();
        return created;
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
        if (const auto capacity = impl_->CheckPublicationRevisionCapacity(); capacity.HasError())
            return capacity;
        auto destroyed = Detail::DestroyCanonicalQueryFixture(impl_->native, fixture);
        if (destroyed.HasValue())
            impl_->InvalidateQueryEventPublication();
        return destroyed;
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
        if (const auto admitted = CheckTickAdmission(*impl_, input); admitted.HasError())
            return admitted;

        impl_->containment.quarantined.clear();
        if (const auto contained = Detail::ContainNonFiniteBodies(*impl_, input, impl_->containment.quarantined, false);
            contained.HasError())
            return contained;
        Detail::SuppressQuarantinedCommands(*impl_, impl_->containment.quarantined);

        impl_->stepping = true;
        const Detail::BooleanResetGuard stepGuard{impl_->stepping};
        impl_->activeTick = input.simulationTick;

        Detail::CanonicalizeCommands(*impl_);
        const Result<std::uint32_t> frame = Detail::ValidateCommandFrame(*impl_, input);
        if (frame.HasError())
            return Result<void>::Failure(frame.ErrorValue());
        const std::uint32_t eligible = frame.Value();
        std::uint32_t applied{};
        if (const auto preStep = RunCanonicalPreStep(*impl_, input, eligible, applied); preStep.HasError())
            return preStep;

        if (const Result<void> jobs = Detail::RunInjectedSolverJobs(*impl_, input); jobs.HasError())
            return jobs;

        if (const auto stepped = StepCanonicalWorldForTick(*impl_, input); stepped.HasError())
            return Result<void>::Failure(stepped.ErrorValue());

        if (const auto contained = Detail::ContainNonFiniteBodies(*impl_, input, impl_->containment.quarantined); contained.HasError())
            return contained;

        return RunCanonicalPostStep(*impl_, input, applied, impl_->containment.quarantined);
    }

    /** @copydoc PhysicsWorld::PublishedTick */
    PhysicsPublishedTick PhysicsWorld::PublishedTick() const noexcept {
        return impl_->publication.Snapshot();
    }

    /** @copydoc PhysicsWorld::CaptureDebugSnapshot */
    Result<std::shared_ptr<const PhysicsDebugSnapshot>> PhysicsWorld::CaptureDebugSnapshot(const PhysicsDebugBudget &budget) const {
        using SnapshotResult = Result<std::shared_ptr<const PhysicsDebugSnapshot>>;
        if (impl_->runtime->ownerThread != std::this_thread::get_id())
            return SnapshotResult::Failure(MakeError(PhysicsErrors::ThreadAffinityViolation));
        if (impl_->state != PhysicsWorldState::ActiveSolver || impl_->stepping || impl_->runtime->state != PhysicsRuntimeState::Ready)
            return SnapshotResult::Failure(MakeError(PhysicsErrors::InvalidState));
        const PhysicsPublishedTick published = impl_->publication.Snapshot();
        if (published.completedTick == 0)
            return SnapshotResult::Failure(MakeError(PhysicsErrors::QuerySnapshotStale));
        try {
            const Detail::CanonicalDebugProjection projected = Detail::ProjectCanonicalDebug(impl_->native, budget);
            const auto events = impl_->queryEvents.events.PublishedEvents();
            const auto &contactLimit = budget.categories[static_cast<std::size_t>(PhysicsDebugCategory::Contact)];
            const std::size_t contactCapacity = std::min<std::size_t>(
                {events.size(), contactLimit.maximumRecords, contactLimit.maximumPayloadBytes / sizeof(PhysicsDebugRecord),
                 budget.maximumPayloadBytes / sizeof(PhysicsDebugRecord), MaximumPhysicsDebugRecords});
            std::vector<PhysicsDebugRecord> contacts;
            contacts.reserve(contactCapacity);
            for (std::size_t index = 0; index < contactCapacity; ++index)
                contacts.emplace_back(PhysicsDebugContact{events[index]});
            const std::array<PhysicsDebugRecord, 1> pipeline{
                PhysicsDebugPipeline{published.appliedCommands, published.eventCount, published.droppedEventCount}};
            const PhysicsDebugSource source =
                DebugSource(impl_->identity, published, projected, contacts, pipeline, events.size() - contacts.size());
            return CapturePhysicsDebugSnapshot(source, published, budget);
        } catch (const std::bad_alloc &) {
            return SnapshotResult::Failure(MakeError(PhysicsErrors::CapacityExceeded, "Unable to project bounded Physics debug evidence."));
        }
    }

    /** @copydoc PhysicsWorld::TickStatistics */
    PhysicsTickStatistics PhysicsWorld::TickStatistics() const noexcept {
        return impl_->statistics;
    }
}  // namespace Horo::Physics
