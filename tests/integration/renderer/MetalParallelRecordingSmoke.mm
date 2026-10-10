#include "Horo/Foundation/JobSystem.h"
#include "RenderMemoryTestSupport.h"
#include "runtime/renderer/modules/metal/MetalParallelRecording.h"
#include "runtime/renderer/modules/metal/MetalResourceInstances.h"
#include "runtime/renderer/modules/metal/MetalResourceRuntime.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstring>
#include <thread>

namespace Horo::Render::Detail {
    namespace {
        /** @brief Waits only in the explicit opt-in GPU test, never in a normal frame path. */
        template <typename Predicate> void RequireBoundedCompletion(Predicate predicate) {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
            while (!predicate()) {
                REQUIRE(std::chrono::steady_clock::now() < deadline);
                std::this_thread::yield();
            }
        }

        /** @brief Runs one actual worker recording and checks its durable terminal result within the test budget. */
        void RequireWorkerRecord(JobSystem &jobs, const std::shared_ptr<MetalParallelRecording> &recording, const std::size_t index) {
            auto admitted = jobs.SubmitResult({}, [recording, index](const CancellationToken &token) {
                return recording->Record(index, token);
            });
            REQUIRE(admitted.HasValue());
            RequireBoundedCompletion([&] {
                return admitted.Value().Snapshot()->terminalResult.has_value();
            });
            REQUIRE_FALSE(admitted.Value().Snapshot()->terminalResult->error.has_value());
        }

        /** @brief Captures an owned native operation prefix; a failed capture releases every preceding operation. */
        std::vector<MetalRecordedOperation> CaptureWorkerOperations(MetalResourceRuntime &resources, id<MTLCommandQueue> queue,
                                                                    const std::span<const RenderGraphWorkload> workloads,
                                                                    const std::span<const RenderGraphResourceInstance> instances) {
            std::vector<MetalRecordedOperation> operations;
            for (const auto &workload : workloads) {
                auto captured = resources.CaptureGraphOperation(workload, instances, [queue commandBuffer], nil);
                REQUIRE(captured.HasValue());
                operations.push_back(std::move(captured).Value());
            }
            return operations;
        }

        /** @brief Realizes one checked placement-backed buffer; the resource runtime remains its owner. */
        std::uint64_t RequirePlacedBuffer(MetalResourceRuntime &resources, const RenderBufferDescriptor &descriptor,
                                          const RenderMemoryCostPlan &cost, const std::uint64_t slot,
                                          const std::span<const std::byte> initialBytes = {}) {
            const auto created = resources.CreateBuffer(descriptor, initialBytes, TestSupport::PlacementFor(cost, slot, slot));
            REQUIRE(created.HasValue());
            return created.Value();
        }
    }  // namespace

