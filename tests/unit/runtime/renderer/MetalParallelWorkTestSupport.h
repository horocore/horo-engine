#pragma once

#include "Horo/Foundation/JobSystem.h"
#include "Horo/Runtime/Render/RenderFrontend.h"
#include "MetalRenderTestSupport.h"
#include "RenderGraphTestUtils.h"

#include <atomic>
#include <chrono>
#include <stdexcept>
#include <thread>

namespace Horo::Render::MetalBackendTests {
    /** @brief Observable test-native encoder and completion controls, never borrowed from a caller by jobs. */
    struct ParallelObservation final {
        std::atomic<bool> releaseFirst{false};
        std::array<std::atomic<bool>, 2> entered{};
        std::array<std::atomic<bool>, 2> finished{};
        std::array<std::thread::id, 2> workerThreads{};
        std::vector<RenderGraphWorkload> submitted;
        std::thread::id submitThread;
        std::size_t failIndex{2};
        bool nativeComplete{false};
        bool accepted{false};
        bool throwCapture{false};
        bool throwAccept{false};
        bool foreignException{false};
        std::size_t captureCalls{}; /**< Render-owner-only native boundary observations. */
        std::size_t acceptanceCalls{};
        std::size_t throwRecordIndex{2};
        std::atomic<std::uint32_t> liveRecordings{};
    };

    /** @brief Owns copied operation payload; artificial encoder gate permits deterministic real-worker lifecycle checks. */
    class TestNativeRecording final : public IRenderParallelGraphRecording {
    public:
        TestNativeRecording(const RenderGraphExecutionRequest &request, std::shared_ptr<ParallelObservation> observation)
            : frame_(request.frame), observation_(std::move(observation)) {
            for (const auto &binding : request.workloads)
                payload_.push_back(binding.workload);
            lease_ = request.lease;
            observation_->liveRecordings.fetch_add(1);
        }

        ~TestNativeRecording() override {
            if (lease_)
                lease_->Release();
            observation_->liveRecordings.fetch_sub(1);
        }

        TestNativeRecording(const TestNativeRecording &) = delete;
        TestNativeRecording &operator=(const TestNativeRecording &) = delete;
        TestNativeRecording(TestNativeRecording &&) = delete;
        TestNativeRecording &operator=(TestNativeRecording &&) = delete;

        FrameToken Frame() const noexcept override {
            return frame_;
        }

        std::size_t PassCount() const noexcept override {
            return payload_.size();
        }

        Result<void> Record(const std::size_t index, const CancellationToken &cancellation) override {
            if (index >= payload_.size() || index >= observation_->entered.size())
                return Result<void>::Failure(MakeError(MetalBackendErrors::InvalidExecutionPlan));
            observation_->workerThreads[index] = std::this_thread::get_id();
            observation_->entered[index].store(true, std::memory_order_release);
            if (!AwaitRecordingAdmission(index, cancellation))
                return JobCancelled();
            if (index == observation_->throwRecordIndex) {
                if (observation_->foreignException)
                    throw 354;
                throw std::runtime_error{"injected worker recording failure"};
            }
            if (index == observation_->failIndex)
                return Result<void>::Failure(MakePortError("render.test.native_record_failed", "Injected worker encoding error."));
            observation_->finished[index].store(true, std::memory_order_release);
            return Result<void>::Success();
        }

        void Cancel() noexcept override {
            closed_.store(true);
        }

        const std::vector<RenderGraphWorkload> &Payload() const noexcept {
            return payload_;
        }

    private:
        /** @brief Keeps the deterministic first-worker gate independently cancellable, including terminal acknowledgement. */
        bool AwaitRecordingAdmission(const std::size_t index, const CancellationToken &cancellation) const {
            while (index == 0 && !observation_->releaseFirst.load()) {
                if (closed_.load() || cancellation.IsCancellationRequested())
                    return false;
                std::this_thread::yield();
            }
            return !closed_.load() && !cancellation.IsCancellationRequested();
        }

