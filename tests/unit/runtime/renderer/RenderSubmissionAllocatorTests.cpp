#include "Horo/Runtime/Render/RenderGraphWorkload.h"
#include "RenderSubmissionAllocator.h"

#include <catch2/catch_test_macros.hpp>
#include <limits>

using namespace Horo::Render;

TEST_CASE("Frontend submission aliases and imports share one monotonically issued history", "[renderer][submission]") {
    Detail::RenderSubmissionAllocator owner;
    const std::array assignments{RenderQueueAssignment{RenderQueueRole::Graphics, {1}},
                                 RenderQueueAssignment{RenderQueueRole::Transfer, {1}}};
    REQUIRE(owner.Configure(assignments).HasValue());
    const auto first = owner.Reserve(RenderQueueRole::Graphics, {});
    REQUIRE(first.HasValue());
    CHECK(first.Value().signal.value == 1);
    REQUIRE(owner.Admit(std::span{&first.Value(), 1}).HasValue());
    CHECK(owner.Admit(std::span{&first.Value(), 1}).HasError());
    const std::array waits{first.Value().signal};
    const auto second = owner.Reserve(RenderQueueRole::Transfer, waits);
    REQUIRE(second.HasValue());
    CHECK(second.Value().queue == first.Value().queue);
    CHECK(second.Value().signal.value == 2);
    REQUIRE(owner.Admit(std::span{&second.Value(), 1}).HasValue());
    owner.BeginFrame();
    const RenderQueueSubmission imported{{1}, {10}, {}, {{1}, 20}};
    REQUIRE(owner.Admit(std::span{&imported, 1}).HasValue());
    const auto next = owner.Reserve(RenderQueueRole::Graphics, {});
    REQUIRE(next.HasValue());
    CHECK(next.Value().order.value == 11);
    CHECK(next.Value().signal.value == 21);
}

TEST_CASE("Frontend submission abandonment burns identities and cannot become a producer", "[renderer][submission]") {
    Detail::RenderSubmissionAllocator owner;
    const std::array assignments{RenderQueueAssignment{RenderQueueRole::Graphics, {1}}};
    REQUIRE(owner.Configure(assignments).HasValue());
    const auto abandoned = owner.Reserve(RenderQueueRole::Graphics, {});
    REQUIRE(abandoned.HasValue());
    owner.BeginFrame();
    const auto next = owner.Reserve(RenderQueueRole::Graphics, {});
    REQUIRE(next.HasValue());
    REQUIRE(owner.Admit(std::span{&next.Value(), 1}).HasValue());
    const std::array waits{abandoned.Value().signal};
    CHECK(owner.Reserve(RenderQueueRole::Graphics, waits).HasError());
    CHECK(next.Value().signal.value == 2);
}

TEST_CASE("Frontend submission invalid imports leave both order and timeline unchanged", "[renderer][submission]") {
    Detail::RenderSubmissionAllocator owner;
    const std::array assignments{RenderQueueAssignment{RenderQueueRole::Graphics, {1}}};
    REQUIRE(owner.Configure(assignments).HasValue());
    const std::array invalid{RenderQueueSubmission{{1}, {10}, {}, {{1}, 10}}, RenderQueueSubmission{{1}, {11}, {}, {{1}, 9}}};
    CHECK(owner.Admit(invalid).HasError());
    const auto first = owner.Reserve(RenderQueueRole::Graphics, {});
    REQUIRE(first.HasValue());
    CHECK(first.Value().signal.value == 1);
    CHECK(first.Value().order.value == 1);
    auto forged = first.Value();
    forged.signal.value = 2;
    CHECK(owner.Admit(std::span{&forged, 1}).HasError());
    REQUIRE(owner.Admit(std::span{&first.Value(), 1}).HasValue());
}

