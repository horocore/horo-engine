#pragma once

/** @file RuntimeLifecycleTestFixture.h
 * @brief Real lifecycle participants for phase ordering, rollback and allocation tests.
 */
#include "Horo/Runtime/Render/NullBackendModule.h"
#include "Horo/Runtime/Render/RenderFrontend.h"
#include "Horo/Runtime/RuntimeHost.h"
#include "Horo/Runtime/RuntimeLifecycle.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <optional>
#include <stdexcept>
#include <vector>

namespace Horo::Runtime::LifecycleTests {
    using Horo::Render::FrameDescriptor;
    using Horo::Render::RegisterNullRenderBackend;
    using Horo::Render::RenderBackendConfig;
    using Horo::Render::RenderBackendId;
    using Horo::Render::RenderBackendRegistry;
    using Horo::Render::RenderFrameScope;
    using Horo::Render::RenderFrontend;
    using Horo::Render::RenderPassDescriptor;
    using Horo::Render::RenderPassId;
    using Horo::Render::RenderPassKind;

    inline void Check(const bool condition) {
        REQUIRE((condition));
    }

    [[nodiscard]] inline Error TestError(const char *code = "runtime.test.failure") {
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
            if (phase == RuntimePhase::VariableUpdate) {
                presentationGeneration = context.presentationClockGeneration;
                presentationContinuity = context.presentationContinuity;
                ++variableUpdateCount;
            }
            lastVariableDelta = context.variableDelta;
            lastInterpolationAlpha = context.interpolationAlpha;
            lastCompletedSimulationTick = context.completedSimulationTick;
            lastDroppedSimulationTime = context.droppedSimulationTime;
            lastRealDeltaWasClamped = context.realDeltaWasClamped;
            lastCommittedFixedStep = context.committedFixedStep;
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
            fixedEvidence.push_back({context.simulationTick, context.attemptNumber, context.frameNumber, context.fixedDelta});
            if (throwFixedTick == context.simulationTick)
                throw std::runtime_error{"Injected fixed exception."};
            if (cancelFixedTick == context.simulationTick)
                host->RequestShutdown();
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
        std::vector<CommittedFixedStepEvidence> fixedEvidence;
        std::vector<int> *shutdownOrder{};
        RuntimeHost *host{};
        std::optional<RuntimePhase> failPhase;
        std::optional<RuntimePhase> throwPhase;
        std::optional<RuntimePhase> cancelPhase;
        std::uint64_t failFixedTick{};
        std::uint64_t throwFixedTick{};
        std::uint64_t cancelFixedTick{};
        CommittedFixedStepEvidence lastCommittedFixedStep;
        Duration lastVariableDelta{};
        double lastInterpolationAlpha{};
        std::uint64_t lastCompletedSimulationTick{};
        Duration lastDroppedSimulationTime{};
        bool lastRealDeltaWasClamped{};
        std::uint64_t presentationGeneration{};
        PresentationClockContinuity presentationContinuity{PresentationClockContinuity::Initial};
        std::uint64_t variableUpdateCount{};
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

    [[nodiscard]] inline std::unique_ptr<RuntimeHost> MakeHost(DeterministicClock &clock, const FrameSchedulerConfig config = {}) {
        auto created = RuntimeHost::Create(clock, config);
        Check(created.HasValue());
        return std::move(created).Value();
    }

}  // namespace Horo::Runtime::LifecycleTests
