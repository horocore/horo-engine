#include "Horo/Runtime/Render/RenderQuery.h"
#include "Horo/Runtime/Render/RenderQueryErrors.h"
#include "Horo/Runtime/Render/RenderUpload.h"
#include "Horo/Runtime/Render/RenderUploadErrors.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstddef>
#include <memory>
#include <thread>

namespace RenderTransferTests {
    using namespace Horo;
    using namespace Horo::Render;
    using namespace std::chrono_literals;

    constexpr RenderResourceOwnerId Renderer{81};
    constexpr RenderResourceOwnerId OtherRenderer{82};
    constexpr RenderBufferHandle Destination{Renderer, 3, 1};
    constexpr RenderQueueId Queue{4};
    constexpr RenderTimelinePoint Completion{Queue, 11};

    [[nodiscard]] RenderUploadLimits UploadLimits() {
        return {.maximumPendingBytes = 8, .maximumRequestBytes = 8, .maximumAlignment = 8, .maximumRequests = 2};
    }

    [[nodiscard]] std::unique_ptr<RenderUploadQueue> CreateUploadQueue() {
        auto queue = RenderUploadQueue::Create(Renderer, UploadLimits());
        REQUIRE(queue.HasValue());
        return std::move(queue).Value();
    }

    [[nodiscard]] RenderUploadDescriptor UploadDescriptor(const std::size_t bytes = 4) {
        return {.destination = Destination, .destinationByteOffset = 2, .byteCount = bytes, .alignment = 4, .timeout = 5ms};
    }