        FrameToken frame_;
        std::shared_ptr<ParallelObservation> observation_;
        std::vector<RenderGraphWorkload> payload_;
        std::atomic<bool> closed_{false};
        IRenderGraphResourceLease *lease_{}; /**< Last CPU/GPU recording owner releases the transferred lease. */
    };

    /** @brief Test-native runtime driven through production Metal backend and frontend rather than a planner fixture. */
    class ParallelMetalRuntime final : public FakeMetalRuntime {
    public:
        ParallelMetalRuntime(IMetalPresentationPort &port, PortState &state, std::shared_ptr<ParallelObservation> observation)
            : FakeMetalRuntime(port, state), observation_(std::move(observation)), owner_(std::this_thread::get_id()) {}

        RenderParallelRecordingCapabilities ParallelRecordingCapabilities() const noexcept override {
            return {2, 1};
        }

        Result<void> ValidateGraphWorkload(const RenderGraphWorkload &, std::span<const RenderGraphResourceInstance>) const override {
            return Result<void>::Success();
        }

        Result<std::shared_ptr<IRenderParallelGraphRecording>> PrepareParallelGraph(const RenderGraphExecutionRequest &request) override {
            ++observation_->captureCalls;
            if (observation_->throwCapture) {
                if (observation_->foreignException)
                    throw 354;
                throw std::runtime_error{"injected native capture failure"};
            }
            recording_ = std::make_shared<TestNativeRecording>(request, observation_);
            leaseRecording_ = recording_;
            return Result<std::shared_ptr<IRenderParallelGraphRecording>>::Success(recording_);
        }

        Result<void> AcceptParallelGraph(const std::shared_ptr<IRenderParallelGraphRecording> &recording) override {
            ++observation_->acceptanceCalls;
            if (observation_->throwAccept) {
                if (observation_->foreignException)
                    throw 354;
                throw std::runtime_error{"injected native acceptance failure"};
            }
            if (std::this_thread::get_id() != owner_ || recording != recording_)
                return Result<void>::Failure(MakeError(MetalBackendErrors::InvalidExecutionPlan));
            for (std::size_t index = 0; index < recording_->PassCount(); ++index) {
                if (!observation_->finished[index].load(std::memory_order_acquire))
                    return Result<void>::Failure(MakeError(MetalBackendErrors::InvalidExecutionPlan));
            }
            observation_->accepted = true;
            return Result<void>::Success();
        }

        Result<void> Present() override {
            if (recording_) {
                if (!observation_->accepted)
                    return Result<void>::Failure(MakeError(MetalBackendErrors::InvalidExecutionPlan));
                observation_->submitted = recording_->Payload();
                observation_->submitThread = std::this_thread::get_id();
                recording_.reset();
            }
            return FakeMetalRuntime::Present();
        }

        Result<void> BeginFrame(const FramebufferExtent extent) override {
            if (observation_->nativeComplete)
                ReleaseLease();
            return FakeMetalRuntime::BeginFrame(extent);
        }

        void AbortFrame() noexcept override {
            if (recording_)
                recording_->Cancel();
            recording_.reset();
            if (!observation_->accepted)
                ReleaseLease();
            FakeMetalRuntime::AbortFrame();
        }

        void Shutdown() noexcept override {
            AbortFrame();
            ReleaseLease();
            FakeMetalRuntime::Shutdown();
        }

    private:
        void ReleaseLease() noexcept {
            leaseRecording_.reset();  // Abandoned workers still retain the capsule; accepted capsules remain until native completion.
        }

        std::shared_ptr<ParallelObservation> observation_;
        std::thread::id owner_;
        std::shared_ptr<TestNativeRecording> recording_;
        std::shared_ptr<TestNativeRecording> leaseRecording_;
    };

    class ParallelMetalFactory final : public Detail::IMetalRuntimeFactory {
    public:
        ParallelMetalFactory(PortState &state, std::shared_ptr<ParallelObservation> observation)
            : state_(state), observation_(std::move(observation)) {}

