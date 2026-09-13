#include "Horo/Physics/PhysicsWorld.h"

#include "CanonicalPhysicsRuntime.h"
#include "Horo/Foundation/JobSystem.h"
#include "Horo/Physics/PhysicsErrors.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <functional>
#include <limits>
#include <new>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

namespace Horo::Physics {
    namespace {
        /** @brief Provides non-throwing mutual exclusion for the bounded publication snapshot copy. */
        class PublicationGuard final {
        public:
            explicit PublicationGuard(std::atomic_flag &lock) noexcept : lock_(lock) {
                while (lock_.test_and_set())
                    std::this_thread::yield();
            }

            PublicationGuard(const PublicationGuard &) = delete;
            PublicationGuard &operator=(const PublicationGuard &) = delete;

            ~PublicationGuard() {
                lock_.clear();
            }

        private:
            std::atomic_flag &lock_;
        };
    }  // namespace

    /** @brief Shared only by the process wrapper and its worlds; identity pointers are owner-thread, stable-address registrations. */
    struct PhysicsRuntime::Impl final {
        explicit Impl(const PhysicsRuntimeMode selectedMode, JobSystem *solverJobSystem)
            : mode(selectedMode), solverJobs(solverJobSystem) {}

        Impl(const Impl &) = delete;
        Impl &operator=(const Impl &) = delete;

        ~Impl() {
            Detail::DestroyCanonicalRuntime(native);
        }

        void ReleaseNativeWhenIdle() noexcept {
            if (const std::array releaseConditions{state == PhysicsRuntimeState::Stopped, identities.empty()};
                !std::ranges::all_of(releaseConditions, std::identity{}))
                return;
            Detail::DestroyCanonicalRuntime(native);
            native = {};
        }

        PhysicsRuntimeMode mode;
        PhysicsRuntimeState state{PhysicsRuntimeState::Ready};
        std::thread::id ownerThread{std::this_thread::get_id()};
        Detail::CanonicalRuntimeHandle native;
        JobSystem *solverJobs{};
        std::vector<const PhysicsWorldId *> identities;
        std::uint64_t nextWorldIdentity{1};
    };

    /** @brief Owns one candidate's settings/native state and unregisters its identity before releasing the runtime lease. */
    struct PhysicsWorld::Impl final {
        Impl(std::shared_ptr<PhysicsRuntime::Impl> runtimeOwner, const PhysicsWorldSettings &worldSettings)
            : runtime(std::move(runtimeOwner)), settings(worldSettings), commands(worldSettings.Values().budgets.maximumCommands),
              sourceOrder(worldSettings.Values().budgets.maximumCommands) {
            runtime->identities.push_back(&identity);
        }

        Impl(const Impl &) = delete;
        Impl &operator=(const Impl &) = delete;

        ~Impl() {
            Retire(PhysicsWorldLifecycleCause::ProcessShutdown);
        }

        void Retire(const PhysicsWorldLifecycleCause cause) noexcept {
            if (state == PhysicsWorldState::Destroyed)
                return;
            Detail::DestroyCanonicalWorld(native);
            native = {};
            state = PhysicsWorldState::Destroyed;
            lifecycleCause = cause;
            lastFailure.reset();
            std::ranges::fill(commands, PhysicsStructuralCommand{});
            commandHead = 0;
            commandCount = 0;
            statistics.pendingCommands = 0;
            std::erase(runtime->identities, &identity);
            runtime->ReleaseNativeWhenIdle();
        }

        void Fail(Error error) {
            state = PhysicsWorldState::Failed;
            lifecycleCause = PhysicsWorldLifecycleCause::FatalSolverError;
            lastFailure = std::move(error);
        }

        void ClearForReset() noexcept {
            identity = {};
            std::ranges::fill(commands, PhysicsStructuralCommand{});
            commandHead = 0;
            commandCount = 0;
            activeTick = 0;
            commandOrderDirty = false;
            stepping = false;
            {
                PublicationGuard publicationGuard{publicationLock};
                published = {};
            }
            statistics = {};
            lastFailure.reset();
            lifecycleCause = PhysicsWorldLifecycleCause::Reset;
        }

