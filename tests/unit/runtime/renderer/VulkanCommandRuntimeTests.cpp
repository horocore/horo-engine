#include "VulkanCommandTestSupport.h"
#include "runtime/renderer/modules/vulkan/VulkanRenderBackendErrors.h"

#include <thread>

namespace Horo::Render::VulkanTest {
    TEST_CASE("Vulkan canonical submission signals WSI binary without replacing timeline completion", "[unit][renderer][vulkan]") {
        CommandFixture fixture;
        fixture.Initialize(1);
        const auto ticket = fixture.Record();
        const auto binary = NativeHandle<VkSemaphore>(9001);
        REQUIRE(fixture.runtime.Submit(ticket, {{1}, {1}, {}, {{1}, 1}}, binary).HasValue());
        REQUIRE(fixture.state.accepted.size() == 1);
        const auto &submitted = fixture.state.accepted.front();
        CHECK(submitted.signal.value == 1);
        CHECK(submitted.signal.semaphore != binary);
        CHECK(submitted.presentationSignal.semaphore == binary);
        CHECK(submitted.presentationSignal.value == 0);
        CHECK(submitted.presentationSignal.stageMask == VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT);
        CHECK(fixture.state.waitCalls == 0);
        fixture.runtime.Abort(ticket);
        CHECK(fixture.runtime.Begin({1}).HasError());
        fixture.state.Complete();
        REQUIRE(fixture.runtime.Poll().HasValue());
        const auto next = fixture.Record();
        CHECK(fixture.runtime.Submit(next, {{1}, {2}, {}, {{1}, 2}}, submitted.signal.semaphore).HasError());
        CHECK(fixture.state.accepted.size() == 1);
    }

    TEST_CASE("Vulkan command initialization rejects missing dispatch and rolls back partial native queues", "[unit][renderer][vulkan]") {
        CommandFixture fixture;
        SECTION("missing entry point") {
            const auto missing = [](VkDevice device, const char *name) {
                return std::string_view{name} == "vkQueueSubmit2" ? nullptr : Resolver(device, name);
            };
            const auto result = fixture.runtime.Initialize(NativeHandle<VkDevice>(1), fixture.queues, 2, 1024, 11, missing);
            REQUIRE(result.HasError());
            CHECK(result.ErrorValue().code.Value() == VulkanBackendErrors::EntryPointMissing.code.Value());
            CHECK(fixture.state.createdPools == 0);
        }
        SECTION("second pool failure") {
            fixture.state.failPoolAt = 2;
            CHECK(fixture.runtime.Initialize(NativeHandle<VkDevice>(1), fixture.queues, 2, 1024, 11, Resolver).HasError());
            CHECK(fixture.state.destroyedPools == 1);
            CHECK(fixture.state.destroyedSemaphores == 1);
        }
        SECTION("native alias cannot impersonate distinct Horo queues") {
            auto queues = fixture.queues;
            queues[1].native = queues[0].native;
            CHECK(fixture.runtime.Initialize(NativeHandle<VkDevice>(1), queues, 2, 1024, 11, Resolver).HasError());
            CHECK(fixture.state.createdPools == 0);
        }
        CHECK_FALSE(fixture.runtime.IsActive());
    }

    TEST_CASE("Vulkan graph command pins survive abort and bounded pending shutdown until exact native completion",
              "[unit][renderer][vulkan]") {
        CommandFixture fixture;
        fixture.Initialize(1);
        fixture.pins.lease = &fixture.lease;
        auto resource = std::make_shared<int>(19);
        std::weak_ptr<int> observed = resource;
        fixture.pins.resources.push_back(resource);
        resource.reset();
        const auto ticket = fixture.Record();
        REQUIRE(fixture.runtime.Retain(ticket, fixture.pins).HasValue());
        REQUIRE(fixture.runtime.Submit(ticket, {{1}, {1}, {}, {{1}, 5}}).HasValue());
        fixture.runtime.Abort(ticket);
        CHECK(fixture.lease.releases == 0);
        CHECK_FALSE(observed.expired());
        CHECK(fixture.runtime.Begin({1}).HasError());
        REQUIRE(fixture.runtime.BeginShutdown().HasValue());
        const auto pending = fixture.runtime.PollDrain();
        REQUIRE(pending.HasValue());
        CHECK(pending.Value() == RenderBackendDrainProgress::Pending);
        CHECK(fixture.state.destroyedPools == 0);
        fixture.state.Complete();
        Test::RequireDrainedOwner(fixture.runtime);
        CHECK(observed.expired());
        CHECK(fixture.lease.releases == 1);
        CHECK(fixture.pins.references == 0);
        CHECK(fixture.state.destroyedPools == 2);
        CHECK(fixture.state.destroyedSemaphores == 2);
        CHECK(fixture.state.waitCalls == 0);
    }

