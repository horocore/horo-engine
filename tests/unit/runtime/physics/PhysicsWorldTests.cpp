#include "Horo/Foundation/JobSystem.h"
#include "Horo/Physics/PhysicsWorld.h"
#include "Horo/Runtime/RuntimeHost.h"
#include "PhysicsTestUtils.h"

#include <array>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <thread>
#include <vector>

namespace Horo::Physics {
    namespace {
        [[nodiscard]] PhysicsStructuralCommand MakeCommand(const std::uint64_t tick, const std::uint64_t world, const std::uint64_t scene,
                                                           const std::uint64_t sourceSequence, const std::uint64_t subject,
                                                           const PhysicsStructuralCommandKind kind,
                                                           const PhysicsCommandTargetKind target = PhysicsCommandTargetKind::Body,
                                                           const std::uint64_t source = 1) {
            return {.order = {.simulationTick = tick,
                              .worldGeneration = world,
                              .sceneGeneration = scene,
                              .targetKind = target,
                              .targetIdentity = subject,
                              .commandKind = kind,
                              .source = PhysicsCommandSourceId::Create(source).Value(),
                              .sourceSequence = sourceSequence}};
        }

        [[nodiscard]] PhysicsWorldSettings BoundedCommandWorldSettings(const std::uint32_t maximumCommands) {
            PhysicsWorldSettingsDescriptor descriptor;
            descriptor.world.capacity = {16, 32, 16, 4096};
            descriptor.budgets.maximumContactPairs = 32;
            descriptor.budgets.maximumContactConstraints = 16;
            descriptor.budgets.maximumInFlightPairs = 8;
            descriptor.budgets.maximumCommands = maximumCommands;
            descriptor.budgets.maximumCommandsPerTick = maximumCommands;
            descriptor.budgets.scratchBytes = 1024 * 1024;
            return PhysicsWorldSettings::Capture(descriptor).Value();
        }

    }  // namespace

#if HORO_TEST_PHYSICS_NATIVE
    namespace {
        struct TickTrace final {
            PhysicsWorld *world{};
            std::vector<PhysicsTickPhase> phases;
            std::vector<std::uint64_t> commandSequences;
            PhysicsCommandAdmissionStatus reentrantAdmission{PhysicsCommandAdmissionStatus::RejectedFull};
            bool reentrantStepRejected{};
            bool queuedDuringTick{};
            bool attemptedReentrantStep{};
        };

        void RecordPhase(void *context, const PhysicsTickPhase phase, const std::uint64_t tick) noexcept {
            auto &trace = *static_cast<TickTrace *>(context);
            trace.phases.push_back(phase);
            if (phase != PhysicsTickPhase::CopyKinematicTargets || trace.queuedDuringTick)
                return;
            trace.queuedDuringTick = true;
            const auto admitted =
                trace.world->QueueStructuralCommand(MakeCommand(tick + 1, 200, 7, 1, 13, PhysicsStructuralCommandKind::Change));
            if (admitted.HasValue())
                trace.reentrantAdmission = admitted.Value().status;
            trace.attemptedReentrantStep = true;
            const auto stepped = trace.world->AdvanceFixedTick(
                {.simulationTick = tick, .sceneGeneration = 7, .fixedDelta = Duration::FromNanoseconds(16'666'667)});
            trace.reentrantStepRejected =
                stepped.HasError() && stepped.ErrorValue().code.Value() == PhysicsErrors::InvalidState.code.Value();
        }

        void RecordCommand(void *context, const PhysicsStructuralCommand &command, const PhysicsCommandSafePoint,
                           const std::uint64_t) noexcept {
            static_cast<TickTrace *>(context)->commandSequences.push_back(command.order.sourceSequence);
        }

        [[nodiscard]] std::unique_ptr<PhysicsWorld> ActiveCanonicalWorld(const PhysicsWorldSettings &settings,
                                                                         std::unique_ptr<PhysicsRuntime> &runtime) {
            runtime = PhysicsRuntime::Create(PhysicsRuntimeMode::Canonical).Value();
            auto world = runtime->PrepareWorld(settings).Value();
            REQUIRE(world->Activate(PhysicsWorldId::Create(200).Value()).HasValue());
            return world;
        }