        [[nodiscard]] Result<void> Reinitialize() {
            using enum PhysicsWorldState;
            Detail::DestroyCanonicalWorld(native);
            native = {};
            ClearForReset();
            if (runtime->mode == PhysicsRuntimeMode::Null) {
                state = PreparedNull;
                return Result<void>::Success();
            }

            try {
                const Result<Detail::CanonicalWorldHandle> created = Detail::CreateCanonicalWorld(runtime->native, settings);
                if (created.HasError()) {
                    state = Failed;
                    lastFailure = created.ErrorValue();
                    return Result<void>::Failure(created.ErrorValue());
                }
                native = created.Value();
                state = PreparedSolver;
                return Result<void>::Success();
            } catch (const std::bad_alloc &) {
                Error error = MakeError(PhysicsErrors::CapacityExceeded, "Unable to rebuild Physics world ownership state during reset.");
                state = Failed;
                lastFailure = error;
                return Result<void>::Failure(std::move(error));
            }
        }

        [[nodiscard]] PhysicsStructuralCommand &CommandAt(const std::uint32_t offset) noexcept {
            return commands[(commandHead + offset) % commands.size()];
        }

        [[nodiscard]] const PhysicsStructuralCommand &CommandAt(const std::uint32_t offset) const noexcept {
            return commands[(commandHead + offset) % commands.size()];
        }

        void DiscardCommands(const std::uint32_t discarded) noexcept {
            if (discarded == 0)
                return;
            commandHead = (commandHead + discarded) % commands.size();
            commandCount -= discarded;
            statistics.pendingCommands = commandCount;
        }

        std::shared_ptr<PhysicsRuntime::Impl> runtime;
        PhysicsWorldSettings settings;
        PhysicsWorldState state{PhysicsWorldState::Preparing};
        PhysicsWorldId identity;
        Detail::CanonicalWorldHandle native;
        std::vector<PhysicsStructuralCommand> commands;
        std::vector<std::uint32_t> sourceOrder;
        std::uint32_t commandHead{};
        std::uint32_t commandCount{};
        std::uint64_t activeTick{};
        bool commandOrderDirty{};
        bool stepping{};
        // The owner thread alone writes publication state; any live-world thread may take a coherent snapshot.
        // The flag protects only the bounded copy below and owns no worker or shutdown lifetime.
        mutable std::atomic_flag publicationLock = ATOMIC_FLAG_INIT;
        PhysicsPublishedTick published;
        PhysicsTickStatistics statistics;
        PhysicsWorldLifecycleCause lifecycleCause{PhysicsWorldLifecycleCause::None};
        std::optional<Error> lastFailure;
    };

    namespace {
        /** @brief Restores one owner-thread boolean state when a guarded scope exits. */
        class BooleanResetGuard final {
        public:
            explicit BooleanResetGuard(bool &state) noexcept : state_(state) {}

            BooleanResetGuard(const BooleanResetGuard &) = delete;
            BooleanResetGuard &operator=(const BooleanResetGuard &) = delete;

            ~BooleanResetGuard() {
                state_ = false;
            }

        private:
            bool &state_;
        };

        /** @brief Records bounded-buffer rejection metrics and preserves explicit destruction retry ownership. */
        [[nodiscard]] PhysicsCommandAdmission RejectFullCommand(auto &impl, const bool destruction) noexcept {
            ++impl.statistics.rejectedCommands;
            if (!destruction)
                return {PhysicsCommandAdmissionStatus::RejectedFull, impl.commandCount};
            ++impl.statistics.destructionRetryCount;
            return {PhysicsCommandAdmissionStatus::DestructionRetryRequired, impl.commandCount};
        }

        /** @brief Replaces every externally visible publication domain under one synchronization boundary. */
        void CommitPublishedTick(auto &impl, const std::uint64_t tick, const std::uint32_t appliedCommands) noexcept {
            PublicationGuard publicationGuard{impl.publicationLock};
            const std::uint64_t revision = impl.published.publicationRevision + 1;
            impl.published = {.completedTick = tick,
                              .publicationRevision = revision,
                              .transformTick = tick,
                              .queryTick = tick,
                              .eventTick = tick,
                              .appliedCommands = appliedCommands};
        }