    [[nodiscard]] std::array<std::byte, 4> UploadBytes() {
        return {std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}};
    }

    [[nodiscard]] Error UploadBackendFailure() {
        return {ErrorCode{"render.test.upload_failed"}, ErrorDomainId{"render.test"}, ErrorSeverity::Error, "Injected upload failure.", {}};
    }

    [[nodiscard]] std::unique_ptr<RenderTimestampQueryQueue> CreateQueryQueue(const bool supported = true) {
        auto queue = RenderTimestampQueryQueue::Create(Renderer, supported, {.maximumRequests = 2});
        REQUIRE(queue.HasValue());
        return std::move(queue).Value();
    }

    [[nodiscard]] RenderTimestampQueryDescriptor QueryDescriptor() {
        return {.queue = Queue, .timeout = 5ms};
    }

    TEST_CASE("Render upload copies bounded payloads and publishes readiness", "[unit][runtime][renderer][upload]") {
        auto queue = CreateUploadQueue();
        const auto bytes = UploadBytes();
        const auto request = queue->Request(UploadDescriptor(), bytes);
        REQUIRE(request.HasValue());
        CHECK(queue->Snapshot().pendingBytes == bytes.size());
        auto payload = queue->Payload(request.Value());
        REQUIRE(payload.HasValue());
        CHECK(payload.Value()[0] == bytes[0]);
        REQUIRE(queue->MarkSubmitted(request.Value(), Completion).HasValue());
        CHECK_FALSE(queue->IsReady(request.Value()).Value());
        REQUIRE(queue->Complete(request.Value()).HasValue());
        CHECK(queue->IsReady(request.Value()).Value());
        CHECK(queue->Snapshot().pendingBytes == 0);
        const Error readyFailure{ErrorCode{"render.test.upload_ready_failure"},
                                 ErrorDomainId{"render.test"},
                                 ErrorSeverity::Error,
                                 "Injected ready-state failure.",
                                 {}};
        CHECK(queue->Fail(request.Value(), readyFailure).ErrorValue().code.Value() == RenderUploadErrors::InvalidTransition.code.Value());
        CHECK(queue->Cancel(request.Value()).ErrorValue().code.Value() == RenderUploadErrors::InvalidTransition.code.Value());
        CHECK(queue->Timeout(request.Value()).ErrorValue().code.Value() == RenderUploadErrors::InvalidTransition.code.Value());
        CHECK(queue->Retire(request.Value()).ErrorValue().code.Value() == RenderUploadErrors::InvalidTransition.code.Value());
        REQUIRE(queue->Discard(request.Value()).HasValue());
        CHECK(queue->Snapshot().requestCount == 0);
    }

    TEST_CASE("Render upload rejects boundary and stale requests", "[unit][runtime][renderer][upload]") {
        CHECK_FALSE(RenderUploadLimits{.maximumPendingBytes = 4, .maximumRequestBytes = 5}.IsValid());
        auto queue = CreateUploadQueue();
        const auto bytes = UploadBytes();
        CHECK(queue->Request(UploadDescriptor(3), bytes).ErrorValue().code.Value() == RenderUploadErrors::PayloadSizeMismatch.code.Value());
        RenderUploadDescriptor foreign = UploadDescriptor();
        foreign.destination = RenderBufferHandle{OtherRenderer, 3, 1};
        CHECK(queue->Request(foreign, bytes).ErrorValue().code.Value() == RenderUploadErrors::InvalidDescriptor.code.Value());
        RenderUploadDescriptor invalidDestination = UploadDescriptor();
        invalidDestination.destination = RenderBufferHandle{};
        CHECK_FALSE(invalidDestination.IsValid());
        CHECK(queue->Request(invalidDestination, bytes).ErrorValue().code.Value() == RenderUploadErrors::InvalidDescriptor.code.Value());
        const auto request = queue->Request(UploadDescriptor(), bytes);
        REQUIRE(request.HasValue());
        CHECK(queue->MarkSubmitted(request.Value(), {}).ErrorValue().code.Value() == RenderUploadErrors::InvalidTransition.code.Value());
    }

    TEST_CASE("Render upload cancellation retains submitted staging until retirement", "[unit][runtime][renderer][upload]") {
        auto queue = CreateUploadQueue();
        const auto bytes = UploadBytes();
        const auto request = queue->Request(UploadDescriptor(), bytes);
        REQUIRE(request.HasValue());
        REQUIRE(queue->MarkSubmitted(request.Value(), Completion).HasValue());
        REQUIRE(queue->Cancel(request.Value()).HasValue());
        CHECK(queue->Snapshot().pendingBytes == bytes.size());
        REQUIRE(queue->Retire(request.Value()).HasValue());
        CHECK(queue->Snapshot().pendingBytes == 0);
        REQUIRE(queue->Discard(request.Value()).HasValue());
    }

    TEST_CASE("Render upload enforces metadata and staging capacity", "[unit][runtime][renderer][upload]") {
        auto queue = CreateUploadQueue();
        const auto bytes = UploadBytes();
        const auto first = queue->Request(UploadDescriptor(), bytes);
        const auto second = queue->Request(UploadDescriptor(), bytes);
        REQUIRE(first.HasValue());
        REQUIRE(second.HasValue());

        const auto denied = queue->Request(UploadDescriptor(), bytes);
        REQUIRE(denied.HasError());
        CHECK(denied.ErrorValue().code.Value() == RenderUploadErrors::CapacityExceeded.code.Value());
        CHECK(queue->Snapshot().pendingBytes == 8);
        CHECK(queue->Snapshot().failedAdmissionCount == 1);

        REQUIRE(queue->Cancel(first.Value()).HasValue());
        REQUIRE(queue->Discard(first.Value()).HasValue());
        const auto admittedAfterRetirement = queue->Request(UploadDescriptor(), bytes);
        REQUIRE(admittedAfterRetirement.HasValue());
    }

    TEST_CASE("Render upload rejects invalid publication transitions", "[unit][runtime][renderer][upload]") {
        auto queue = CreateUploadQueue();
        const auto bytes = UploadBytes();
        const auto backendFailure = UploadBackendFailure();
        const auto timedOut = queue->Request(UploadDescriptor(), bytes);
        REQUIRE(timedOut.HasValue());
        REQUIRE(queue->MarkSubmitted(timedOut.Value(), Completion).HasValue());
        const auto failed = queue->Request(UploadDescriptor(), bytes);
        REQUIRE(failed.HasValue());
        REQUIRE(queue->Fail(failed.Value(), backendFailure).HasValue());
        REQUIRE(queue->Discard(failed.Value()).HasValue());
        const auto pendingTimeout = queue->Request(UploadDescriptor(), bytes);
        REQUIRE(pendingTimeout.HasValue());
        REQUIRE(queue->Timeout(pendingTimeout.Value()).HasValue());
        REQUIRE(queue->Retire(pendingTimeout.Value()).HasValue());
        REQUIRE(queue->Discard(pendingTimeout.Value()).HasValue());
        const auto pending = queue->Request(UploadDescriptor(), bytes);
        REQUIRE(pending.HasValue());
        CHECK(queue->Payload(pending.Value()).HasValue());
        CHECK(queue->Complete(pending.Value()).ErrorValue().code.Value() == RenderUploadErrors::InvalidTransition.code.Value());
        CHECK(queue->Retire(pending.Value()).ErrorValue().code.Value() == RenderUploadErrors::InvalidTransition.code.Value());
        REQUIRE(queue->Cancel(pending.Value()).HasValue());
        REQUIRE(queue->Cancel(pending.Value()).HasValue());
        REQUIRE(queue->Complete(pending.Value()).HasValue());
        REQUIRE(queue->Fail(pending.Value(), backendFailure).HasValue());
        REQUIRE(queue->Discard(pending.Value()).HasValue());

        CHECK(queue->Payload(timedOut.Value()).ErrorValue().code.Value() == RenderUploadErrors::InvalidTransition.code.Value());
        REQUIRE(queue->Timeout(timedOut.Value()).HasValue());
        REQUIRE(queue->Timeout(timedOut.Value()).HasValue());
        REQUIRE(queue->Fail(timedOut.Value(), backendFailure).HasValue());
        REQUIRE(queue->Retire(timedOut.Value()).HasValue());
        REQUIRE(queue->Discard(timedOut.Value()).HasValue());
    }

    TEST_CASE("Render upload enforces owner thread affinity", "[unit][runtime][renderer][upload]") {
        auto queue = CreateUploadQueue();
        const auto bytes = UploadBytes();
        const auto backendFailure = UploadBackendFailure();
        const auto affinityRequest = queue->Request(UploadDescriptor(), bytes);
        REQUIRE(affinityRequest.HasValue());
        std::array<bool, 10> wrongThread{};
        std::thread worker([&] {
            wrongThread[0] =
                queue->Request(UploadDescriptor(), bytes).ErrorValue().code.Value() == RenderUploadErrors::WrongThread.code.Value();
            wrongThread[1] =
                queue->Payload(affinityRequest.Value()).ErrorValue().code.Value() == RenderUploadErrors::WrongThread.code.Value();
            wrongThread[2] =
                queue->State(affinityRequest.Value()).ErrorValue().code.Value() == RenderUploadErrors::WrongThread.code.Value();
            wrongThread[3] = queue->MarkSubmitted(affinityRequest.Value(), Completion).ErrorValue().code.Value() ==
                             RenderUploadErrors::WrongThread.code.Value();
            wrongThread[4] =
                queue->Complete(affinityRequest.Value()).ErrorValue().code.Value() == RenderUploadErrors::WrongThread.code.Value();
            wrongThread[5] = queue->Fail(affinityRequest.Value(), backendFailure).ErrorValue().code.Value() ==
                             RenderUploadErrors::WrongThread.code.Value();
            wrongThread[6] =
                queue->Cancel(affinityRequest.Value()).ErrorValue().code.Value() == RenderUploadErrors::WrongThread.code.Value();
            wrongThread[7] =
                queue->Timeout(affinityRequest.Value()).ErrorValue().code.Value() == RenderUploadErrors::WrongThread.code.Value();
            wrongThread[8] =
                queue->Retire(affinityRequest.Value()).ErrorValue().code.Value() == RenderUploadErrors::WrongThread.code.Value();
            wrongThread[9] =
                queue->Discard(affinityRequest.Value()).ErrorValue().code.Value() == RenderUploadErrors::WrongThread.code.Value();
        });
        worker.join();
        CHECK(wrongThread[0]);
        CHECK(wrongThread[1]);
        CHECK(wrongThread[2]);
        CHECK(wrongThread[3]);
        CHECK(wrongThread[4]);
        CHECK(wrongThread[5]);
        CHECK(wrongThread[6]);
        CHECK(wrongThread[7]);
        CHECK(wrongThread[8]);
        CHECK(wrongThread[9]);
    }

    TEST_CASE("Render upload stops admission and shuts down idempotently", "[unit][runtime][renderer][upload]") {
        auto queue = CreateUploadQueue();
        const std::array bytes{std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}};
        queue->StopAdmission();
        CHECK(queue->Request(UploadDescriptor(), bytes).ErrorValue().code.Value() == RenderUploadErrors::Stopped.code.Value());
        queue->Shutdown();
        queue->Shutdown();
    }

    TEST_CASE("Render timestamp query reports unsupported and owns ready result", "[unit][runtime][renderer][query]") {
        auto unsupported = CreateQueryQueue(false);
        CHECK(unsupported->Request(QueryDescriptor()).ErrorValue().code.Value() == RenderQueryErrors::Unsupported.code.Value());
        auto invalidDescriptor = QueryDescriptor();
        invalidDescriptor.timeout = 0ms;
        CHECK(CreateQueryQueue()->Request(invalidDescriptor).ErrorValue().code.Value() ==
              RenderQueryErrors::InvalidDescriptor.code.Value());

        auto queue = CreateQueryQueue();
        const auto query = queue->Request(QueryDescriptor());
        REQUIRE(query.HasValue());
        CHECK(queue->Acquire(query.Value()).ErrorValue().code.Value() == RenderQueryErrors::ResultPending.code.Value());
        REQUIRE(queue->MarkSubmitted(query.Value(), Completion).HasValue());
        REQUIRE(queue->Complete(query.Value(), 12ns).HasValue());
        CHECK(queue->IsReady(query.Value()).Value());
        const auto result = queue->Acquire(query.Value());
        REQUIRE(result.HasValue());
        CHECK(result.Value().timestamp == 12ns);
        CHECK(queue->Snapshot().requestCount == 0);
    }

    TEST_CASE("Render timestamp query preserves failures and enforces owner thread", "[unit][runtime][renderer][query]") {
        auto queue = CreateQueryQueue();
        const auto query = queue->Request(QueryDescriptor());
        REQUIRE(query.HasValue());
        const Error backendFailure{ErrorCode{"render.test.query_failed"},
                                   ErrorDomainId{"render.test"},
                                   ErrorSeverity::Error,
                                   "Injected query failure.",
                                   {}};
        REQUIRE(queue->Fail(query.Value(), backendFailure).HasValue());
        CHECK(queue->Acquire(query.Value()).ErrorValue().code.Value() == backendFailure.code.Value());
        REQUIRE(queue->Discard(query.Value()).HasValue());

        auto threadRequest = Result<RenderTimestampQueryId>::Success({});
        auto threadMark = Result<void>::Success();
        auto threadResult = Result<RenderTimestampQueryState>::Success(RenderTimestampQueryState::Ready);
        std::thread worker([&] {
            threadRequest = queue->Request(QueryDescriptor());
            threadMark = queue->MarkSubmitted(query.Value(), Completion);
            threadResult = queue->State(query.Value());
        });
        worker.join();
        REQUIRE(threadRequest.HasError());
        CHECK(threadRequest.ErrorValue().code.Value() == RenderQueryErrors::WrongThread.code.Value());
        REQUIRE(threadMark.HasError());
        CHECK(threadMark.ErrorValue().code.Value() == RenderQueryErrors::WrongThread.code.Value());
        REQUIRE(threadResult.HasError());
        CHECK(threadResult.ErrorValue().code.Value() == RenderQueryErrors::WrongThread.code.Value());
    }

    TEST_CASE("Render timestamp query rejects capacity exhaustion and ready discard", "[unit][runtime][renderer][query]") {
        auto queueResult = RenderTimestampQueryQueue::Create(Renderer, true, {.maximumRequests = 1});
        REQUIRE(queueResult.HasValue());
        auto queue = std::move(queueResult).Value();

        const auto query = queue->Request(QueryDescriptor());
        REQUIRE(query.HasValue());
        const auto denied = queue->Request(QueryDescriptor());
        REQUIRE(denied.HasError());
        CHECK(denied.ErrorValue().code.Value() == RenderQueryErrors::CapacityExceeded.code.Value());
        CHECK(queue->Snapshot().failedAdmissionCount == 1);

        REQUIRE(queue->MarkSubmitted(query.Value(), Completion).HasValue());
        REQUIRE(queue->Complete(query.Value(), 12ns).HasValue());
        CHECK(queue->Discard(query.Value()).ErrorValue().code.Value() == RenderQueryErrors::InvalidTransition.code.Value());
        REQUIRE(queue->Acquire(query.Value()).HasValue());
    }

    TEST_CASE("Render timestamp query preserves cancellation and timeout retirement rules", "[unit][runtime][renderer][query]") {
        auto queue = CreateQueryQueue();
        const Error backendFailure{ErrorCode{"render.test.query_failed_again"},
                                   ErrorDomainId{"render.test"},
                                   ErrorSeverity::Error,
                                   "Injected query failure.",
                                   {}};
        const auto timedOut = queue->Request(QueryDescriptor());
        REQUIRE(timedOut.HasValue());
        REQUIRE(queue->MarkSubmitted(timedOut.Value(), Completion).HasValue());

        const auto pending = queue->Request(QueryDescriptor());
        REQUIRE(pending.HasValue());
        CHECK(queue->MarkSubmitted(pending.Value(), {}).ErrorValue().code.Value() == RenderQueryErrors::InvalidTransition.code.Value());
        CHECK(queue->Complete(pending.Value(), 1ns).ErrorValue().code.Value() == RenderQueryErrors::InvalidTransition.code.Value());
        REQUIRE(queue->Cancel(pending.Value()).HasValue());
        REQUIRE(queue->Cancel(pending.Value()).HasValue());
        REQUIRE(queue->Complete(pending.Value(), 1ns).HasValue());
        CHECK(queue->Acquire(pending.Value()).ErrorValue().code.Value() == RenderQueryErrors::Cancelled.code.Value());
        REQUIRE(queue->Fail(pending.Value(), backendFailure).HasValue());
        REQUIRE(queue->Discard(pending.Value()).HasValue());

        REQUIRE(queue->Timeout(timedOut.Value()).HasValue());
        REQUIRE(queue->Timeout(timedOut.Value()).HasValue());
        CHECK(queue->Acquire(timedOut.Value()).ErrorValue().code.Value() == RenderQueryErrors::TimedOut.code.Value());
        REQUIRE(queue->Fail(timedOut.Value(), backendFailure).HasValue());
        REQUIRE(queue->Retire(timedOut.Value()).HasValue());
        REQUIRE(queue->Discard(timedOut.Value()).HasValue());

        const auto ready = queue->Request(QueryDescriptor());
        REQUIRE(ready.HasValue());
        REQUIRE(queue->MarkSubmitted(ready.Value(), Completion).HasValue());
        CHECK(queue->Complete(ready.Value(), -1ns).ErrorValue().code.Value() == RenderQueryErrors::TimestampInvalid.code.Value());
        REQUIRE(queue->Complete(ready.Value(), 1ns).HasValue());
        CHECK(queue->IsReady(ready.Value()).Value());
        CHECK(queue->Fail(ready.Value(), backendFailure).ErrorValue().code.Value() == RenderQueryErrors::InvalidTransition.code.Value());
        CHECK(queue->Cancel(ready.Value()).ErrorValue().code.Value() == RenderQueryErrors::InvalidTransition.code.Value());
        CHECK(queue->Timeout(ready.Value()).ErrorValue().code.Value() == RenderQueryErrors::InvalidTransition.code.Value());
        CHECK(queue->Retire(ready.Value()).ErrorValue().code.Value() == RenderQueryErrors::InvalidTransition.code.Value());
        CHECK(queue->Discard(ready.Value()).ErrorValue().code.Value() == RenderQueryErrors::InvalidTransition.code.Value());
        REQUIRE(queue->Acquire(ready.Value()).HasValue());
        CHECK(queue->Acquire(ready.Value()).ErrorValue().code.Value() == RenderQueryErrors::InvalidRequest.code.Value());

        queue->StopAdmission();
        CHECK(queue->Request(QueryDescriptor()).ErrorValue().code.Value() == RenderQueryErrors::Stopped.code.Value());
        queue->Shutdown();
        queue->Shutdown();
    }
}  // namespace RenderTransferTests