        void RequirePublishedTick(const PhysicsPublishedTick &published, const std::uint64_t expectedTick,
                                  const std::uint64_t expectedRevision, const std::uint32_t expectedCommands) {
            REQUIRE(published.completedTick == expectedTick);
            REQUIRE(published.publicationRevision == expectedRevision);
            REQUIRE(published.transformTick == published.completedTick);
            REQUIRE(published.queryTick == published.completedTick);
            REQUIRE(published.eventTick == published.completedTick);
            REQUIRE(published.appliedCommands == expectedCommands);
            REQUIRE(published.eventCount == 0);
            REQUIRE(published.droppedEventCount == 0);
        }

        struct SolverJobTrace final {
            std::atomic_uint32_t completed{};
            std::atomic_bool fail{};
        };

        Result<void> RunSolverJob(void *context, const CancellationToken &) noexcept {
            auto &trace = *static_cast<SolverJobTrace *>(context);
            trace.completed.fetch_add(1, std::memory_order_release);
            if (trace.fail.load(std::memory_order_acquire))
                return Result<void>::Failure(MakeError(PhysicsErrors::InitializationFailed, "Injected solver worker failure."));
            return Result<void>::Success();
        }

        Result<void> RunUntilCancelled(void *context, const CancellationToken &cancellation) noexcept {
            auto &cancelled = *static_cast<std::atomic_bool *>(context);
            while (!cancellation.IsCancellationRequested())
                std::this_thread::yield();
            cancelled.store(true, std::memory_order_release);
            return Result<void>::Success();
        }

        class PhysicsTickParticipant final : public Runtime::RuntimeLifecycleParticipant {
        public:
            explicit PhysicsTickParticipant(std::vector<std::uint64_t> &trace) : trace_(&trace) {}

            Result<void> Startup(const CancellationToken &) override {
                auto createdRuntime = PhysicsRuntime::Create(PhysicsRuntimeMode::Canonical);
                if (createdRuntime.HasError())
                    return Result<void>::Failure(createdRuntime.ErrorValue());
                runtime_ = std::move(createdRuntime).Value();
                auto prepared = runtime_->PrepareWorld(Test::SmallWorldSettings());
                if (prepared.HasError())
                    return Result<void>::Failure(prepared.ErrorValue());
                world_ = std::move(prepared).Value();
                return world_->Activate(PhysicsWorldId::Create(201).Value());
            }

            Result<void> OnPhase(Runtime::RuntimePhase, const Runtime::FrameContext &) override {
                return Result<void>::Success();
            }

            Result<void> OnFixedUpdate(const Runtime::FixedStepContext &context) override {
                const auto created = world_->QueueStructuralCommand(
                    MakeCommand(context.simulationTick, 201, 9, 1, context.simulationTick * 2 - 1, PhysicsStructuralCommandKind::Create));
                if (created.HasError())
                    return Result<void>::Failure(created.ErrorValue());
                const auto destroyed = world_->QueueStructuralCommand(
                    MakeCommand(context.simulationTick, 201, 9, 2, context.simulationTick * 2, PhysicsStructuralCommandKind::Destroy));
                if (destroyed.HasError())
                    return Result<void>::Failure(destroyed.ErrorValue());
                const PhysicsTickObserver observer{.context = trace_, .command = RecordCommandSequence};
                const auto stepped = world_->AdvanceFixedTick({.simulationTick = context.simulationTick,
                                                               .sceneGeneration = 9,
                                                               .fixedDelta = context.fixedDelta,
                                                               .observer = observer});
                if (stepped.HasValue())
                    lastPublishedTick = world_->PublishedTick().completedTick;
                return stepped;
            }

            void Shutdown() noexcept override {
                world_.reset();
                runtime_.reset();
            }

            static void RecordCommandSequence(void *context, const PhysicsStructuralCommand &command, PhysicsCommandSafePoint,
                                              std::uint64_t) noexcept {
                static_cast<std::vector<std::uint64_t> *>(context)->push_back(command.order.targetIdentity);
            }

