#include "../../../support/renderer/RenderBackendDrainTestSupport.h"
#include "RenderFrontendTestSupport.h"

#include <chrono>
#include <thread>

namespace Horo::Tests::RenderFrontendTesting {
    TEST_CASE("Frontend submission observation rejects stale devices and active-frame blocking before backend work",
              "[renderer][frontend][submission]") {
        lifecycleState = {};
        lifecycleState.supportsSubmissionObservation = true;
        auto frontend = CreateTrackingFrontend();
        const auto incarnation = frontend->Capabilities().support.deviceIncarnation;
        RenderCpuWait wait{{{1}, 1}, RenderCpuWaitPurpose::Poll, std::chrono::nanoseconds{0}, incarnation};
        auto stale = wait;
        stale.expectedDeviceIncarnation = 0;
        REQUIRE(frontend->ObserveSubmission(stale).HasError());
        ++stale.expectedDeviceIncarnation;
        if (stale.expectedDeviceIncarnation == incarnation)
            ++stale.expectedDeviceIncarnation;
        REQUIRE(frontend->ObserveSubmission(stale).HasError());
        CHECK(lifecycleState.submissionObservations == 0);
        {
            auto begun = frontend->BeginFrame({.frameNumber = 1, .outputExtent = {16, 16}});
            REQUIRE(begun.HasValue());
            auto frame = std::move(begun).Value();
            wait.purpose = RenderCpuWaitPurpose::DeterministicTest;
            wait.timeout = std::chrono::milliseconds{1};
            REQUIRE(frontend->ObserveSubmission(wait).HasError());
            CHECK(lifecycleState.submissionObservations == 0);
            wait.purpose = RenderCpuWaitPurpose::Poll;
            wait.timeout = std::chrono::nanoseconds{0};
            REQUIRE(frontend->ObserveSubmission(wait).HasValue());
            CHECK(lifecycleState.submissionObservations == 1);
        }
        CHECK(lifecycleState.abortCount == 1);
        Horo::Render::Test::RequireDrainedOwner(*frontend);
    }

    TEST_CASE("Frontend observation contains standard and foreign backend exceptions while preserving typed errors",
              "[renderer][frontend][submission][exceptions]") {
        lifecycleState = {};
        lifecycleState.supportsSubmissionObservation = true;
        auto frontend = CreateTrackingFrontend();
        const RenderCpuWait wait{{{1}, 1},
                                 RenderCpuWaitPurpose::Poll,
                                 std::chrono::nanoseconds{0},
                                 frontend->Capabilities().support.deviceIncarnation};
        for (const auto failure : {ObservationFailure::StandardException, ObservationFailure::ForeignException}) {
            lifecycleState.observationFailure = failure;
            const auto observed = frontend->ObserveSubmission(wait);
            REQUIRE(observed.HasError());
            CHECK(observed.ErrorValue().code.Value() == "render.frontend.frame_exception");
            CHECK(lifecycleState.shutdownCount == 0);
        }
        lifecycleState.observationFailure = ObservationFailure::TypedError;
        const auto typed = frontend->ObserveSubmission(wait);
        REQUIRE(typed.HasError());
        CHECK(typed.ErrorValue().code.Value() == "render.test.observation");
        CHECK(typed.ErrorValue().message == "Injected typed observation failure.");
        lifecycleState.observationFailure = ObservationFailure::None;
        REQUIRE(frontend->ObserveSubmission(wait).Value());
        Horo::Render::Test::RequireDrainedOwner(*frontend);
    }

    TEST_CASE("Frontend retains closing owner through pending drain and permits bounded completion recovery",
              "[renderer][frontend][shutdown]") {
        lifecycleState = {};
        lifecycleState.supportsSubmissionObservation = true;
        auto frontend = CreateTrackingFrontend();
        const RenderCpuWait wait{{{1}, 1},
                                 RenderCpuWaitPurpose::Poll,
                                 std::chrono::nanoseconds{0},
                                 frontend->Capabilities().support.deviceIncarnation};
        lifecycleState.drainPending = true;
        REQUIRE(frontend->BeginShutdown().HasValue());
        REQUIRE(frontend->PollDrain().Value() == RenderBackendDrainProgress::Pending);
        CHECK(lifecycleState.shutdownCount == 0);
        CHECK(frontend->BeginFrame({.frameNumber = 1, .outputExtent = {16, 16}}).HasError());
        CHECK(frontend->CreateBuffer({.byteSize = 16, .usage = RenderBufferUsage::CopySource}, {}).HasError());
        const auto pending = frontend->ObserveSubmission(wait);
        REQUIRE(pending.HasValue());
        CHECK_FALSE(pending.Value());
        bool foreignRejected = false;
        std::thread worker{[&] {
            const auto closing = frontend->BeginShutdown();
            const auto drain = frontend->PollDrain();
            const auto observation = frontend->ObserveSubmission(wait);
            foreignRejected = closing.HasError() && drain.HasError() && observation.HasError();
        }};
        worker.join();
        CHECK(foreignRejected);
        lifecycleState.drainPending = false;
        const auto completed = frontend->ObserveSubmission(wait);
        REQUIRE(completed.HasValue());
        CHECK(completed.Value());
        Horo::Render::Test::RequireDrainedOwner(*frontend);
        CHECK(lifecycleState.shutdownCount == 1);
        CHECK(frontend->ObserveSubmission(wait).HasError());
    }

    TEST_CASE("Selected devices use one non-reused incarnation producer across owner recreation", "[renderer][frontend][submission]") {
        lifecycleState = {};
        auto first = CreateTrackingFrontend();
        const auto original = first->Capabilities().support.deviceIncarnation;
        Horo::Render::Test::RequireDrainedOwner(*first);
        first.reset();
        const auto interveningPeer = AcquireRenderDeviceIncarnation();
        REQUIRE(interveningPeer.HasValue());
        auto second = CreateTrackingFrontend();
        const auto recreated = second->Capabilities().support.deviceIncarnation;
        CHECK(original != 0);
        CHECK(original != interveningPeer.Value());
        CHECK(recreated != original);
        CHECK(recreated != interveningPeer.Value());
        Horo::Render::Test::RequireDrainedOwner(*second);
    }

    TEST_CASE("Foreign resource retirement cannot release an owner-thread native lifetime", "[renderer][frontend][retirement]") {
        lifecycleState = {};
        auto frontend = CreateTrackingFrontend();
        const auto created = frontend->CreateBuffer({.byteSize = 16, .usage = RenderBufferUsage::CopySource}, {});
        REQUIRE(created.HasValue());
        REQUIRE(frontend->ProcessResourceRequests().HasValue());
        bool rejected = false;
        std::thread worker{[&] {
            const auto released = frontend->ReleaseBuffer(created.Value().handle);
            rejected = released.HasError() && released.ErrorValue().code.Value() == "render.frontend.wrong_thread";
        }};
        worker.join();
        CHECK(rejected);
        CHECK(lifecycleState.destroyBufferCount == 0);
        REQUIRE(frontend->ReleaseBuffer(created.Value().handle).HasValue());
        CHECK(lifecycleState.destroyBufferCount == 1);
        Horo::Render::Test::RequireDrainedOwner(*frontend);
    }

}  // namespace Horo::Tests::RenderFrontendTesting