    TEST_CASE("Vulkan batch history accepts contiguous signals at capacity and rejects a new discontinuous range",
              "[unit][renderer][vulkan][submission]") {
        CommandFixture fixture;
        fixture.Initialize(1);
        for (std::uint64_t index = 1; index <= 512; ++index) {
            const auto ticket = fixture.Record();
            REQUIRE(fixture.runtime.Submit(ticket, {{1}, {index}, {}, {{1}, index * 2}}).HasValue());
            fixture.state.Complete();
            REQUIRE(fixture.runtime.Poll().HasValue());
        }
        const std::array contiguous{RenderGraphQueueSubmission{{{1}, {513}, {}, {{1}, 1025}}, {0, 1}}};
        CHECK(fixture.runtime.ValidateBatch(contiguous).HasValue());
        const std::array discontinuous{RenderGraphQueueSubmission{{{1}, {513}, {}, {{1}, 1026}}, {0, 1}}};
        const auto rejected = fixture.runtime.ValidateBatch(discontinuous);
        REQUIRE(rejected.HasError());
        CHECK(rejected.ErrorValue().code.Value() == VulkanBackendErrors::TimelineInvalid.code.Value());
        CHECK(fixture.runtime.ValidateBatch(contiguous).HasValue());
        CHECK(fixture.state.accepted.size() == 512);
    }

    TEST_CASE("Vulkan submits exact cross-queue waits and frontend timeline values without CPU waits", "[unit][renderer][vulkan]") {
        CommandFixture fixture;
        fixture.Initialize();
        const auto producer = fixture.Record({1});
        const auto consumer = fixture.Record({2});
        const std::array waits{RenderTimelinePoint{{1}, 7}};
        const RenderQueueSubmission dependent{{2}, {2}, waits, {{2}, 3}};
        CHECK(fixture.runtime.Submit(consumer, dependent).HasError());
        CHECK(fixture.state.accepted.empty());
        REQUIRE(fixture.runtime.Submit(producer, {{1}, {1}, {}, {{1}, 7}}).HasValue());
        REQUIRE(fixture.runtime.Submit(consumer, dependent).HasValue());
        REQUIRE(fixture.state.accepted.size() == 2);
        const auto &native = fixture.state.accepted[1];
        REQUIRE(native.waits.size() == 1);
        CHECK(native.waits[0].value == 7);
        CHECK(native.waits[0].semaphore == fixture.state.accepted[0].signal.semaphore);
        CHECK(native.waits[0].stageMask == VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT);
        CHECK(native.signal.value == 3);
        CHECK(native.queue == fixture.queues[1].native);
        CHECK(fixture.state.timelineOnly);
        CHECK(fixture.state.families == std::vector<std::uint32_t>{3, 7});
        CHECK(fixture.state.waitCalls == 0);
    }