            std::uint64_t lastPublishedTick{};

        private:
            std::vector<std::uint64_t> *trace_{};
            std::unique_ptr<PhysicsRuntime> runtime_;
            std::unique_ptr<PhysicsWorld> world_;
        };

        [[nodiscard]] std::vector<std::uint64_t> RunPhysicsCadence(const std::uint32_t frameCount) {
            DeterministicClock clock;
            std::vector<std::uint64_t> trace;
            trace.reserve(120);
            auto host = Runtime::RuntimeHost::Create(clock).Value();
            REQUIRE(host->AddParticipant(std::make_unique<PhysicsTickParticipant>(trace)).HasValue());
            REQUIRE(host->Startup().HasValue());
            REQUIRE(host->RunFrame().HasValue());
            constexpr std::int64_t totalNanoseconds = 60LL * 16'666'667;
            const std::int64_t baseDelta = totalNanoseconds / frameCount;
            for (std::uint32_t frame = 0; frame < frameCount; ++frame) {
                const std::int64_t remainder = frame + 1 == frameCount ? totalNanoseconds % frameCount : 0;
                clock.Advance(Duration::FromNanoseconds(baseDelta + remainder));
                REQUIRE(host->RunFrame().HasValue());
            }
            REQUIRE(host->Statistics().completedSimulationTick == 60);
            return trace;
        }
    }  // namespace

    TEST_CASE("Physics fixed tick defers reentrant commands and publishes one complete revision", "[physics][tick]") {
        std::unique_ptr<PhysicsRuntime> runtime;
        auto world = ActiveCanonicalWorld(Test::SmallWorldSettings(), runtime);
        REQUIRE(world->QueueStructuralCommand(MakeCommand(1, 200, 7, 1, 11, PhysicsStructuralCommandKind::Create)).Value().status ==
                PhysicsCommandAdmissionStatus::Deferred);
        REQUIRE(world->QueueStructuralCommand(MakeCommand(1, 200, 7, 2, 12, PhysicsStructuralCommandKind::Destroy)).Value().status ==
                PhysicsCommandAdmissionStatus::Deferred);

        TickTrace trace{.world = world.get()};
        trace.phases.reserve(11);
        trace.commandSequences.reserve(2);
        const PhysicsTickObserver observer{.context = &trace, .phase = RecordPhase, .command = RecordCommand};
        constexpr Duration fixedDelta = Duration::FromNanoseconds(16'666'667);
        REQUIRE(world->AdvanceFixedTick({.simulationTick = 1, .sceneGeneration = 7, .fixedDelta = fixedDelta, .observer = observer})
                    .HasValue());
        constexpr std::array expectedPhases{
            PhysicsTickPhase::ApplyDeferredPreStep, PhysicsTickPhase::CopyKinematicTargets,
            PhysicsTickPhase::ApplyDynamicInputs,   PhysicsTickPhase::BroadPhase,
            PhysicsTickPhase::ContactGeneration,    PhysicsTickPhase::ConstraintSolve,
            PhysicsTickPhase::IntegrateBodies,      PhysicsTickPhase::WriteRuntimeTransforms,
            PhysicsTickPhase::ProduceEvents,        PhysicsTickPhase::ApplyDeferredPostStep,
            PhysicsTickPhase::PublishCompletedTick,
        };
        REQUIRE(trace.phases.size() == expectedPhases.size());
        for (std::size_t index = 0; index < expectedPhases.size(); ++index)
            REQUIRE(trace.phases[index] == expectedPhases[index]);
        REQUIRE(trace.commandSequences == std::vector<std::uint64_t>{1, 2});
        REQUIRE(trace.reentrantAdmission == PhysicsCommandAdmissionStatus::Deferred);
        REQUIRE(trace.attemptedReentrantStep);
        REQUIRE(trace.reentrantStepRejected);

        RequirePublishedTick(world->PublishedTick(), 1, 1, 2);
        REQUIRE(world->TickStatistics().pendingCommands == 1);

        trace.phases.clear();
        trace.commandSequences.clear();
        REQUIRE(world->AdvanceFixedTick({.simulationTick = 2, .sceneGeneration = 7, .fixedDelta = fixedDelta, .observer = observer})
                    .HasValue());
        REQUIRE(trace.commandSequences == std::vector<std::uint64_t>{1});
        REQUIRE(world->PublishedTick().appliedCommands == 1);
        REQUIRE(world->TickStatistics().pendingCommands == 0);
        REQUIRE(world->AdvanceFixedTick({.simulationTick = 2, .sceneGeneration = 7, .fixedDelta = fixedDelta}).HasError());
        REQUIRE(world->PublishedTick().completedTick == 2);
    }