TEST_CASE("Frontend submission capacity is finite and recovers at the next frame", "[renderer][submission]") {
    Detail::RenderSubmissionAllocator owner;
    const std::array assignments{RenderQueueAssignment{RenderQueueRole::Graphics, {1}}};
    REQUIRE(owner.Configure(assignments).HasValue());
    for (std::size_t index = 0; index < Detail::RenderSubmissionAllocator::MaximumReservations; ++index)
        REQUIRE(owner.Reserve(RenderQueueRole::Graphics, {}).HasValue());
    CHECK(owner.Reserve(RenderQueueRole::Graphics, {}).HasError());
    owner.BeginFrame();
    const auto next = owner.Reserve(RenderQueueRole::Graphics, {});
    REQUIRE(next.HasValue());
    CHECK(next.Value().signal.value == Detail::RenderSubmissionAllocator::MaximumReservations + 1);
    CHECK(owner.Reserve(RenderQueueRole::Compute, {}).HasError());
}

TEST_CASE("Frontend cross-queue reservations require admitted dependency order", "[renderer][submission]") {
    Detail::RenderSubmissionAllocator owner;
    const std::array assignments{RenderQueueAssignment{RenderQueueRole::Graphics, {1}},
                                 RenderQueueAssignment{RenderQueueRole::Transfer, {2}}};
    REQUIRE(owner.Configure(assignments).HasValue());
    const auto producer = owner.Reserve(RenderQueueRole::Transfer, {});
    REQUIRE(producer.HasValue());
    const std::array waits{producer.Value().signal};
    const auto consumer = owner.Reserve(RenderQueueRole::Graphics, waits);
    REQUIRE(consumer.HasValue());
    CHECK(owner.Admit(std::span{&consumer.Value(), 1}).HasError());
    const std::array valid{producer.Value(), consumer.Value()};
    REQUIRE(owner.Admit(valid).HasValue());
    const std::array duplicateWaits{producer.Value().signal, producer.Value().signal};
    CHECK(owner.Reserve(RenderQueueRole::Graphics, duplicateWaits).HasError());
}

TEST_CASE("Frontend order and timeline exhaustion fail before wraparound", "[renderer][submission]") {
    Detail::RenderSubmissionAllocator owner;
    const std::array assignments{RenderQueueAssignment{RenderQueueRole::Graphics, {1}}};
    REQUIRE(owner.Configure(assignments).HasValue());
    constexpr auto boundary = std::numeric_limits<std::uint64_t>::max() - 1;
    const RenderQueueSubmission last{{1}, {boundary}, {}, {{1}, boundary}};
    REQUIRE(owner.Admit(std::span{&last, 1}).HasValue());
    CHECK(owner.Reserve(RenderQueueRole::Graphics, {}).HasError());
    owner.BeginFrame();
    CHECK(owner.Reserve(RenderQueueRole::Graphics, {}).HasError());
}

TEST_CASE("Frontend graph imports preserve reservations across oversized and partially invalid batches", "[renderer][submission]") {
    Detail::RenderSubmissionAllocator owner;
    const std::array assignments{RenderQueueAssignment{RenderQueueRole::Graphics, {1}}};
    REQUIRE(owner.Configure(assignments).HasValue());
    const auto first = owner.Reserve(RenderQueueRole::Graphics, {});
    REQUIRE(first.HasValue());
    const RenderGraphQueueSubmission initial{first.Value(), {0, 1}};
    const std::array<RenderGraphQueueSubmission, Detail::RenderSubmissionAllocator::MaximumReservations + 1> oversized{};
    CHECK(owner.AdmitGraphSubmissions(oversized).HasError());
    CHECK(owner.AdmitGraphSubmissions({}).HasValue());
    REQUIRE(owner.AdmitGraphSubmissions(std::span{&initial, 1}).HasValue());
    const auto next = owner.Reserve(RenderQueueRole::Graphics, {});
    REQUIRE(next.HasValue());
    CHECK(next.Value().signal.value == 2);
    const RenderGraphQueueSubmission valid{next.Value(), {0, 1}};
    const std::array partiallyInvalid{valid, RenderGraphQueueSubmission{}};
    CHECK(owner.AdmitGraphSubmissions(partiallyInvalid).HasError());
    CHECK(owner.AdmitGraphSubmissions(std::span{&valid, 1}).HasValue());
}
