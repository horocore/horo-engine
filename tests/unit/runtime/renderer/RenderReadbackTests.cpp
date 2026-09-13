#include "Horo/Runtime/Render/RenderReadback.h"
#include "Horo/Runtime/Render/RenderReadbackErrors.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstddef>
#include <memory>
#include <utility>

namespace {
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
}  // namespace