    TEST_CASE("Physics canonicalizes command frames independently of producer insertion order", "[physics][tick][determinism][headless]") {
        std::unique_ptr<PhysicsRuntime> runtime;
        auto world = ActiveCanonicalWorld(Test::SmallWorldSettings(), runtime);
        TickTrace trace{.world = world.get()};
        const PhysicsTickObserver observer{.context = &trace, .command = RecordCommand};
        constexpr Duration fixedDelta = Duration::FromNanoseconds(16'666'667);

        const std::array firstFrame{
            MakeCommand(1, 200, 17, 2, 30, PhysicsStructuralCommandKind::Change, PhysicsCommandTargetKind::Constraint),
            MakeCommand(1, 200, 17, 1, 20, PhysicsStructuralCommandKind::Change, PhysicsCommandTargetKind::Body),
            MakeCommand(1, 200, 17, 1, 10, PhysicsStructuralCommandKind::Change, PhysicsCommandTargetKind::World, 2),
        };
        for (const std::size_t index : {2U, 0U, 1U})
            REQUIRE(world->QueueStructuralCommand(firstFrame[index]).Value().status == PhysicsCommandAdmissionStatus::Deferred);
        REQUIRE(world->AdvanceFixedTick({.simulationTick = 1, .sceneGeneration = 17, .fixedDelta = fixedDelta, .observer = observer})
                    .HasValue());
        const std::vector<std::uint64_t> canonical = trace.commandSequences;

        trace.commandSequences.clear();
        const std::array secondFrame{
            MakeCommand(2, 200, 17, 2, 30, PhysicsStructuralCommandKind::Change, PhysicsCommandTargetKind::Constraint),
            MakeCommand(2, 200, 17, 1, 20, PhysicsStructuralCommandKind::Change, PhysicsCommandTargetKind::Body),
            MakeCommand(2, 200, 17, 1, 10, PhysicsStructuralCommandKind::Change, PhysicsCommandTargetKind::World, 2),
        };
        for (const std::size_t index : {1U, 2U, 0U})
            REQUIRE(world->QueueStructuralCommand(secondFrame[index]).Value().status == PhysicsCommandAdmissionStatus::Deferred);
        REQUIRE(world->AdvanceFixedTick({.simulationTick = 2, .sceneGeneration = 17, .fixedDelta = fixedDelta, .observer = observer})
                    .HasValue());
        REQUIRE(trace.commandSequences == canonical);
        REQUIRE(canonical == std::vector<std::uint64_t>{1, 1, 2});
    }