        /** @brief Normalizes and canonicalizes retained commands without frame-hot allocation. */
        void CanonicalizeCommands(auto &impl) noexcept {
            if (!impl.commandOrderDirty)
                return;
            if (impl.commandHead != 0) {
                std::ranges::rotate(impl.commands, impl.commands.begin() + impl.commandHead);
                impl.commandHead = 0;
            }
            std::ranges::sort(impl.commands.begin(), impl.commands.begin() + impl.commandCount,
                              [](const PhysicsStructuralCommand &left, const PhysicsStructuralCommand &right) {
                return PhysicsCommandOrderLess(left.order, right.order);
            });
            impl.commandOrderDirty = false;
        }

        /** @brief Validates one complete tick frame after canonicalization and before observation. */
        [[nodiscard]] Result<std::uint32_t> ValidateCommandFrame(auto &impl, const PhysicsFixedTickInput &input) {
            std::uint32_t eligible{};
            while (eligible < impl.commandCount && impl.CommandAt(eligible).order.simulationTick == input.simulationTick)
                ++eligible;
            if (eligible > impl.settings.Values().budgets.maximumCommandsPerTick)
                return Result<std::uint32_t>::Failure(
                    MakeError(PhysicsErrors::CapacityExceeded, "The canonical Physics command frame exceeds its admitted tick budget."));
            for (std::uint32_t offset = 0; offset < eligible; ++offset) {
                if (const PhysicsCommandOrderKey &key = impl.CommandAt(offset).order;
                    key.worldGeneration != impl.identity.Value() || key.sceneGeneration != input.sceneGeneration)
                    return Result<std::uint32_t>::Failure(
                        MakeError(PhysicsErrors::CommandOrderInvalid, "A Physics command targets a stale world or scene generation."));
                impl.sourceOrder[offset] = offset;
            }
            std::ranges::sort(impl.sourceOrder.begin(), impl.sourceOrder.begin() + eligible,
                              [&impl](const std::uint32_t left, const std::uint32_t right) {
                const PhysicsCommandOrderKey &leftKey = impl.CommandAt(left).order;
                const PhysicsCommandOrderKey &rightKey = impl.CommandAt(right).order;
                return std::tie(leftKey.source, leftKey.sourceSequence) < std::tie(rightKey.source, rightKey.sourceSequence);
            });
            for (std::uint32_t offset = 0; offset < eligible; ++offset) {
                const PhysicsCommandOrderKey &key = impl.CommandAt(impl.sourceOrder[offset]).order;
                const bool startsSource = offset == 0 || impl.CommandAt(impl.sourceOrder[offset - 1]).order.source != key.source;
                if (startsSource && key.sourceSequence != 1)
                    return Result<std::uint32_t>::Failure(
                        MakeError(PhysicsErrors::CommandOrderInvalid, "A Physics command source sequence has a missing predecessor."));
                if (!startsSource) {
                    const std::uint64_t previousSequence = impl.CommandAt(impl.sourceOrder[offset - 1]).order.sourceSequence;
                    if (previousSequence == std::numeric_limits<std::uint64_t>::max() || key.sourceSequence != previousSequence + 1)
                        return Result<std::uint32_t>::Failure(
                            MakeError(PhysicsErrors::CommandOrderInvalid, "A Physics command source sequence has a missing predecessor."));
                }
            }
            return Result<std::uint32_t>::Success(eligible);
        }

        /** @brief Emits one optional synchronous phase observation on the owner thread. */
        void ObservePhase(const PhysicsFixedTickInput &input, const PhysicsTickPhase phase) noexcept {
            if (input.observer.phase)
                input.observer.phase(input.observer.context, phase, input.simulationTick);
        }

        /** @brief Emits eligible commands selected for one semantic safe point without mutating the queue. */
        void ObserveCommands(const auto &impl, const PhysicsFixedTickInput &input, const std::uint32_t eligible,
                             const PhysicsStructuralCommandKind selectedKind, const PhysicsCommandSafePoint safePoint,
                             std::uint32_t &applied) noexcept {
            using enum PhysicsStructuralCommandKind;
            for (std::uint32_t offset = 0; offset < eligible; ++offset) {
                const PhysicsStructuralCommand &command = impl.CommandAt(offset);
                if (const bool selected =
                        selectedKind == Destroy ? command.order.commandKind == Destroy : command.order.commandKind != Destroy;
                    !selected)
                    continue;
                if (input.observer.command)
                    input.observer.command(input.observer.context, command, safePoint, input.simulationTick);
                ++applied;
            }
        }