        Result<std::unique_ptr<Detail::IMetalRuntime>> Create(IMetalPresentationPort &port, MetalEditorGraphicsBridge &) const override {
            return Result<std::unique_ptr<Detail::IMetalRuntime>>::Success(
                std::make_unique<ParallelMetalRuntime>(port, state_, observation_));
        }

    private:
        PortState &state_;
        std::shared_ptr<ParallelObservation> observation_;
    };

    inline CompiledRenderGraphExecution CompileParallelGraph(RenderGraphBuilder &builder,
                                                             const std::span<const RenderQueueAssignment> queues,
                                                             const std::span<const RenderGraphImportedState> initial = {}) {
        auto graph = Test::RequireGraph(builder);
        auto schedule = Test::RequireSchedule(graph);
        auto synchronization = SynthesizeRenderGraphSynchronization(graph, schedule, queues, initial);
        REQUIRE(synchronization.HasValue());
        auto compiled = CompileRenderGraphExecution(graph, schedule, synchronization.Value(), queues);
        REQUIRE(compiled.HasValue());
        return std::move(compiled).Value();
    }

    inline std::unique_ptr<RenderFrontend> MakeParallelFrontend(PortState &state, FakePresentationPort &port,
                                                                MetalEditorGraphicsBridge &bridge,
                                                                const std::shared_ptr<ParallelObservation> &observation) {
        ParallelMetalFactory factory{state, observation};
        RenderBackendRegistry registry;
        REQUIRE(Detail::RegisterMetalRenderBackendWithRuntimeFactory(registry, port, bridge, factory).HasValue());
        REQUIRE(registry.Seal().HasValue());
        auto created = RenderFrontend::Create(registry, RenderBackendId{"metal"}, {});
        REQUIRE(created.HasValue());
        return std::move(created).Value();
    }

    /** @brief Owns frontend composition; declare after JobSystem so owner cancellation precedes scheduler destruction. */
    struct ParallelHostFixture final {
        PortState state;
        FakePresentationPort port{state};
        MetalEditorGraphicsBridge bridge;
        std::shared_ptr<ParallelObservation> observation{std::make_shared<ParallelObservation>()};
        std::unique_ptr<RenderFrontend> frontend{MakeParallelFrontend(state, port, bridge, observation)};
    };

    /** @brief Owned two-pass producer input; callers may destroy or mutate it immediately after capture. */
    struct ParallelPrimaryGraph final {
        CompiledRenderGraphExecution graph;
        std::array<RenderGraphPassWorkload, 2> workloads;
    };

    inline ParallelPrimaryGraph MakeParallelPrimaryGraph(const std::array<PrimaryOutputAttachment, 2> operations = {}) {
        auto builder = Test::RequireBuilder();
        std::array<RenderGraphPassRef, 2> passIds;
        for (auto &id : passIds)
            id = Test::RequirePass(builder, RenderPassKind::Graphics, RenderQueueRole::Graphics);
        const std::array queues{RenderQueueAssignment{RenderQueueRole::Graphics, {1}}};
        return {CompileParallelGraph(builder, queues),
                {RenderGraphPassWorkload{passIds[0], operations[0]}, RenderGraphPassWorkload{passIds[1], operations[1]}}};
    }

    inline RenderFrameScope BeginParallelTestFrame(RenderFrontend &frontend) {
        auto begun = frontend.BeginFrame({1, {64, 64}});
        REQUIRE(begun.HasValue());
        return std::move(begun).Value();
    }

    template <typename Predicate> void AwaitWorker(Predicate predicate) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
        while (!predicate()) {
            REQUIRE(std::chrono::steady_clock::now() < deadline);
            std::this_thread::yield();
        }
    }

    inline Result<RenderParallelExecutionProgress> AwaitOwner(RenderFrameScope &frame) {
        std::optional<Result<RenderParallelExecutionProgress>> terminal;
        AwaitWorker([&] {
            auto progress = frame.PollParallelExecution();
            if (progress.HasError() || progress.Value() != RenderParallelExecutionProgress::Pending) {
                terminal.emplace(std::move(progress));
                return true;
            }
            return false;
        });
        return std::move(*terminal);
    }
}  // namespace Horo::Render::MetalBackendTests