    TEST_CASE("Physics command frame validation fails closed and retains prior publication", "[physics][tick][determinism]") {
        std::unique_ptr<PhysicsRuntime> runtime;
        auto world = ActiveCanonicalWorld(Test::SmallWorldSettings(), runtime);
        constexpr Duration fixedDelta = Duration::FromNanoseconds(16'666'667);

        const auto skipped = world->AdvanceFixedTick({.simulationTick = 2, .sceneGeneration = 8, .fixedDelta = fixedDelta});
        REQUIRE(skipped.HasError());
        REQUIRE(skipped.ErrorValue().code.Value() == PhysicsErrors::DescriptorInvalid.code.Value());
        REQUIRE(world->PublishedTick().publicationRevision == 0);

        REQUIRE(
            world->QueueStructuralCommand(MakeCommand(1, 201, 8, 1, 1, PhysicsStructuralCommandKind::Create)).ErrorValue().code.Value() ==
            PhysicsErrors::CommandOrderInvalid.code.Value());
        REQUIRE(world->QueueStructuralCommand(MakeCommand(1, 200, 8, 2, 2, PhysicsStructuralCommandKind::Create)).HasValue());
        const auto missing = world->AdvanceFixedTick({.simulationTick = 1, .sceneGeneration = 8, .fixedDelta = fixedDelta});
        REQUIRE(missing.HasError());
        REQUIRE(missing.ErrorValue().code.Value() == PhysicsErrors::CommandOrderInvalid.code.Value());
        REQUIRE(world->PublishedTick().publicationRevision == 0);
        REQUIRE(world->TickStatistics().pendingCommands == 1);

        REQUIRE(world->QueueStructuralCommand(MakeCommand(1, 200, 8, 1, 1, PhysicsStructuralCommandKind::Create)).HasValue());
        const auto staleScene = world->AdvanceFixedTick({.simulationTick = 1, .sceneGeneration = 9, .fixedDelta = fixedDelta});
        REQUIRE(staleScene.HasError());
        REQUIRE(staleScene.ErrorValue().code.Value() == PhysicsErrors::CommandOrderInvalid.code.Value());
        REQUIRE(world->PublishedTick().publicationRevision == 0);
        REQUIRE(world->TickStatistics().pendingCommands == 2);
        REQUIRE(world->AdvanceFixedTick({.simulationTick = 1, .sceneGeneration = 8, .fixedDelta = fixedDelta}).HasValue());
        REQUIRE(world->PublishedTick().completedTick == 1);
        REQUIRE(world->PublishedTick().appliedCommands == 2);
        REQUIRE(world->TickStatistics().pendingCommands == 0);

        REQUIRE(
            world->QueueStructuralCommand(MakeCommand(1, 200, 8, 1, 3, PhysicsStructuralCommandKind::Change)).ErrorValue().code.Value() ==
            PhysicsErrors::CommandOrderInvalid.code.Value());
        REQUIRE(world->QueueStructuralCommand(MakeCommand(2, 200, 8, 1, 3, PhysicsStructuralCommandKind::Change)).HasValue());
        REQUIRE(world->QueueStructuralCommand(MakeCommand(2, 200, 8, 1, 4, PhysicsStructuralCommandKind::Change)).HasValue());
        const auto duplicate = world->AdvanceFixedTick({.simulationTick = 2, .sceneGeneration = 8, .fixedDelta = fixedDelta});
        REQUIRE(duplicate.HasError());
        REQUIRE(duplicate.ErrorValue().code.Value() == PhysicsErrors::CommandOrderInvalid.code.Value());
        REQUIRE(world->PublishedTick().completedTick == 1);
        REQUIRE(world->TickStatistics().pendingCommands == 2);
        world->Shutdown();
        REQUIRE(
            world->QueueStructuralCommand(MakeCommand(2, 200, 8, 2, 4, PhysicsStructuralCommandKind::Change)).ErrorValue().code.Value() ==
            PhysicsErrors::InvalidState.code.Value());
    }

