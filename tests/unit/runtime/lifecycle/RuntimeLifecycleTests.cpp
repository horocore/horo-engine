#include "Horo/Runtime/Render/NullBackendModule.h"
#include "Horo/Runtime/Render/RenderFrontend.h"
#include "Horo/Runtime/RuntimeHost.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <new>
#include <optional>
#include <stdexcept>
#include <vector>

namespace {
    std::atomic<std::size_t> gAllocationCount{};

    void *CountedAllocate(const std::size_t size) {
        gAllocationCount.fetch_add(1, std::memory_order_relaxed);
        if (void *memory = std::malloc(size))
            return memory;
        throw std::bad_alloc{};
    }
}  // namespace

void *operator new(const std::size_t size) {
    return CountedAllocate(size);
}

void *operator new[](const std::size_t size) {
    return CountedAllocate(size);
}

void operator delete(void *memory) noexcept {
    std::free(memory);
}

void operator delete[](void *memory) noexcept {
    std::free(memory);
}

void operator delete(void *memory, std::size_t) noexcept {
    std::free(memory);
}

void operator delete[](void *memory, std::size_t) noexcept {
    std::free(memory);
}

namespace {
    using namespace Horo;
    using namespace Horo::Render;
    using namespace Horo::Runtime;

    void Check(const bool condition) {
        REQUIRE((condition));
    }

    [[nodiscard]] Error TestError(const char *code = "runtime.test.failure") {
        return {ErrorCode{code}, ErrorDomainId{"runtime.test"}, ErrorSeverity::Error, "Injected failure.", {}};
    }

    class RecordingParticipant : public RuntimeLifecycleParticipant {
    public:
        explicit RecordingParticipant(std::vector<RuntimePhase> *phases = nullptr) : phases_(phases) {}

        Result<void> Startup(const CancellationToken &) override {
            ++startupCount;
            if (failStartup)
                return Result<void>::Failure(TestError("runtime.test.startup"));
            return Result<void>::Success();
        }

        Result<void> OnPhase(const RuntimePhase phase, const FrameContext &context) override {
            if (phases_)
                phases_->push_back(phase);
            lastVariableDelta = context.variableDelta;
            lastInterpolationAlpha = context.interpolationAlpha;
            lastCompletedSimulationTick = context.completedSimulationTick;
            lastDroppedSimulationTime = context.droppedSimulationTime;
            lastRealDeltaWasClamped = context.realDeltaWasClamped;
            if (throwPhase.has_value() && *throwPhase == phase)
                throw std::runtime_error{"Injected exception."};
            if (failPhase.has_value() && *failPhase == phase)
                return Result<void>::Failure(TestError());
            if (cancelPhase.has_value() && *cancelPhase == phase)
                host->RequestShutdown();
            return Result<void>::Success();
        }

        Result<void> OnFixedUpdate(const FixedStepContext &context) override {
            fixedTicks.push_back(context.simulationTick);
            if (failFixedTick == context.simulationTick)
                return Result<void>::Failure(TestError("runtime.test.fixed"));
            return Result<void>::Success();
        }

        void Shutdown() noexcept override {
            ++shutdownCount;
            if (shutdownOrder)
                shutdownOrder->push_back(id);
        }

        std::vector<RuntimePhase> *phases_{};
        std::vector<std::uint64_t> fixedTicks;
        std::vector<int> *shutdownOrder{};
        RuntimeHost *host{};
        std::optional<RuntimePhase> failPhase;
        std::optional<RuntimePhase> throwPhase;
        std::optional<RuntimePhase> cancelPhase;
        std::uint64_t failFixedTick{};
        Duration lastVariableDelta{};
        double lastInterpolationAlpha{};
        std::uint64_t lastCompletedSimulationTick{};
        Duration lastDroppedSimulationTime{};
        bool lastRealDeltaWasClamped{};
        int id{};
        int startupCount{};
        int shutdownCount{};
        bool failStartup{false};
    };

    class AllocationFreeParticipant final : public RuntimeLifecycleParticipant {
    public:
        Result<void> Startup(const CancellationToken &) override {
            return Result<void>::Success();
        }

        Result<void> OnPhase(RuntimePhase, const FrameContext &) override {
            ++phaseCount;
            return Result<void>::Success();
        }

        Result<void> OnFixedUpdate(const FixedStepContext &) override {
            ++fixedCount;
            return Result<void>::Success();
        }

        void Shutdown() noexcept override {}

        std::uint64_t phaseCount{};
        std::uint64_t fixedCount{};
    };