    TEST_CASE("Vulkan command identities and timeline admissions do not wrap or reuse retired tickets", "[unit][renderer][vulkan]") {
        CommandFixture fixture;
        fixture.Initialize(1, 8);
        const auto first = fixture.Record();
        fixture.runtime.Abort(first);
        const auto second = fixture.Record();
        CHECK(first != second);
        CHECK(fixture.runtime.RecordingBuffer(first).HasError());
        CHECK(fixture.runtime.Submit(second, {{1}, {1}, {}, {{1}, 9}}).HasError());
        REQUIRE(fixture.runtime.Submit(second, {{1}, {1}, {}, {{1}, 8}}).HasValue());
        fixture.state.Complete();
        REQUIRE(fixture.runtime.Poll().HasValue());
        const auto third = fixture.Record();
        CHECK(fixture.runtime.Submit(third, {{1}, {1}, {}, {{1}, 9}}).HasError());
        CHECK(fixture.runtime.Submit(third, {{1}, {2}, {}, {{1}, 8}}).HasError());
        REQUIRE(fixture.runtime.Submit(third, {{1}, {2}, {}, {{1}, 9}}).HasValue());
        CHECK(fixture.state.accepted.size() == 2);
    }

    TEST_CASE("Vulkan completion polling never blocks and finite timeout is not device loss", "[unit][renderer][vulkan]") {
        CommandFixture fixture;
        fixture.Initialize();
        const auto ticket = fixture.Record();
        REQUIRE(fixture.runtime.Submit(ticket, {{1}, {1}, {}, {{1}, 1}}).HasValue());
        const auto poll = fixture.runtime.Observe({{{1}, 1}, RenderCpuWaitPurpose::Poll, std::chrono::nanoseconds{0}, 11});
        REQUIRE(poll.HasValue());
        CHECK_FALSE(poll.Value());
        CHECK(fixture.state.waitCalls == 0);
        const auto timeout = fixture.runtime.Observe({{{1}, 1}, RenderCpuWaitPurpose::DeterministicTest, std::chrono::nanoseconds{1}, 11});
        REQUIRE(timeout.HasError());
        CHECK(timeout.ErrorValue().code.Value() == VulkanBackendErrors::CompletionTimeout.code.Value());
        CHECK(fixture.runtime.IsActive());
        CHECK(fixture.state.waitCalls == 1);
        fixture.state.waitResult = VK_SUCCESS;
        REQUIRE(fixture.runtime.Observe({{{1}, 1}, RenderCpuWaitPurpose::DeterministicTest, std::chrono::milliseconds{1}, 11}).Value());
    }

    TEST_CASE("Vulkan stale completion cannot cross recreated device incarnations even when queue and value repeat",
              "[unit][renderer][vulkan]") {
        const auto first = AcquireRenderDeviceIncarnation();
        const auto second = AcquireRenderDeviceIncarnation();
        REQUIRE(first.HasValue());
        REQUIRE(second.HasValue());
        REQUIRE(first.Value() != second.Value());
        CommandFixture fixture;
        fixture.Initialize(1, 1024, first.Value());
        const auto oldTicket = fixture.Record();
        REQUIRE(fixture.runtime.Submit(oldTicket, {{1}, {1}, {}, {{1}, 1}}).HasValue());
        fixture.state.Complete();
        Test::RequireDrainedOwner(fixture.runtime);
        fixture.Initialize(1, 1024, second.Value());
        const auto current = fixture.Record();
        REQUIRE(fixture.runtime.Submit(current, {{1}, {1}, {}, {{1}, 1}}).HasValue());
        const auto before = fixture.state.counterCalls;
        const auto stale = fixture.runtime.Observe({{{1}, 1}, RenderCpuWaitPurpose::Poll, std::chrono::nanoseconds{0}, first.Value()});
        REQUIRE(stale.HasError());
        CHECK(stale.ErrorValue().code.Value() == VulkanBackendErrors::TimelineInvalid.code.Value());
        CHECK(fixture.state.counterCalls == before);
        const auto active = fixture.runtime.Observe({{{1}, 1}, RenderCpuWaitPurpose::Poll, std::chrono::nanoseconds{0}, second.Value()});
        REQUIRE(active.HasValue());
        CHECK_FALSE(active.Value());
    }