    TEST_CASE("Actual Metal workers encode reversed completion and owner commits copy hazards in canonical order",
              "[integration][renderer][metal][parallel][gpu]") {
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        REQUIRE(device != nil);
        id<MTLCommandQueue> queue = [device newCommandQueueWithMaxCommandBufferCount:MetalRecordingBudget::NativeQueueCapacity];
        REQUIRE(queue != nil);
        MetalResourceRuntime resources;
        resources.Initialize((__bridge void *)device, (__bridge void *)queue);
        const std::array<std::byte, 16> bytes{std::byte{0x35}, std::byte{0x92}, std::byte{0xa7}, std::byte{0x01}};
        const RenderBufferDescriptor descriptor{.byteSize = bytes.size(),
                                                .usage = RenderBufferUsage::CopySource | RenderBufferUsage::CopyDestination,
                                                .access = RenderBufferAccess::HostVisible};
        const auto cost = resources.QueryBufferMemoryCost(descriptor);
        REQUIRE(cost.HasValue());
        const auto first = RequirePlacedBuffer(resources, descriptor, cost.Value(), 1, bytes);
        const auto middle = RequirePlacedBuffer(resources, descriptor, cost.Value(), 2);
        const auto last = RequirePlacedBuffer(resources, descriptor, cost.Value(), 3);
        const RenderGraphResourceId firstId{{1}, 1};
        const RenderGraphResourceId middleId{{1}, 2};
        const RenderGraphResourceId lastId{{1}, 3};
        const std::array instances{RenderGraphResourceInstance{firstId, first}, RenderGraphResourceInstance{middleId, middle},
                                   RenderGraphResourceInstance{lastId, last}};
        const std::array workloads{RenderGraphWorkload{RenderGraphBufferCopy{firstId, middleId, 0, 0, bytes.size()}},
                                   RenderGraphWorkload{RenderGraphBufferCopy{middleId, lastId, 0, 0, bytes.size()}}};
        auto operations = CaptureWorkerOperations(resources, queue, workloads, instances);
        auto budget = std::make_shared<MetalRecordingBudget>();
        auto recording = std::make_shared<MetalParallelRecording>(FrameToken{1}, std::move(operations), budget);
        CHECK(budget->live.load() == 1);
        JobSystem jobs{{.workerCount = 2}};
        RequireWorkerRecord(jobs, recording, 1);
        CHECK_FALSE(recording->NativeComplete().Value());
        CHECK(recording->Accept().HasError());
        RequireWorkerRecord(jobs, recording, 0);
        REQUIRE(recording->Accept().HasValue());
        recording->Commit();
        RequireBoundedCompletion([&] {
            const auto completed = recording->NativeComplete();
            REQUIRE(completed.HasValue());
            return completed.Value();
        });
        auto *result = reinterpret_cast<MetalBufferInstance *>(static_cast<std::uintptr_t>(last));
        CHECK(std::memcmp(result->buffer.contents, bytes.data(), bytes.size()) == 0);
        jobs.Shutdown(ShutdownPolicy::Drain);
        recording.reset();
        CHECK(budget->live.load() == 0);
        resources.Shutdown();
    }

    TEST_CASE("Cancelled Metal payload owns placement heaps beyond resource-runtime shutdown without late submission",
              "[integration][renderer][metal][parallel][gpu][shutdown]") {
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        REQUIRE(device != nil);
        id<MTLCommandQueue> queue = [device newCommandQueue];
        MetalResourceRuntime resources;
        resources.Initialize((__bridge void *)device, (__bridge void *)queue);
        const RenderBufferDescriptor descriptor{.byteSize = 16,
                                                .usage = RenderBufferUsage::CopySource | RenderBufferUsage::CopyDestination,
                                                .access = RenderBufferAccess::HostVisible};
        const auto cost = resources.QueryBufferMemoryCost(descriptor);
        REQUIRE(cost.HasValue());
        const auto first = RequirePlacedBuffer(resources, descriptor, cost.Value(), 1);
        const auto last = RequirePlacedBuffer(resources, descriptor, cost.Value(), 2);
        const RenderGraphResourceId firstId{{1}, 1};
        const RenderGraphResourceId lastId{{1}, 2};
        const std::array instances{RenderGraphResourceInstance{firstId, first}, RenderGraphResourceInstance{lastId, last}};
        __weak id<MTLHeap> retainedHeap = nil;
        std::vector<MetalRecordedOperation> operations;
        {
            auto captured =
                resources.CaptureGraphOperation(RenderGraphBufferCopy{firstId, lastId, 0, 0, 16}, instances, [queue commandBuffer], nil);
            REQUIRE(captured.HasValue());
            retainedHeap = captured.Value().sourceHeap;
            operations.push_back(std::move(captured).Value());
        }
        auto budget = std::make_shared<MetalRecordingBudget>();
        auto recording = std::make_shared<MetalParallelRecording>(FrameToken{1}, std::move(operations), budget);
        recording->Cancel();
        resources.Shutdown();
        CHECK(retainedHeap != nil);
        CHECK(recording->Idle());
        CHECK(IsJobCancelled(recording->Record(0, {}).ErrorValue()));
        CHECK(recording->Accept().HasError());
        recording.reset();
        CHECK(budget->live.load() == 0);
    }
}  // namespace Horo::Render::Detail