        /** @brief Validates one solver-neutral batch before any tick phase is observed. */
        [[nodiscard]] Result<void> ValidateSolverJobs(const JobSystem *jobs, const PhysicsSolverJobBatch &batch) {
            if (batch.jobCount == 0)
                return Result<void>::Success();
            if (jobs == nullptr)
                return Result<void>::Failure(
                    MakeError(PhysicsErrors::CapabilityUnavailable, "This Physics runtime has no injected solver job system."));
            if (batch.jobs == nullptr || batch.jobCount > MaximumPhysicsSolverJobsPerTick || batch.joinTimeout <= Duration{})
                return Result<void>::Failure(
                    MakeError(PhysicsErrors::DescriptorInvalid, "Solver jobs require valid bounded storage and a positive join timeout."));
            for (std::uint32_t index = 0; index < batch.jobCount; ++index) {
                if (batch.jobs[index].execute == nullptr)
                    return Result<void>::Failure(
                        MakeError(PhysicsErrors::DescriptorInvalid, "Every solver job requires an executable callback."));
            }
            return Result<void>::Success();
        }

        /** @brief Reports whether two errors preserve the same stable domain/code identity. */
        [[nodiscard]] bool SameErrorCode(const Error &left, const Error &right) noexcept {
            return left.domain.Value() == right.domain.Value() && left.code.Value() == right.code.Value();
        }

        /** @brief Dispatches one validated solver-neutral batch and drains it before the tick may continue. */
        [[nodiscard]] Result<void> RunSolverJobs(JobSystem &jobs, const PhysicsSolverJobBatch &batch) {
            // Admit the complete bounded batch before observing failures so a fast
            // child cannot make later admission depend on worker scheduling.
            TaskGroup group(jobs, TaskGroupFailurePolicy::CollectAll);
            for (std::uint32_t index = 0; index < batch.jobCount; ++index) {
                const PhysicsSolverJob job = batch.jobs[index];
                const auto spawned = group.Spawn({}, [job](const CancellationToken &cancellation) {
                    return job.execute(job.context, cancellation);
                });
                if (spawned.HasError())
                    return Result<void>::Failure(spawned.ErrorValue());
            }

            const Result<void> joined = group.Join({.waitPolicy = WaitPolicy::OwnerThreadBlockAllowed, .timeout = batch.joinTimeout});
            if (joined.HasValue())
                return joined;
            group.RequestCancel();
            if (const Result<void> drained = group.Join({.waitPolicy = WaitPolicy::OwnerThreadBlockAllowed,
                                                         .timeout = Duration::FromNanoseconds(std::numeric_limits<std::int64_t>::max())});
                drained.HasError() && SameErrorCode(joined.ErrorValue(), drained.ErrorValue()))
                return joined;
            return Result<void>::Failure(MakeError(PhysicsErrors::SolverDeadlineExceeded));
        }

        /** @brief Checks mutable owner affinity and lifecycle admission for one fixed tick. */
        [[nodiscard]] Result<void> CheckReadyForTick(const auto &impl) {
            if (impl.runtime->ownerThread != std::this_thread::get_id())
                return Result<void>::Failure(MakeError(PhysicsErrors::ThreadAffinityViolation));
            if (impl.state == PhysicsWorldState::ActiveNull)
                return Result<void>::Failure(MakeError(PhysicsErrors::CapabilityUnavailable));
            if (impl.state != PhysicsWorldState::ActiveSolver || impl.stepping)
                return Result<void>::Failure(MakeError(PhysicsErrors::InvalidState));
            return Result<void>::Success();
        }