    class NullRenderParticipant final : public RuntimeLifecycleParticipant {
    public:
        Result<void> Startup(const CancellationToken &) override {
            if (Result<void> registered = RegisterNullRenderBackend(registry_); registered.HasError())
                return registered;
            if (Result<void> sealed = registry_.Seal(); sealed.HasError())
                return sealed;
            auto created = RenderFrontend::Create(registry_, RenderBackendId{"null"}, RenderBackendConfig{});
            if (created.HasError())
                return Result<void>::Failure(created.ErrorValue());
            frontend_ = std::move(created).Value();
            return Result<void>::Success();
        }

        Result<void> OnPhase(const RuntimePhase phase, const FrameContext &context) override {
            if (phase == RuntimePhase::RenderExecution) {
                auto begun = frontend_->BeginFrame(FrameDescriptor{.frameNumber = context.frameNumber, .outputExtent = {64, 64}});
                if (begun.HasError())
                    return Result<void>::Failure(begun.ErrorValue());
                frame_.emplace(std::move(begun).Value());
                const std::array passes{RenderPassDescriptor{.id = RenderPassId{1}, .kind = RenderPassKind::Graphics}};
                return frame_->Execute(passes);
            }
            if (phase == RuntimePhase::Presentation) {
                Result<void> result = frame_->Present();
                frame_.reset();
                ++presentCount;
                return result;
            }
            return Result<void>::Success();
        }

        Result<void> OnFixedUpdate(const FixedStepContext &) override {
            return Result<void>::Success();
        }

        void Shutdown() noexcept override {
            frame_.reset();
            frontend_.reset();
        }

        int presentCount{};

    private:
        RenderBackendRegistry registry_;
        std::unique_ptr<RenderFrontend> frontend_;
        std::optional<RenderFrameScope> frame_;
    };

    [[nodiscard]] std::unique_ptr<RuntimeHost> MakeHost(DeterministicClock &clock, const FrameSchedulerConfig config = {}) {
        auto created = RuntimeHost::Create(clock, config);
        Check(created.HasValue());
        return std::move(created).Value();
    }

    TEST_CASE("Phase Order And Tick Contexts Are Canonical", "[unit][runtime]") {
        DeterministicClock clock;
        std::vector<RuntimePhase> phases;
        auto participant = std::make_unique<RecordingParticipant>(&phases);
        RecordingParticipant *observed = participant.get();
        auto host = MakeHost(clock);
        Check(host->AddParticipant(std::move(participant)).HasValue());
        Check(host->Startup().HasValue());
        Check(host->RunFrame().HasValue());
        clock.Advance(Duration::FromNanoseconds(16'666'667));
        Check(host->RunFrame().HasValue());

        constexpr std::array expected{RuntimePhase::BeginFrame,
                                      RuntimePhase::PollPlatformEvents,
                                      RuntimePhase::BuildInputSnapshot,
                                      RuntimePhase::ApplyQueuedOwnerThreadCommands,
                                      RuntimePhase::NetworkPoll,
                                      RuntimePhase::NetworkFlush,
                                      RuntimePhase::VariableUpdate,
                                      RuntimePhase::RenderExtraction,
                                      RuntimePhase::RenderExecution,
                                      RuntimePhase::RenderGui,
                                      RuntimePhase::Presentation,
                                      RuntimePhase::CommitDeferredLifecycleChanges,
                                      RuntimePhase::EndFrame};
        Check(phases.size() == expected.size() * 2);
        for (std::size_t index = 0; index < expected.size(); ++index) {
            Check(phases[index] == expected[index]);
            Check(phases[index + expected.size()] == expected[index]);
        }
        Check(observed->fixedTicks == std::vector<std::uint64_t>{1});
        Check(observed->lastCompletedSimulationTick == 1);
        Check(observed->lastInterpolationAlpha == 0.0);
    }

    TEST_CASE("Catch Up Is Bounded And Observable", "[unit][runtime]") {
        DeterministicClock clock;
        auto participant = std::make_unique<RecordingParticipant>();
        RecordingParticipant *observed = participant.get();
        auto host = MakeHost(clock);
        Check(host->AddParticipant(std::move(participant)).HasValue());
        Check(host->Startup().HasValue());
        Check(host->RunFrame().HasValue());
        clock.Advance(Duration::FromMilliseconds(250));
        Check(host->RunFrame().HasValue());
        Check(observed->fixedTicks.size() == 5);
        Check(observed->lastCompletedSimulationTick == 5);
        Check(observed->lastInterpolationAlpha >= 0.0 && observed->lastInterpolationAlpha < 1.0);
        Check(observed->lastRealDeltaWasClamped == false);
        const FrameSchedulerStatistics statistics = host->Statistics();
        Check(statistics.catchUpLimitedFrameCount == 1);
        Check(statistics.totalDroppedFixedSteps == 9);
        Check(statistics.totalDroppedSimulationTime.ToNanoseconds() == 9 * 16'666'667LL);

        clock.Advance(Duration::FromMilliseconds(500));
        Check(host->RunFrame().HasValue());
        Check(host->Statistics().maximumDeltaClampCount == 1);
    }

