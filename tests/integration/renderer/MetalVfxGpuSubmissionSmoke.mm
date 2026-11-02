#include "Horo/Runtime/Render/RenderFrontend.h"
#include "Horo/Runtime/Render/RenderGraphExecution.h"
#include "runtime/renderer/frontend/RenderFrontendResourceAccess.h"
#include "runtime/renderer/frontend/RenderSubmissionAllocator.h"
#include "runtime/renderer/modules/metal/MetalBackendModule.h"
#include "runtime/renderer/modules/metal/MetalSingleQueueSubmissions.h"
#include "runtime/renderer/modules/metal/MetalVfxGpuState.h"

#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <optional>
#include <thread>
#include <vector>

namespace Horo::Render::Test {
    namespace {
        /** @brief Retains exact typed failures in the private native journal qualification. */
        template <typename T> T Value(Result<T> result) {
            INFO((result.HasError() ? result.ErrorValue().code.Value() + ": " + result.ErrorValue().message : "Accepted."));
            REQUIRE(result.HasValue());
            return std::move(result).Value();
        }

        class JournalPort final : public IMetalPresentationPort {
        public:
            Result<void> CreateSurface() override {
                layer_ = [CAMetalLayer layer];
                return Result<void>::Success();
            }

            void *Layer() const noexcept override {
                return (__bridge void *)layer_;
            }

            void DestroySurface() noexcept override {
                layer_ = nil;
            }

        private:
            __strong CAMetalLayer *layer_{nil};
        };

        /** @brief Test-only bounded retirement; no successful assertion or ordinary runtime path waits on the CPU. */
        bool Terminal(id<MTLCommandBuffer> commands) {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
            while (commands && commands.status != MTLCommandBufferStatusCompleted && commands.status != MTLCommandBufferStatusError) {
                if (std::chrono::steady_clock::now() >= deadline)
                    return false;
                std::this_thread::sleep_for(std::chrono::milliseconds{1});
            }
            return true;
        }

        bool Drain(RenderFrontend &frontend) {
            if (frontend.BeginShutdown().HasError())
                return false;
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
            for (;;) {
                const auto progress = frontend.PollDrain();
                if (progress.HasError())
                    return false;
                if (progress.Value() == RenderBackendDrainProgress::Drained)
                    return true;
                if (std::chrono::steady_clock::now() >= deadline)
                    return false;
                std::this_thread::sleep_for(std::chrono::milliseconds{1});
            }
        }

        /** @brief Uses an actual selected frontend's queue topology and native queue, with a separate canonical allocator.
         * @details These points prove the private journal boundary only. They cannot become frontend step acknowledgements.
         */
        class JournalFixture final {
        public:
            ~JournalFixture() {
                gate_.signaledValue = 1;
                if (!Terminal(submitted_))
                    std::terminate();
                journal_.Clear();
                if (frontend_ && !Drain(*frontend_))
                    std::terminate();
            }

            void Initialize() {
                REQUIRE(RegisterMetalRenderBackend(registry_, port_, bridge_).HasValue());
                REQUIRE(registry_.Seal().HasValue());
                frontend_ = Value(
                    RenderFrontend::Create(registry_, RenderBackendId{"metal"}, {.requirePresentation = false, .maxFramesInFlight = 2}));
                const auto assignments = Value(Detail::RenderFrontendResourceAccess::QueueAssignments(*frontend_));
                queues_.assign(assignments.begin(), assignments.end());
                incarnation_ = frontend_->Capabilities().support.deviceIncarnation;
                REQUIRE(allocator_.Configure(queues_).HasValue());
                REQUIRE(journal_.Initialize(queues_, incarnation_).HasValue());
                PrepareGraph();
                queue_ = (__bridge id<MTLCommandQueue>)bridge_.CommandQueue();
                REQUIRE(queue_ != nil);
            }

            RenderQueueSubmission Reserve(std::span<const RenderTimelinePoint> waits = {}) {
                const auto submission = Value(allocator_.Reserve(RenderQueueRole::Graphics, waits));
                REQUIRE(allocator_.Admit(std::span{&submission, 1}).HasValue());
                return submission;
            }

            void BeginCommands() {
                commands_ = [queue_ commandBuffer];
                REQUIRE(commands_ != nil);
            }

            void Capture(const RenderQueueSubmission &submission) {
                BeginCommands();
                REQUIRE(CaptureResult(submission).HasValue());
            }

            Result<void> CaptureResult(const RenderQueueSubmission &submission) {
                const std::array partitions{RenderGraphQueueSubmission{submission, {0, 1}}};
                const RenderGraphExecutionRequest request{.graph = *graph_, .workloads = {}, .resources = {}, .submissions = partitions};
                return journal_.Capture(request, commands_);
            }

            Result<bool> Observe(RenderTimelinePoint point) const {
                return journal_.Observe({.completion = point, .expectedDeviceIncarnation = incarnation_});
            }