    TEST_CASE("Physics retains interleaved future frames within one bounded command queue", "[physics][commands][determinism]") {
        std::unique_ptr<PhysicsRuntime> runtime;
        auto world = ActiveCanonicalWorld(BoundedCommandWorldSettings(4), runtime);
        constexpr Duration fixedDelta = Duration::FromNanoseconds(16'666'667);

        REQUIRE(world->QueueStructuralCommand(MakeCommand(2, 200, 8, 1, 20, PhysicsStructuralCommandKind::Create)).HasValue());
        REQUIRE(world->QueueStructuralCommand(MakeCommand(1, 200, 8, 1, 10, PhysicsStructuralCommandKind::Create)).HasValue());
        REQUIRE(world->QueueStructuralCommand(MakeCommand(2, 200, 8, 2, 21, PhysicsStructuralCommandKind::Change)).HasValue());
        REQUIRE(world->QueueStructuralCommand(MakeCommand(1, 200, 8, 2, 11, PhysicsStructuralCommandKind::Change)).Value().status ==
                PhysicsCommandAdmissionStatus::RejectedFull);
        REQUIRE(world->QueueStructuralCommand(MakeCommand(1, 200, 8, 2, 11, PhysicsStructuralCommandKind::Destroy)).Value().status ==
                PhysicsCommandAdmissionStatus::Deferred);

        REQUIRE(world->AdvanceFixedTick({.simulationTick = 1, .sceneGeneration = 8, .fixedDelta = fixedDelta}).HasValue());
        REQUIRE(world->PublishedTick().appliedCommands == 2);
        REQUIRE(world->TickStatistics().pendingCommands == 2);
        REQUIRE(world->QueueStructuralCommand(MakeCommand(3, 200, 8, 1, 30, PhysicsStructuralCommandKind::Create)).Value().status ==
                PhysicsCommandAdmissionStatus::Deferred);
        REQUIRE(world->AdvanceFixedTick({.simulationTick = 2, .sceneGeneration = 8, .fixedDelta = fixedDelta}).HasValue());
        REQUIRE(world->PublishedTick().appliedCommands == 2);
        REQUIRE(world->TickStatistics().pendingCommands == 1);
        REQUIRE(world->AdvanceFixedTick({.simulationTick = 3, .sceneGeneration = 8, .fixedDelta = fixedDelta}).HasValue());
        REQUIRE(world->PublishedTick().appliedCommands == 1);
        REQUIRE(world->TickStatistics().pendingCommands == 0);
    }

    TEST_CASE("Physics rejects malformed or unavailable solver job batches before starting a tick", "[physics][tick][jobs]") {
        constexpr Duration fixedDelta = Duration::FromNanoseconds(16'666'667);
        {
            std::unique_ptr<PhysicsRuntime> runtime;
            auto world = ActiveCanonicalWorld(Test::SmallWorldSettings(), runtime);
            SolverJobTrace trace;
            const PhysicsSolverJob solverJob{.context = &trace, .execute = RunSolverJob};
            const auto unavailable = world->AdvanceFixedTick(
                {.simulationTick = 1,
                 .sceneGeneration = 1,
                 .fixedDelta = fixedDelta,
                 .solverJobs = {.jobs = &solverJob, .jobCount = 1, .joinTimeout = Duration::FromMilliseconds(100)}});
            REQUIRE(unavailable.HasError());
            REQUIRE(unavailable.ErrorValue().code.Value() == PhysicsErrors::CapabilityUnavailable.code.Value());
            REQUIRE(world->State() == PhysicsWorldState::ActiveSolver);
        }

        JobSystem jobs({.workerCount = 1, .maxQueuedJobs = 1});
        auto runtime = PhysicsRuntime::Create(PhysicsRuntimeMode::Canonical, &jobs).Value();
        auto world = runtime->PrepareWorld(Test::SmallWorldSettings()).Value();
        REQUIRE(world->Activate(PhysicsWorldId::Create(204).Value()).HasValue());
        const PhysicsSolverJob missingCallback{};
        const auto oversized = world->AdvanceFixedTick({.simulationTick = 1,
                                                        .sceneGeneration = 1,
                                                        .fixedDelta = fixedDelta,
                                                        .solverJobs = {.jobs = &missingCallback,
                                                                       .jobCount = MaximumPhysicsSolverJobsPerTick + 1,
                                                                       .joinTimeout = Duration::FromMilliseconds(100)}});
        REQUIRE(oversized.HasError());
        REQUIRE(oversized.ErrorValue().code.Value() == PhysicsErrors::DescriptorInvalid.code.Value());
        const auto nullCallback = world->AdvanceFixedTick(
            {.simulationTick = 1,
             .sceneGeneration = 1,
             .fixedDelta = fixedDelta,
             .solverJobs = {.jobs = &missingCallback, .jobCount = 1, .joinTimeout = Duration::FromMilliseconds(100)}});
        REQUIRE(nullCallback.HasError());
        REQUIRE(nullCallback.ErrorValue().code.Value() == PhysicsErrors::DescriptorInvalid.code.Value());
        REQUIRE(world->State() == PhysicsWorldState::ActiveSolver);
        REQUIRE(world->PublishedTick().publicationRevision == 0);
        world.reset();
        runtime.reset();
        jobs.Shutdown(ShutdownPolicy::Cancel);
    }

