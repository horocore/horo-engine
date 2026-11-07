#include "Horo/Runtime/Render/RenderReadback.h"
#include "Horo/Runtime/Render/RenderReadbackErrors.h"
#include "support/AllocationProbe.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstddef>
#include <memory>
#include <thread>
#include <utility>

namespace RenderReadbackTests {
    using namespace Horo;
    using namespace Horo::Render;
    using namespace std::chrono_literals;

    constexpr RenderResourceOwnerId Renderer{71};
    constexpr RenderResourceOwnerId OtherRenderer{72};
    constexpr RenderBufferHandle Source{Renderer, 2, 3};
    constexpr RenderTimelinePoint Completion{{1}, 9};

    [[nodiscard]] RenderReadbackLimits Limits() {
        return {.maximumPendingBytes = 16,
                .maximumRetainedResultBytes = 16,
                .maximumRequestBytes = 8,
                .maximumAlignment = 8,
                .maximumRequests = 3};
    }

    [[nodiscard]] RenderReadbackDescriptor Descriptor(const std::size_t bytes = 4) {
        return {.source = Source, .sourceByteOffset = 2, .byteCount = bytes, .alignment = 4, .timeout = 5ms};
    }

    [[nodiscard]] std::unique_ptr<RenderReadbackQueue> CreateQueue(const RenderReadbackLimits limits = Limits()) {
        auto created = RenderReadbackQueue::Create(Renderer, limits);
        REQUIRE(created.HasValue());
        return std::move(created).Value();
    }

    [[nodiscard]] RenderReadbackId Submit(RenderReadbackQueue &queue, const std::size_t bytes = 4) {
        auto requested = queue.Request(Descriptor(bytes));
        REQUIRE(requested.HasValue());
        REQUIRE(queue.MarkSubmitted(requested.Value(), Completion).HasValue());
        return requested.Value();
    }

    TEST_CASE("Render readback validates finite request and queue bounds", "[unit][runtime][renderer][readback]") {
        CHECK(Limits().IsValid());
        CHECK_FALSE(RenderReadbackLimits{.maximumPendingBytes = 0}.IsValid());
        CHECK_FALSE(RenderReadbackLimits{.maximumPendingBytes = 4, .maximumRequestBytes = 5}.IsValid());
        CHECK_FALSE(RenderReadbackLimits{.maximumAlignment = 3}.IsValid());
        CHECK_FALSE(RenderReadbackLimits{.maximumRequests = 0}.IsValid());
        CHECK(Descriptor().IsValid());

        RenderReadbackDescriptor invalidSource = Descriptor();
        invalidSource.source = RenderTextureHandle{};
        CHECK_FALSE(invalidSource.IsValid());
        RenderReadbackDescriptor invalidTimeout = Descriptor();
        invalidTimeout.timeout = 0ns;
        CHECK_FALSE(invalidTimeout.IsValid());

        const auto invalidOwner = RenderReadbackQueue::Create({}, Limits());
        REQUIRE(invalidOwner.HasError());
        CHECK(invalidOwner.ErrorValue().code.Value() == RenderReadbackErrors::InvalidConfiguration.code.Value());

        auto queue = CreateQueue();
        const auto oversized = queue->Request(Descriptor(9));
        REQUIRE(oversized.HasError());
        CHECK(oversized.ErrorValue().code.Value() == RenderReadbackErrors::InvalidDescriptor.code.Value());
        RenderReadbackDescriptor foreignSource = Descriptor();
        foreignSource.source = RenderBufferHandle{OtherRenderer, 1, 1};
        CHECK(queue->Request(foreignSource).ErrorValue().code.Value() == RenderReadbackErrors::InvalidDescriptor.code.Value());
    }