        /** @brief Checks sequence and exact fixed delta before a tick begins. */
        [[nodiscard]] Result<void> ValidateTickInput(const auto &impl, const PhysicsFixedTickInput &input) {
            if (const auto configuredNanoseconds =
                    static_cast<std::int64_t>(std::llround(impl.settings.Values().world.fixedDeltaSeconds * 1'000'000'000.0));
                input.simulationTick == 0 || input.sceneGeneration == 0 || input.simulationTick != impl.published.completedTick + 1 ||
                input.fixedDelta.ToNanoseconds() != configuredNanoseconds)
                return Result<void>::Failure(MakeError(PhysicsErrors::DescriptorInvalid,
                                                       "Physics requires the next one-based tick and the world's exact fixed delta."));
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
        if (capability >= PhysicsCapability::Count)
            return PhysicsCapabilitySupport::Unknown;
        const auto canonicalWorld = static_cast<std::uint8_t>(impl_->mode == PhysicsRuntimeMode::Canonical) *
                                    static_cast<std::uint8_t>(capability == PhysicsCapability::WorldCreation);
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
            return Result<PhysicsCommandAdmission>::Success(RejectFullCommand(*impl_, destruction));

        const auto tail = (impl_->commandHead + impl_->commandCount) % capacity;
        impl_->commands[tail] = command;
        ++impl_->commandCount;
        impl_->commandOrderDirty = true;
        ++impl_->statistics.admittedCommands;
        impl_->statistics.pendingCommands = impl_->commandCount;
        impl_->statistics.maximumCommandDepth = std::max(impl_->statistics.maximumCommandDepth, impl_->commandCount);
        return Result<PhysicsCommandAdmission>::Success({PhysicsCommandAdmissionStatus::Deferred, impl_->commandCount});
    }

    /** @copydoc PhysicsWorld::AdvanceFixedTick */
    Result<void> PhysicsWorld::AdvanceFixedTick(const PhysicsFixedTickInput &input) {
        using enum PhysicsTickPhase;
        if (const Result<void> ready = CheckReadyForTick(*impl_); ready.HasError())
            return ready;
        if (const Result<void> validInput = ValidateTickInput(*impl_, input); validInput.HasError())
            return validInput;
        if (const Result<void> jobs = ValidateSolverJobs(impl_->runtime->solverJobs, input.solverJobs); jobs.HasError())
            return jobs;

        impl_->stepping = true;
        const BooleanResetGuard stepGuard{impl_->stepping};
        impl_->activeTick = input.simulationTick;

        CanonicalizeCommands(*impl_);
        const Result<std::uint32_t> frame = ValidateCommandFrame(*impl_, input);
        if (frame.HasError())
            return Result<void>::Failure(frame.ErrorValue());
        const std::uint32_t eligible = frame.Value();
        std::uint32_t applied{};
        ObservePhase(input, ApplyDeferredPreStep);
        ObserveCommands(*impl_, input, eligible, PhysicsStructuralCommandKind::Create, PhysicsCommandSafePoint::PreStep, applied);
        ObservePhase(input, CopyKinematicTargets);
        ObservePhase(input, ApplyDynamicInputs);
        ObservePhase(input, BroadPhase);
        ObservePhase(input, ContactGeneration);
        ObservePhase(input, ConstraintSolve);

        if (input.solverJobs.jobCount != 0) {
            const Result<void> jobs = RunSolverJobs(*impl_->runtime->solverJobs, input.solverJobs);
            if (jobs.HasError()) {
                impl_->Fail(jobs.ErrorValue());
                return Result<void>::Failure(jobs.ErrorValue());
            }
        }

        if (const auto stepped =
                Detail::StepCanonicalWorld(impl_->native, static_cast<float>(impl_->settings.Values().world.fixedDeltaSeconds));
            stepped.HasError()) {
            impl_->Fail(stepped.ErrorValue());
            return Result<void>::Failure(stepped.ErrorValue());
        }

        ObservePhase(input, IntegrateBodies);
        ObservePhase(input, WriteRuntimeTransforms);
        ObservePhase(input, ProduceEvents);
        ObservePhase(input, ApplyDeferredPostStep);
        ObserveCommands(*impl_, input, eligible, PhysicsStructuralCommandKind::Destroy, PhysicsCommandSafePoint::PostStep, applied);

        impl_->DiscardCommands(eligible);
        CommitPublishedTick(*impl_, input.simulationTick, applied);
        impl_->statistics.completedTicks = input.simulationTick;
        ObservePhase(input, PublishCompletedTick);
        return Result<void>::Success();
    }

    /** @copydoc PhysicsWorld::PublishedTick */
    PhysicsPublishedTick PhysicsWorld::PublishedTick() const noexcept {
        PublicationGuard publicationGuard{impl_->publicationLock};
        return impl_->published;
    }

    /** @copydoc PhysicsWorld::TickStatistics */
    PhysicsTickStatistics PhysicsWorld::TickStatistics() const noexcept {
        return impl_->statistics;
    }
}  // namespace Horo::Physics
