#include "runtime/renderer/modules/metal/MetalParallelRecording.h"
#include "runtime/renderer/modules/metal/MetalSubmittedGraphQueue.h"

#include <catch2/catch_test_macros.hpp>

/** @brief Native status seam for deterministic polling-policy tests, not qualification of driver execution. */
@interface HoroTestParallelCommandStatus : NSObject
@property(nonatomic) MTLCommandBufferStatus status;
@end
@implementation HoroTestParallelCommandStatus
@end

namespace Horo::Render::Detail {
    /** @brief Owner-visible pin release counter; never captured by GPU callbacks. */
    class CountingGraphLease final : public IRenderGraphResourceLease {
    public:
        void Release() noexcept override {
            ++released;
        }

        std::size_t released{};
    };

    TEST_CASE("Any recorded Metal buffer error prevents successful native retirement despite a successful final marker",
              "[unit][renderer][metal][parallel][completion]") {
        HoroTestParallelCommandStatus *first = [[HoroTestParallelCommandStatus alloc] init];
        HoroTestParallelCommandStatus *second = [[HoroTestParallelCommandStatus alloc] init];
        first.status = MTLCommandBufferStatusCompleted;
        second.status = MTLCommandBufferStatusCompleted;
        SECTION("earliest worker failed") {
            first.status = MTLCommandBufferStatusError;
        }
        SECTION("later worker failed") {
            second.status = MTLCommandBufferStatusError;
        }
        SECTION("earliest worker pending despite final completion") {
            first.status = MTLCommandBufferStatusScheduled;
        }
        std::vector<MetalRecordedOperation> operations;
        operations.push_back({.commands = (id<MTLCommandBuffer>)first});
        operations.push_back({.commands = (id<MTLCommandBuffer>)second});
        auto budget = std::make_shared<MetalRecordingBudget>();
        MetalParallelRecording recording{{1}, std::move(operations), budget};
        const auto completed = recording.NativeComplete();
        if (first.status == MTLCommandBufferStatusError || second.status == MTLCommandBufferStatusError) {
            REQUIRE(completed.HasError());
            CHECK(completed.ErrorValue().code.Value() == "render.metal.command_submission_failed");
        } else {
            REQUIRE(completed.HasValue());
            CHECK_FALSE(completed.Value());
        }
    }

    TEST_CASE("Native submitted graph queue keeps its owner lease until every worker completes or the domain closes",
              "[unit][renderer][metal][parallel][completion][lease]") {
        HoroTestParallelCommandStatus *worker = [[HoroTestParallelCommandStatus alloc] init];
        HoroTestParallelCommandStatus *tail = [[HoroTestParallelCommandStatus alloc] init];
        worker.status = MTLCommandBufferStatusScheduled;
        tail.status = MTLCommandBufferStatusCompleted;
        std::vector<MetalRecordedOperation> operations{{.commands = (id<MTLCommandBuffer>)worker}};
        auto budget = std::make_shared<MetalRecordingBudget>();
        auto recording = std::make_shared<MetalParallelRecording>(FrameToken{1}, std::move(operations), budget);
        CountingGraphLease lease;
        IRenderGraphResourceLease *activeLease = &lease;
        MetalSubmittedGraphQueue queue;
        queue.Initialize(3);
        queue.Remember((id<MTLCommandBuffer>)tail, activeLease, recording);
        CHECK(activeLease == nullptr);
        CHECK_FALSE(recording);
        REQUIRE(queue.Poll().HasValue());
        CHECK(queue.Count() == 1);
        CHECK(lease.released == 0);
        CHECK(budget->live.load() == 1);
        SECTION("worker completes before the owner safe point") {
            worker.status = MTLCommandBufferStatusCompleted;
            REQUIRE(queue.Poll().HasValue());
            CHECK(queue.Count() == 0);
            CHECK(lease.released == 1);
        }
        SECTION("worker GPU failure forbids successful retirement") {
            worker.status = MTLCommandBufferStatusError;
            REQUIRE(queue.Poll().HasError());
            CHECK(lease.released == 0);
            queue.ReleaseLeases();
            CHECK(lease.released == 1);
        }
        queue.Clear();
        CHECK(budget->live.load() == 0);
    }
}  // namespace Horo::Render::Detail