            void CommitGated() {
                gate_ = [queue_.device newSharedEvent];
                REQUIRE(gate_ != nil);
                [commands_ encodeWaitForEvent:gate_ value:1];
                [commands_ commit];
                submitted_ = commands_;
                journal_.Commit(commands_);
            }

            void Complete(RenderTimelinePoint point) {
                gate_.signaledValue = 1;
                REQUIRE(Terminal(submitted_));
                REQUIRE(submitted_.status == MTLCommandBufferStatusCompleted);
                REQUIRE(Value(Observe(point)));
            }

            void Abort() {
                journal_.Abort(commands_);
                commands_ = nil;
            }

        private:
            void PrepareGraph() {
                auto builder = Value(RenderGraphBuilder::Create({.maxPasses = 1, .maxResources = 1, .maxUsages = 1}));
                REQUIRE(builder.AddPass(RenderPassKind::Graphics, RenderQueueRole::Graphics).HasValue());
                const auto graph = Value(builder.Finalize());
                const auto schedule = Value(CompileRenderGraph(graph));
                const auto synchronization = Value(SynthesizeRenderGraphSynchronization(graph, schedule, queues_, {}));
                graph_.emplace(Value(CompileRenderGraphExecution(graph, schedule, synchronization, queues_)));
            }

            JournalPort port_;
            MetalEditorGraphicsBridge bridge_;
            RenderBackendRegistry registry_;
            std::unique_ptr<RenderFrontend> frontend_;
            Detail::RenderSubmissionAllocator allocator_;
            Detail::MetalSingleQueueSubmissions journal_;
            std::vector<RenderQueueAssignment> queues_;
            std::optional<CompiledRenderGraphExecution> graph_;
            std::uint64_t incarnation_{};
            __strong id<MTLCommandQueue> queue_{nil};
            __strong id<MTLCommandBuffer> commands_{nil}, submitted_{nil};
            __strong id<MTLSharedEvent> gate_{nil};
        };
    }  // namespace

    TEST_CASE("Metal private queue journal separates encoding, native commit and actual completion",
              "[integration][renderer][metal][vfx-gpu][gpu][private-runtime][submission]") {
        INFO("Separate canonical allocator: this is private journal proof, not frontend acknowledgement or whole GPU VFX acceptance.");
        JournalFixture fixture;
        fixture.Initialize();
        const auto producer = fixture.Reserve();
        fixture.Capture(producer);
        REQUIRE(fixture.Observe(producer.signal).HasError());
        fixture.CommitGated();
        REQUIRE_FALSE(Value(fixture.Observe(producer.signal)));
        fixture.Complete(producer.signal);
        const std::array waits{producer.signal};
        const auto consumer = fixture.Reserve(waits);
        fixture.Capture(consumer);
        fixture.Abort();
        REQUIRE(fixture.Observe(consumer.signal).HasError());
    }

    TEST_CASE("Metal private queue journal rejects future and abandoned producer waits",
              "[integration][renderer][metal][vfx-gpu][gpu][private-runtime][submission]") {
        JournalFixture fixture;
        fixture.Initialize();
        const auto abandoned = fixture.Reserve();
        fixture.Capture(abandoned);
        fixture.Abort();
        const auto candidate = fixture.Reserve();
        const auto future = fixture.Reserve();
        fixture.BeginCommands();
        auto invalid = candidate;
        const std::array futureWait{future.signal};
        invalid.waits = futureWait;
        REQUIRE(fixture.CaptureResult(invalid).HasError());
        const std::array abandonedWait{abandoned.signal};
        invalid.waits = abandonedWait;
        REQUIRE(fixture.CaptureResult(invalid).HasError());
    }

    TEST_CASE("Metal retirement cursor reaches a later quiescent owner behind a live prefix",
              "[integration][renderer][metal][vfx-gpu][retirement][ownership-model]") {
        INFO("CPU ownership/fairness model only: no prepared handles, shader payloads, dispatch or native GPU completion is fabricated.");
        auto first = std::make_shared<Detail::MetalVfxGpuSimulation>();
        auto second = std::make_shared<Detail::MetalVfxGpuSimulation>();
        auto candidate = std::make_shared<Detail::MetalVfxGpuSimulation>();
        const std::weak_ptr<Detail::MetalVfxGpuSimulation> retired = candidate;
        std::vector<std::shared_ptr<Detail::MetalVfxGpuSimulation>> inventory{first, second, std::move(candidate)};
        std::size_t cursor{};
        Detail::DrainRetiredMetalVfxGpuSimulations(inventory, cursor, 2);
        REQUIRE(inventory.size() == 3);
        REQUIRE_FALSE(retired.expired());
        Detail::DrainRetiredMetalVfxGpuSimulations(inventory, cursor, 1);
        REQUIRE(retired.expired());
        REQUIRE(inventory.size() == 2);
        REQUIRE(inventory[0] == first);
        REQUIRE(inventory[1] == second);
        inventory.clear();
        Detail::DrainRetiredMetalVfxGpuSimulations(inventory, cursor, 1);
        REQUIRE(cursor == 0);
    }

}  // namespace Horo::Render::Test