    TEST_CASE("Render readback completion publishes immutable consumer-owned bytes", "[unit][runtime][renderer][readback]") {
        auto queue = CreateQueue();
        const RenderReadbackId request = Submit(*queue);
        CHECK(queue->State(request).Value() == RenderReadbackState::Submitted);
        CHECK(queue->Snapshot().pendingBytes == 4);
        CHECK(queue->Acquire(request).ErrorValue().code.Value() == RenderReadbackErrors::ResultPending.code.Value());

        std::array mapped{std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}};
        REQUIRE(queue->Complete(request, mapped).HasValue());
        mapped[0] = std::byte{9};
        CHECK(queue->Snapshot().pendingBytes == 0);
        CHECK(queue->Snapshot().retainedResultBytes == 4);
        CHECK(queue->State(request).Value() == RenderReadbackState::Ready);
        const Error readyFailure{ErrorCode{"render.test.readback_ready_failure"},
                                 ErrorDomainId{"render.test"},
                                 ErrorSeverity::Error,
                                 "Injected ready-state failure.",
                                 {}};
        CHECK(queue->Fail(request, readyFailure).ErrorValue().code.Value() == RenderReadbackErrors::InvalidTransition.code.Value());
        CHECK(queue->Cancel(request).ErrorValue().code.Value() == RenderReadbackErrors::InvalidTransition.code.Value());
        CHECK(queue->Timeout(request).ErrorValue().code.Value() == RenderReadbackErrors::InvalidTransition.code.Value());
        CHECK(queue->Retire(request).ErrorValue().code.Value() == RenderReadbackErrors::InvalidTransition.code.Value());

        auto acquired = queue->Acquire(request);
        REQUIRE(acquired.HasValue());
        RenderReadbackResult result = std::move(acquired).Value();
        CHECK(result.Id() == request);
        CHECK(result.Completion() == Completion);
        REQUIRE(result.Bytes().size() == 4);
        CHECK(result.Bytes().front() == std::byte{1});
        CHECK(queue->Snapshot().requestCount == 0);
        CHECK(queue->Snapshot().retainedResultBytes == 4);