    TEST_CASE("Negative Delta Is Normalized", "[unit][runtime]") {
        DeterministicClock clock(Duration::FromMilliseconds(100));
        auto participant = std::make_unique<RecordingParticipant>();
        RecordingParticipant *observed = participant.get();
        auto host = MakeHost(clock);
        Check(host->AddParticipant(std::move(participant)).HasValue());
        Check(host->Startup().HasValue());
        Check(host->RunFrame().HasValue());
        clock.Advance(Duration::FromMilliseconds(-10));
        Check(host->RunFrame().HasValue());
        Check(observed->lastVariableDelta == Duration{});
        Check(host->Statistics().negativeDeltaNormalizationCount == 1);
    }

    TEST_CASE("Startup Failure Rolls Back And Shutdown Is Reverse Ordered", "[unit][runtime]") {
        DeterministicClock clock;
        std::vector<int> order;
        auto host = MakeHost(clock);
        auto first = std::make_unique<RecordingParticipant>();
        first->id = 1;
        first->shutdownOrder = &order;
        auto second = std::make_unique<RecordingParticipant>();
        RecordingParticipant *failed = second.get();
        second->id = 2;
        second->shutdownOrder = &order;
        second->failStartup = true;
        auto third = std::make_unique<RecordingParticipant>();
        RecordingParticipant *skipped = third.get();
        third->id = 3;
        third->shutdownOrder = &order;
        Check(host->AddParticipant(std::move(first)).HasValue());
        Check(host->AddParticipant(std::move(second)).HasValue());
        Check(host->AddParticipant(std::move(third)).HasValue());
        Check(host->Startup().HasError());
        Check(order == std::vector<int>{1});
        Check(failed->shutdownCount == 0);
        Check(skipped->startupCount == 0);

        order.clear();
        auto successful = MakeHost(clock);
        for (int id = 1; id <= 3; ++id) {
            auto service = std::make_unique<RecordingParticipant>();
            service->id = id;
            service->shutdownOrder = &order;
            Check(successful->AddParticipant(std::move(service)).HasValue());
        }
        Check(successful->Startup().HasValue());
        successful->Shutdown();
        successful->Shutdown();
        Check(order == std::vector<int>({3, 2, 1}));
    }

    TEST_CASE("Failures And Exceptions Gate Presentation", "[unit][runtime]") {
        constexpr std::array gatedPhases{RuntimePhase::RenderExtraction, RuntimePhase::RenderExecution, RuntimePhase::RenderGui};
        for (const RuntimePhase failedPhase : gatedPhases) {
            DeterministicClock clock;
            std::vector<RuntimePhase> phases;
            auto participant = std::make_unique<RecordingParticipant>(&phases);
            participant->failPhase = failedPhase;
            auto host = MakeHost(clock);
            Check(host->AddParticipant(std::move(participant)).HasValue());
            Check(host->Startup().HasValue());
            const Result<void> failed = host->RunFrame();
            Check(failed.HasError());
            Check(failed.ErrorValue().code.Value() == "runtime.test.failure");
            Check(std::find(phases.begin(), phases.end(), RuntimePhase::Presentation) == phases.end());
            Check(std::find(phases.begin(), phases.end(), RuntimePhase::EndFrame) == phases.end());
            Check(host->State() == RuntimeLifecycleState::Stopped);
        }

        DeterministicClock clock;
        auto participant = std::make_unique<RecordingParticipant>();
        participant->throwPhase = RuntimePhase::VariableUpdate;
        auto host = MakeHost(clock);
        Check(host->AddParticipant(std::move(participant)).HasValue());
        Check(host->Startup().HasValue());
        const Result<void> failed = host->RunFrame();
        Check(failed.HasError());
        Check(failed.ErrorValue().code.Value() == "runtime.lifecycle.unexpected_exception");
    }

    TEST_CASE("Failed Fixed Tick Does Not Commit", "[unit][runtime]") {
        DeterministicClock clock;
        auto participant = std::make_unique<RecordingParticipant>();
        participant->failFixedTick = 1;
        auto host = MakeHost(clock);
        Check(host->AddParticipant(std::move(participant)).HasValue());
        Check(host->Startup().HasValue());
        Check(host->RunFrame().HasValue());
        clock.Advance(Duration::FromNanoseconds(16'666'667));
        Check(host->RunFrame().HasError());
        Check(host->Statistics().completedSimulationTick == 0);
        Check(host->Statistics().totalDroppedFixedSteps == 0);
    }