    TEST_CASE("Physics rejects foreign-thread world mutation with a typed affinity error", "[physics][tick][threading]") {
        std::unique_ptr<PhysicsRuntime> runtime;
        auto world = ActiveCanonicalWorld(Test::SmallWorldSettings(), runtime);
        std::atomic_bool queueRejected{};
        std::atomic_bool tickRejected{};
        std::thread foreign([&] {
            const auto queued = world->QueueStructuralCommand(MakeCommand(1, 200, 1, 1, 1, PhysicsStructuralCommandKind::Create));
            queueRejected.store(queued.HasError() &&
                                    queued.ErrorValue().code.Value() == PhysicsErrors::ThreadAffinityViolation.code.Value(),
                                std::memory_order_release);
            const auto stepped =
                world->AdvanceFixedTick({.simulationTick = 1, .sceneGeneration = 1, .fixedDelta = Duration::FromNanoseconds(16'666'667)});
            tickRejected.store(stepped.HasError() &&
                                   stepped.ErrorValue().code.Value() == PhysicsErrors::ThreadAffinityViolation.code.Value(),
                               std::memory_order_release);
        });
        foreign.join();
        REQUIRE(queueRejected.load(std::memory_order_acquire));
        REQUIRE(tickRejected.load(std::memory_order_acquire));
        REQUIRE(world->State() == PhysicsWorldState::ActiveSolver);
        REQUIRE(world->PublishedTick().completedTick == 0);
    }

    TEST_CASE("Physics cancels and drains solver work before a failed world can be destroyed", "[physics][tick][jobs]") {
        JobSystem jobs({.workerCount = 1, .maxQueuedJobs = 1});
        auto runtime = PhysicsRuntime::Create(PhysicsRuntimeMode::Canonical, &jobs).Value();
        auto world = runtime->PrepareWorld(Test::SmallWorldSettings()).Value();
        REQUIRE(world->Activate(PhysicsWorldId::Create(203).Value()).HasValue());

        std::atomic_bool cancellationObserved{};
        const PhysicsSolverJob solverJob{.context = &cancellationObserved, .execute = RunUntilCancelled};
        const PhysicsSolverJobBatch batch{.jobs = &solverJob, .jobCount = 1, .joinTimeout = Duration::FromMilliseconds(500)};
        const auto stepped = world->AdvanceFixedTick(
            {.simulationTick = 1, .sceneGeneration = 1, .fixedDelta = Duration::FromNanoseconds(16'666'667), .solverJobs = batch});
        REQUIRE(stepped.HasError());
        REQUIRE(stepped.ErrorValue().code.Value() == PhysicsErrors::SolverDeadlineExceeded.code.Value());
        REQUIRE(cancellationObserved.load(std::memory_order_acquire));
        REQUIRE(world->State() == PhysicsWorldState::Failed);
        REQUIRE(world->PublishedTick().publicationRevision == 0);
        world.reset();
        runtime.reset();
        jobs.Shutdown(ShutdownPolicy::Cancel);
    }