        const auto deniedWhileLeased = queue->Request(Descriptor(8));
        REQUIRE(deniedWhileLeased.HasValue());
        const auto capacityDenied = queue->Request(Descriptor(8));
        REQUIRE(capacityDenied.HasError());
        CHECK(capacityDenied.ErrorValue().code.Value() == RenderReadbackErrors::CapacityExceeded.code.Value());
        CHECK(queue->Snapshot().failedAdmissionCount == 1);
    }

    TEST_CASE("Render readback cancellation and timeout retain submitted staging until retirement", "[unit][runtime][renderer][readback]") {
        auto queue = CreateQueue();
        auto pending = queue->Request(Descriptor());
        REQUIRE(pending.HasValue());
        REQUIRE(queue->Cancel(pending.Value()).HasValue());
        CHECK(queue->Snapshot().pendingBytes == 0);
        CHECK(queue->Acquire(pending.Value()).ErrorValue().code.Value() == RenderReadbackErrors::Cancelled.code.Value());
        REQUIRE(queue->Discard(pending.Value()).HasValue());

        const auto pendingTimeout = queue->Request(Descriptor());
        REQUIRE(pendingTimeout.HasValue());
        REQUIRE(queue->Timeout(pendingTimeout.Value()).HasValue());
        CHECK(queue->Acquire(pendingTimeout.Value()).ErrorValue().code.Value() == RenderReadbackErrors::TimedOut.code.Value());
        REQUIRE(queue->Discard(pendingTimeout.Value()).HasValue());

        const RenderReadbackId submitted = Submit(*queue);
        REQUIRE(queue->Cancel(submitted).HasValue());
        CHECK(queue->Snapshot().pendingBytes == 4);
        CHECK(queue->Discard(submitted).ErrorValue().code.Value() == RenderReadbackErrors::InvalidTransition.code.Value());
        REQUIRE(queue->Retire(submitted).HasValue());
        CHECK(queue->Snapshot().pendingBytes == 0);
        REQUIRE(queue->Discard(submitted).HasValue());

        const RenderReadbackId timedOut = Submit(*queue);
        REQUIRE(queue->Timeout(timedOut).HasValue());
        CHECK(queue->Snapshot().pendingBytes == 4);
        std::array mapped{std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}};
        REQUIRE(queue->Complete(timedOut, mapped).HasValue());
        CHECK(queue->Snapshot().pendingBytes == 0);
        CHECK(queue->Snapshot().retainedResultBytes == 0);
        CHECK(queue->Acquire(timedOut).ErrorValue().code.Value() == RenderReadbackErrors::TimedOut.code.Value());
        REQUIRE(queue->Discard(timedOut).HasValue());
    }

    TEST_CASE("Render readback preserves backend failures and rejects invalid lifecycle transitions",
              "[unit][runtime][renderer][readback]") {
        auto queue = CreateQueue();
        auto pending = queue->Request(Descriptor());
        REQUIRE(pending.HasValue());
        CHECK(queue->MarkSubmitted(pending.Value(), {}).ErrorValue().code.Value() == RenderReadbackErrors::InvalidTransition.code.Value());

        const Error backendFailure{ErrorCode{"render.test.map_failed"},
                                   ErrorDomainId{"render.test"},
                                   ErrorSeverity::Error,
                                   "Injected mapping failure.",
                                   {}};
        REQUIRE(queue->Fail(pending.Value(), backendFailure).HasValue());
        auto failed = queue->Acquire(pending.Value());
        REQUIRE(failed.HasError());
        CHECK(failed.ErrorValue().code.Value() == backendFailure.code.Value());
        REQUIRE(queue->Discard(pending.Value()).HasValue());

        const auto foreign = queue->State(RenderReadbackId{OtherRenderer, 1});
        REQUIRE(foreign.HasError());
        CHECK(foreign.ErrorValue().code.Value() == RenderReadbackErrors::InvalidRequest.code.Value());

        queue->StopAdmission();
        CHECK_FALSE(queue->Snapshot().acceptingRequests);
        CHECK(queue->Request(Descriptor()).ErrorValue().code.Value() == RenderReadbackErrors::Stopped.code.Value());
    }

    TEST_CASE("Render readback enforces request capacity and owner-thread affinity", "[unit][runtime][renderer][readback]") {
        auto queue = CreateQueue({.maximumPendingBytes = 8,
                                  .maximumRetainedResultBytes = 8,
                                  .maximumRequestBytes = 8,
                                  .maximumAlignment = 8,
                                  .maximumRequests = 1});
        const auto request = queue->Request(Descriptor());
        REQUIRE(request.HasValue());
        const auto denied = queue->Request(Descriptor());
        REQUIRE(denied.HasError());
        CHECK(denied.ErrorValue().code.Value() == RenderReadbackErrors::CapacityExceeded.code.Value());
        CHECK(queue->Snapshot().failedAdmissionCount == 1);

        auto threadRequest = Result<RenderReadbackId>::Success({});
        auto threadMark = Result<void>::Success();
        auto threadResult = Result<RenderReadbackState>::Success(RenderReadbackState::Ready);
        std::thread worker([&] {
            threadRequest = queue->Request(Descriptor());
            threadMark = queue->MarkSubmitted(request.Value(), Completion);
            threadResult = queue->State(request.Value());
        });
        worker.join();
        REQUIRE(threadRequest.HasError());
        CHECK(threadRequest.ErrorValue().code.Value() == RenderReadbackErrors::WrongThread.code.Value());
        REQUIRE(threadMark.HasError());
        CHECK(threadMark.ErrorValue().code.Value() == RenderReadbackErrors::WrongThread.code.Value());
        REQUIRE(threadResult.HasError());
        CHECK(threadResult.ErrorValue().code.Value() == RenderReadbackErrors::WrongThread.code.Value());
    }

    TEST_CASE("Render readback rejects mismatched mappings before publishing a result", "[unit][runtime][renderer][readback]") {
        auto queue = CreateQueue();
        const RenderReadbackId request = Submit(*queue);
        const std::array wrongSize{std::byte{1}, std::byte{2}};
        CHECK(queue->Complete(request, wrongSize).ErrorValue().code.Value() == RenderReadbackErrors::MappingSizeMismatch.code.Value());
        CHECK(queue->State(request).Value() == RenderReadbackState::Submitted);

        const std::array mapped{std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}};
        REQUIRE(queue->Complete(request, mapped).HasValue());
        CHECK(queue->Acquire(request).HasValue());
    }

    TEST_CASE("Render readback handles repeated cancellation and timeout retirement", "[unit][runtime][renderer][readback]") {
        auto queue = CreateQueue();
        const Error backendFailure{ErrorCode{"render.test.readback_failed_again"},
                                   ErrorDomainId{"render.test"},
                                   ErrorSeverity::Error,
                                   "Injected readback failure.",
                                   {}};

        const auto cancelled = queue->Request(Descriptor());
        REQUIRE(cancelled.HasValue());
        CHECK(queue->Complete(cancelled.Value(), {}).ErrorValue().code.Value() == RenderReadbackErrors::InvalidTransition.code.Value());
        REQUIRE(queue->Cancel(cancelled.Value()).HasValue());
        REQUIRE(queue->Cancel(cancelled.Value()).HasValue());
        REQUIRE(queue->Complete(cancelled.Value(), {}).HasValue());
        REQUIRE(queue->Fail(cancelled.Value(), backendFailure).HasValue());
        REQUIRE(queue->Discard(cancelled.Value()).HasValue());

        const auto timedOut = Submit(*queue);
        REQUIRE(queue->Timeout(timedOut).HasValue());
        REQUIRE(queue->Timeout(timedOut).HasValue());
        REQUIRE(queue->Fail(timedOut, backendFailure).HasValue());
        REQUIRE(queue->Retire(timedOut).HasValue());
        REQUIRE(queue->Discard(timedOut).HasValue());
    }

    TEST_CASE("Render readback shutdown stops admission while acquired results remain owned", "[unit][runtime][renderer][readback]") {
        auto queue = CreateQueue();
        const RenderReadbackId request = Submit(*queue);
        const std::array mapped{std::byte{4}, std::byte{3}, std::byte{2}, std::byte{1}};
        REQUIRE(queue->Complete(request, mapped).HasValue());
        auto acquired = queue->Acquire(request);
        REQUIRE(acquired.HasValue());
        RenderReadbackResult result = std::move(acquired).Value();

        queue->Shutdown();
        queue->Shutdown();
        CHECK_FALSE(queue->Snapshot().acceptingRequests);
        CHECK(queue->Request(Descriptor()).ErrorValue().code.Value() == RenderReadbackErrors::Stopped.code.Value());
        queue.reset();
        REQUIRE(result.Bytes().size() == mapped.size());
        CHECK(result.Bytes().back() == std::byte{1});
    }

    /** @brief Prepares actual queue-owned CPU storage only; timeline metadata here is not a native execution claim. */
    std::unique_ptr<RenderReadbackQueue> PreparedQueue(std::uint32_t slots = 2) {
        auto queue = CreateQueue();
        const auto prepared = queue->PreparePayloadStorage({slots, 8, 8}, 1024U * 1024U);
        REQUIRE(prepared.HasValue());
        CHECK(prepared.Value() == queue->RetainedPayloadBytes());
        return queue;
    }

    RenderReadbackId PreparedSubmitted(RenderReadbackQueue &queue) {
        const auto request = queue.RequestPrepared(Descriptor());
        REQUIRE(request.status == RenderReadbackPreparedStatus::Complete);
        REQUIRE(queue.MarkSubmitted(request.id, Completion).HasValue());
        return request.id;
    }

    TEST_CASE("Prepared readback charges actual cold storage and rejects impossible pool configuration", "[readback][prepared]") {
        auto queue = CreateQueue();
        const auto base = queue->RetainedPayloadBytes();
        CHECK(queue->PreparePayloadStorage({2, 8, 8}, base).HasError());
        CHECK(queue->RetainedPayloadBytes() == base);
        CHECK(queue->PreparePayloadStorage({0, 8, 8}, 1024U * 1024U).HasError());
        CHECK(queue->PreparePayloadStorage({2, 8, 3}, 1024U * 1024U).HasError());
        const auto prepared = queue->PreparePayloadStorage({2, 8, 8}, 1024U * 1024U);
        REQUIRE(prepared.HasValue());
        CHECK(prepared.Value() > base + 16);
        CHECK(prepared.Value() == queue->RetainedPayloadBytes());
        CHECK(queue->PreparePayloadStorage({2, 8, 8}, 1024U * 1024U).HasError());
    }

    TEST_CASE("Prepared readback completion acquires the same aligned bytes without hot allocation or copy", "[readback][prepared]") {
        auto queue = PreparedQueue();
        RenderReadbackResult lease;
        RenderReadbackPreparedStatus completion{}, acquisition{}, pressure{};
        RenderReadbackPreparedRequest request;
        std::uintptr_t address{};
        std::size_t allocations{};
        {
            Horo::Tests::AllocationProbe::ScopedMeasurement measurement;
            request = queue->RequestPrepared(Descriptor());
            (void)queue->MarkSubmitted(request.id, Completion);
            auto writable = queue->WritablePrepared(request.id);
            address = reinterpret_cast<std::uintptr_t>(writable.bytes.data());
            if (writable.bytes.size() == 4)
                writable.bytes[0] = std::byte{0x5a};
            completion = queue->CompletePrepared(request.id, Completion);
            acquisition = queue->AcquirePrepared(request.id, lease);
            const auto second = queue->RequestPrepared(Descriptor());
            pressure = queue->RequestPrepared(Descriptor()).status;
            (void)queue->Cancel(second.id);
            (void)queue->Discard(second.id);
            allocations = measurement.Snapshot().requests;
        }
        REQUIRE(request.status == RenderReadbackPreparedStatus::Complete);
        CHECK(completion == RenderReadbackPreparedStatus::Complete);
        CHECK(acquisition == RenderReadbackPreparedStatus::Complete);
        CHECK(pressure == RenderReadbackPreparedStatus::CapacityExceeded);
        CHECK(allocations == 0);
        REQUIRE(lease.Bytes().size() == 4);
        CHECK(reinterpret_cast<std::uintptr_t>(lease.Bytes().data()) == address);
        CHECK(address % 8 == 0);
        CHECK(lease.Bytes()[0] == std::byte{0x5a});
        CHECK(lease.Id() == request.id);
        CHECK(lease.Completion() == Completion);
    }

    TEST_CASE("Prepared readback retains submitted cancellation and timeout charges until exact retirement", "[readback][prepared]") {
        auto queue = PreparedQueue(1);
        const auto id = PreparedSubmitted(*queue);
        SECTION("cancel") {
            REQUIRE(queue->Cancel(id).HasValue());
        }
        SECTION("timeout") {
            REQUIRE(queue->Timeout(id).HasValue());
        }
        CHECK(queue->WritablePrepared(id).status == RenderReadbackPreparedStatus::InvalidTransition);
        CHECK(queue->Snapshot().pendingBytes == 4);
        CHECK(queue->RequestPrepared(Descriptor()).status == RenderReadbackPreparedStatus::CapacityExceeded);
        CHECK(queue->CompletePrepared(id, {{1}, 10}) == RenderReadbackPreparedStatus::InvalidTransition);
        CHECK(queue->Snapshot().pendingBytes == 4);
        REQUIRE(queue->Retire(id).HasValue());
        REQUIRE(queue->Discard(id).HasValue());
        CHECK(queue->CollectPreparedResults() == 1);
        const auto next = queue->RequestPrepared(Descriptor());
        CHECK(next.status == RenderReadbackPreparedStatus::Complete);
        CHECK(next.id != id);
        CHECK(queue->WritablePrepared(id).status == RenderReadbackPreparedStatus::InvalidRequest);
        CHECK(queue->CompletePrepared(id, Completion) == RenderReadbackPreparedStatus::InvalidRequest);
    }

    TEST_CASE("Prepared readback never exposes pending ready or stale requests as writable", "[readback][prepared]") {
        auto queue = PreparedQueue();
        const auto pending = queue->RequestPrepared(Descriptor());
        REQUIRE(pending.status == RenderReadbackPreparedStatus::Complete);
        CHECK(queue->WritablePrepared(pending.id).status == RenderReadbackPreparedStatus::InvalidTransition);
        CHECK(queue->CompletePrepared(pending.id, Completion) == RenderReadbackPreparedStatus::InvalidTransition);
        RenderReadbackResult output;
        CHECK(queue->AcquirePrepared(pending.id, output) == RenderReadbackPreparedStatus::ResultPending);
        REQUIRE(queue->MarkSubmitted(pending.id, Completion).HasValue());
        CHECK(queue->CompletePrepared(pending.id, {{2}, 9}) == RenderReadbackPreparedStatus::InvalidTransition);
        CHECK(queue->CompletePrepared(pending.id, Completion) == RenderReadbackPreparedStatus::Complete);
        CHECK(queue->WritablePrepared(pending.id).status == RenderReadbackPreparedStatus::InvalidTransition);
        CHECK(queue->CompletePrepared(pending.id, Completion) == RenderReadbackPreparedStatus::InvalidTransition);
        CHECK(queue->AcquirePrepared(pending.id, output) == RenderReadbackPreparedStatus::Complete);
        const auto retained = output.Id();
        CHECK(queue->AcquirePrepared(pending.id, output) == RenderReadbackPreparedStatus::InvalidTransition);
        CHECK(output.Id() == retained);
        auto foreign = Descriptor();
        foreign.source = RenderBufferHandle{OtherRenderer, 1, 1};
        CHECK(queue->RequestPrepared(foreign).status == RenderReadbackPreparedStatus::InvalidDescriptor);
    }

    TEST_CASE("Prepared readback lease prevents reuse until consumer release and survives queue destruction", "[readback][prepared]") {
        auto queue = PreparedQueue(1);
        const auto id = PreparedSubmitted(*queue);
        queue->WritablePrepared(id).bytes[0] = std::byte{7};
        REQUIRE(queue->CompletePrepared(id, Completion) == RenderReadbackPreparedStatus::Complete);
        RenderReadbackResult lease;
        REQUIRE(queue->AcquirePrepared(id, lease) == RenderReadbackPreparedStatus::Complete);
        CHECK(queue->CollectPreparedResults() == 0);
        CHECK(queue->RetirePreparedResult(id) == RenderReadbackPreparedStatus::ResultPending);
        CHECK(queue->RetirePreparedResult({OtherRenderer, id.value}) == RenderReadbackPreparedStatus::InvalidRequest);
        CHECK(queue->RetirePreparedResult({Renderer, id.value + 999}) == RenderReadbackPreparedStatus::InvalidRequest);
        CHECK(queue->Snapshot().requestCount == 1);
        CHECK(queue->Snapshot().readyCount == 1);
        CHECK(queue->RequestPrepared(Descriptor()).status == RenderReadbackPreparedStatus::CapacityExceeded);
        SECTION("consumer release then owner reuse") {
            std::size_t allocations{};
            std::thread consumer([owned = std::move(lease), &allocations]() mutable {
                Horo::Tests::AllocationProbe::ScopedMeasurement measurement;
                owned = RenderReadbackResult{};
                allocations = measurement.Snapshot().requests;
            });
            consumer.join();
            CHECK(allocations == 0);
            CHECK(queue->CollectPreparedResults() == 0);
            RenderReadbackPreparedStatus retired{};
            {
                Horo::Tests::AllocationProbe::ScopedMeasurement measurement;
                retired = queue->RetirePreparedResult(id);
                allocations = measurement.Snapshot().requests;
            }
            CHECK(retired == RenderReadbackPreparedStatus::Complete);
            CHECK(allocations == 0);
            CHECK(queue->RetirePreparedResult(id) == RenderReadbackPreparedStatus::InvalidRequest);
            CHECK(queue->Snapshot().retainedResultBytes == 0);
            CHECK(queue->RequestPrepared(Descriptor()).status == RenderReadbackPreparedStatus::Complete);
        }
        SECTION("queue dies first") {
            queue.reset();
            CHECK(lease.Bytes()[0] == std::byte{7});
            CHECK(lease.Id() == id);
            std::thread consumer([owned = std::move(lease)]() mutable {
                owned = RenderReadbackResult{};
            });
            consumer.join();
        }
    }

    TEST_CASE("Prepared readback shares legacy accounting and identity without owner-thread escape", "[readback][prepared]") {
        auto queue = PreparedQueue();
        auto legacy = queue->Request(Descriptor());
        REQUIRE(legacy.HasValue());
        const auto prepared = queue->RequestPrepared(Descriptor());
        REQUIRE(prepared.status == RenderReadbackPreparedStatus::Complete);
        CHECK(prepared.id.value > legacy.Value().value);
        CHECK(queue->WritablePrepared(legacy.Value()).status == RenderReadbackPreparedStatus::InvalidRequest);
        RenderReadbackPreparedStatus foreign{};
        std::thread worker([&] {
            foreign = queue->RequestPrepared(Descriptor()).status;
        });
        worker.join();
        CHECK(foreign == RenderReadbackPreparedStatus::WrongThread);
        REQUIRE(queue->MarkSubmitted(prepared.id, Completion).HasValue());
        CHECK(queue->Complete(prepared.id, std::array<std::byte, 4>{}).HasError());
        CHECK(queue->CompletePrepared(prepared.id, Completion) == RenderReadbackPreparedStatus::Complete);
        auto acquired = queue->Acquire(prepared.id);
        REQUIRE(acquired.HasValue());
        CHECK(acquired.Value().Id() == prepared.id);
        CHECK(queue->Acquire(prepared.id).HasError());
        RenderReadbackResult secondLease;
        CHECK(queue->AcquirePrepared(prepared.id, secondLease) == RenderReadbackPreparedStatus::InvalidTransition);
        CHECK(queue->RetirePreparedResult(prepared.id) == RenderReadbackPreparedStatus::ResultPending);
        queue->StopAdmission();
        CHECK(queue->RequestPrepared(Descriptor()).status == RenderReadbackPreparedStatus::Stopped);
    }
}  // namespace RenderReadbackTests