    TEST_CASE("Vulkan wrong-thread commands preserve the usable owner and unsent shutdown releases once", "[unit][renderer][vulkan]") {
        CommandFixture fixture;
        fixture.Initialize();
        fixture.pins.lease = &fixture.lease;
        const auto ticket = fixture.Record();
        REQUIRE(fixture.runtime.Retain(ticket, fixture.pins).HasValue());
        std::array<bool, 4> rejected{};
        std::jthread foreign([&] {
            const auto begin = fixture.runtime.Begin({1});
            const auto poll = fixture.runtime.Poll();
            const auto close = fixture.runtime.BeginShutdown();
            const auto finish = fixture.runtime.Finish(ticket);
            rejected = {begin.HasError() && begin.ErrorValue().code.Value() == VulkanBackendErrors::WrongThread.code.Value(),
                        poll.HasError() && poll.ErrorValue().code.Value() == VulkanBackendErrors::WrongThread.code.Value(),
                        close.HasError() && close.ErrorValue().code.Value() == VulkanBackendErrors::WrongThread.code.Value(),
                        finish.HasError() && finish.ErrorValue().code.Value() == VulkanBackendErrors::WrongThread.code.Value()};
        });
        foreign.join();
        CHECK(std::ranges::all_of(rejected, [](const bool value) {
            return value;
        }));
        CHECK(fixture.runtime.IsActive());
        Test::RequireDrainedOwner(fixture.runtime);
        CHECK(fixture.lease.releases == 1);
        CHECK(fixture.state.accepted.empty());
    }

    TEST_CASE("Vulkan submission allocation failure stays abortable while actual device loss closes and drains terminal ownership",
              "[unit][renderer][vulkan]") {
        CommandFixture fixture;
        fixture.Initialize();
        fixture.pins.lease = &fixture.lease;
        const auto ticket = fixture.Record();
        REQUIRE(fixture.runtime.Retain(ticket, fixture.pins).HasValue());
        SECTION("native host allocation failure") {
            fixture.state.submitResult = VK_ERROR_OUT_OF_HOST_MEMORY;
            const auto failed = fixture.runtime.Submit(ticket, {{1}, {1}, {}, {{1}, 1}});
            REQUIRE(failed.HasError());
            CHECK(failed.ErrorValue().code.Value() == VulkanBackendErrors::NativeCallFailed.code.Value());
            CHECK(fixture.runtime.IsActive());
            fixture.runtime.Abort(ticket);
            CHECK(fixture.lease.releases == 1);
        }
        SECTION("native device loss") {
            fixture.state.submitResult = VK_ERROR_DEVICE_LOST;
            const auto failed = fixture.runtime.Submit(ticket, {{1}, {1}, {}, {{1}, 1}});
            REQUIRE(failed.HasError());
            CHECK(failed.ErrorValue().code.Value() == VulkanBackendErrors::DeviceLost.code.Value());
            CHECK_FALSE(fixture.runtime.IsActive());
            CHECK(fixture.lease.releases == 0);
            fixture.runtime.Abort(ticket);
            CHECK(fixture.lease.releases == 0);
            Test::RequireDrainedOwner(fixture.runtime);
            CHECK(fixture.lease.releases == 1);
        }
    }

    TEST_CASE("Vulkan observation cannot infer issuance from a larger signaled counter", "[unit][renderer][vulkan]") {
        CommandFixture fixture;
        fixture.Initialize();
        const auto ticket = fixture.Record();
        REQUIRE(fixture.runtime.Submit(ticket, {{1}, {1}, {}, {{1}, 3}}).HasValue());
        const int before = fixture.state.counterCalls;
        const auto hole = fixture.runtime.Observe({{{1}, 2}, RenderCpuWaitPurpose::Poll, std::chrono::nanoseconds{0}, 11});
        REQUIRE(hole.HasError());
        CHECK(fixture.state.counterCalls == before);
        const auto issued = fixture.runtime.Observe({{{1}, 3}, RenderCpuWaitPurpose::Poll, std::chrono::nanoseconds{0}, 11});
        REQUIRE(issued.HasValue());
        CHECK_FALSE(issued.Value());
    }

}  // namespace Horo::Render::VulkanTest