    TEST_CASE("Physics command capacity reserves destruction and returns explicit retry ownership", "[physics][commands]") {
        std::unique_ptr<PhysicsRuntime> runtime;
        auto world = ActiveCanonicalWorld(BoundedCommandWorldSettings(3), runtime);

        REQUIRE(world->QueueStructuralCommand({}).ErrorValue().code.Value() == PhysicsErrors::CommandOrderInvalid.code.Value());
        REQUIRE(world->QueueStructuralCommand(MakeCommand(1, 200, 8, 1, 21, PhysicsStructuralCommandKind::Create)).Value().status ==
                PhysicsCommandAdmissionStatus::Deferred);
        REQUIRE(world->QueueStructuralCommand(MakeCommand(1, 200, 8, 2, 22, PhysicsStructuralCommandKind::Change)).Value().status ==
                PhysicsCommandAdmissionStatus::Deferred);
        REQUIRE(world->QueueStructuralCommand(MakeCommand(1, 200, 8, 3, 23, PhysicsStructuralCommandKind::Create)).Value().status ==
                PhysicsCommandAdmissionStatus::RejectedFull);
        REQUIRE(world->QueueStructuralCommand(MakeCommand(1, 200, 8, 3, 23, PhysicsStructuralCommandKind::Destroy)).Value().status ==
                PhysicsCommandAdmissionStatus::Deferred);
        REQUIRE(world->QueueStructuralCommand(MakeCommand(2, 200, 8, 1, 24, PhysicsStructuralCommandKind::Destroy)).Value().status ==
                PhysicsCommandAdmissionStatus::DestructionRetryRequired);
        const PhysicsTickStatistics statistics = world->TickStatistics();
        REQUIRE(statistics.pendingCommands == 3);
        REQUIRE(statistics.maximumCommandDepth == 3);
        REQUIRE(statistics.rejectedCommands == 2);
        REQUIRE(statistics.destructionRetryCount == 1);

        constexpr Duration fixedDelta = Duration::FromNanoseconds(16'666'667);
        REQUIRE(world->AdvanceFixedTick({.simulationTick = 1, .sceneGeneration = 8, .fixedDelta = fixedDelta}).HasValue());
        REQUIRE(world->QueueStructuralCommand(MakeCommand(2, 200, 8, 1, 24, PhysicsStructuralCommandKind::Destroy)).Value().status ==
                PhysicsCommandAdmissionStatus::Deferred);
        REQUIRE(world->AdvanceFixedTick({.simulationTick = 2, .sceneGeneration = 8, .fixedDelta = fixedDelta}).HasValue());
        REQUIRE(world->PublishedTick().appliedCommands == 1);
        REQUIRE(world->TickStatistics().pendingCommands == 0);
    }

    TEST_CASE("Physics publication snapshots remain coherent for concurrent readers", "[physics][tick][threading]") {
        std::unique_ptr<PhysicsRuntime> runtime;
        auto world = ActiveCanonicalWorld(Test::SmallWorldSettings(), runtime);
        std::atomic_bool reading{true};
        std::atomic_bool coherent{true};
        std::thread reader([&] {
            while (reading.load(std::memory_order_acquire)) {
                const PhysicsPublishedTick published = world->PublishedTick();
                if (published.publicationRevision == 0)
                    continue;
                const bool sameTick = published.completedTick == published.transformTick &&
                                      published.completedTick == published.queryTick && published.completedTick == published.eventTick;
                if (!sameTick || published.publicationRevision != published.completedTick)
                    coherent.store(false, std::memory_order_release);
            }
        });

        constexpr Duration fixedDelta = Duration::FromNanoseconds(16'666'667);
        for (std::uint64_t tick = 1; tick <= 100; ++tick)
            REQUIRE(world->AdvanceFixedTick({.simulationTick = tick, .sceneGeneration = 1, .fixedDelta = fixedDelta}).HasValue());
        reading.store(false, std::memory_order_release);
        reader.join();

        REQUIRE(coherent.load(std::memory_order_acquire));
        RequirePublishedTick(world->PublishedTick(), 100, 100, 0);
    }

    TEST_CASE("Runtime cadence cannot change Physics tick phase or command order", "[physics][tick][runtime]") {
        const std::vector<std::uint64_t> thirtyHz = RunPhysicsCadence(30);
        const std::vector<std::uint64_t> sixtyHz = RunPhysicsCadence(60);
        const std::vector<std::uint64_t> highHz = RunPhysicsCadence(144);
        REQUIRE(thirtyHz.size() == 120);
        REQUIRE(sixtyHz == thirtyHz);
        REQUIRE(highHz == thirtyHz);
    }
#endif
}  // namespace Horo::Physics