    TEST_CASE("Suspend Pumps Only Safe Phases And Resume Drops Wall Time", "[unit][runtime]") {
        DeterministicClock clock;
        std::vector<RuntimePhase> phases;
        auto participant = std::make_unique<RecordingParticipant>(&phases);
        RecordingParticipant *observed = participant.get();
        auto host = MakeHost(clock);
        Check(host->AddParticipant(std::move(participant)).HasValue());
        Check(host->Startup().HasValue());
        Check(host->RunFrame().HasValue());
        phases.clear();
        Check(host->Suspend().HasValue());
        clock.Advance(Duration::FromMilliseconds(500));
        Check(host->RunFrame().HasValue());
        constexpr std::array suspended{RuntimePhase::BeginFrame, RuntimePhase::PollPlatformEvents,
                                       RuntimePhase::ApplyQueuedOwnerThreadCommands, RuntimePhase::NetworkPoll, RuntimePhase::EndFrame};
        Check(phases.size() == suspended.size());
        for (std::size_t index = 0; index < suspended.size(); ++index)
            Check(phases[index] == suspended[index]);
        Check(host->Resume().HasValue());
        phases.clear();
        Check(host->RunFrame().HasValue());
        Check(observed->fixedTicks.empty());
    }

    TEST_CASE("Mid Frame Cancellation Stops Later Phases", "[unit][runtime]") {
        DeterministicClock clock;
        std::vector<RuntimePhase> phases;
        auto host = MakeHost(clock);
        auto participant = std::make_unique<RecordingParticipant>(&phases);
        participant->host = host.get();
        participant->cancelPhase = RuntimePhase::VariableUpdate;
        Check(host->AddParticipant(std::move(participant)).HasValue());
        Check(host->Startup().HasValue());
        const Result<void> cancelled = host->RunFrame();
        Check(cancelled.HasError());
        Check(cancelled.ErrorValue().code.Value() == "runtime.host.cancelled");
        Check(std::find(phases.begin(), phases.end(), RuntimePhase::RenderExtraction) == phases.end());
        Check(host->State() == RuntimeLifecycleState::Stopped);
    }

    std::uint64_t RunDeterministicCadence(const int frameCount) {
        DeterministicClock clock;
        auto participant = std::make_unique<RecordingParticipant>();
        RecordingParticipant *observed = participant.get();
        auto host = MakeHost(clock, FrameSchedulerConfig{.fixedStep = Duration::FromMilliseconds(10),
                                                         .maximumFrameDelta = Duration::FromMilliseconds(250),
                                                         .maximumCatchUpSteps = 25});
        Check(host->AddParticipant(std::move(participant)).HasValue());
        Check(host->Startup().HasValue());
        Check(host->RunFrame().HasValue());
        for (int frame = 0; frame < frameCount; ++frame) {
            const std::int64_t baseDelta = 1'000'000'000LL / frameCount;
            const std::int64_t remainder = frame == frameCount - 1 ? 1'000'000'000LL % frameCount : 0;
            clock.Advance(Duration::FromNanoseconds(baseDelta + remainder));
            Check(host->RunFrame().HasValue());
        }
        return observed->lastCompletedSimulationTick;
    }

    TEST_CASE("Fixed Simulation Is Independent Of Presentation Cadence", "[unit][runtime]") {
        const std::uint64_t thirtyHz = RunDeterministicCadence(30);
        const std::uint64_t sixtyHz = RunDeterministicCadence(60);
        const std::uint64_t highHz = RunDeterministicCadence(144);
        Check(thirtyHz == 100);
        Check(sixtyHz == thirtyHz);
        Check(highHz == thirtyHz);
    }

    TEST_CASE("Headless Null Renderer Uses Canonical Contract", "[unit][runtime]") {
        DeterministicClock clock;
        auto participant = std::make_unique<NullRenderParticipant>();
        NullRenderParticipant *observed = participant.get();
        auto host = MakeHost(clock);
        Check(host->AddParticipant(std::move(participant)).HasValue());
        Check(host->Startup().HasValue());
        Check(host->RunFrame().HasValue());
        Check(observed->presentCount == 1);
    }

    TEST_CASE("Successful Steady State Scheduler Does Not Allocate", "[unit][runtime]") {
        DeterministicClock clock;
        auto participant = std::make_unique<AllocationFreeParticipant>();
        auto host = MakeHost(clock);
        Check(host->AddParticipant(std::move(participant)).HasValue());
        Check(host->Startup().HasValue());
        Check(host->RunFrame().HasValue());
        clock.Advance(Duration::FromNanoseconds(16'666'667));
        gAllocationCount.store(0, std::memory_order_relaxed);
        Check(host->RunFrame().HasValue());
        Check(gAllocationCount.load(std::memory_order_relaxed) == 0);
    }
}  // namespace
