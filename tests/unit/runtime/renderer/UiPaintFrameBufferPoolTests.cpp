#include "Horo/Runtime/Render/UiPaintFrameBufferPool.h"
#include "RenderFrontendTestSupport.h"

namespace {
    using namespace Horo;
    using namespace Horo::Render;
    using namespace Horo::Tests::RenderFrontendTesting;
    constexpr UiPaintFrameBudget Budget{3, 3, 1};

    struct Candidate final {
        std::array<UiPaintVertex, 3> vertices{};
        std::array<std::uint32_t, 3> indices{0, 1, 2};
        std::array<UiPaintParameters, 1> parameters{UiPaintParameters{{640, 480}}};
    };

    std::unique_ptr<RenderFrontend> Frontend() {
        lifecycleState = {};
        lifecycleState.uiPaint.enabled = true;
        return CreateTrackingFrontend();
    }
}  // namespace

TEST_CASE("UI resident pool backpressure and cancellation retain slot identity and revision", "[renderer][ui-paint][pool]") {
    auto frontend = Frontend();
    auto created = UiPaintFrameBufferPool::Create(*frontend, Budget, 2);
    REQUIRE(created.HasValue());
    auto pool = std::move(created).Value();
    Candidate candidate;
    CHECK(pool->Update(0, candidate.vertices, candidate.indices, candidate.parameters).HasError());
    REQUIRE(frontend->ProcessResourceRequests().HasValue());
    CHECK(lifecycleState.createBufferCount == 6);
    auto first = pool->Update(0, candidate.vertices, candidate.indices, candidate.parameters);
    REQUIRE(first.HasValue());
    CHECK(first.Value().revision == 1);
    const auto identities = lifecycleState.uiPaint.instances;
    lifecycleState.uiPaint.pending = true;
    const auto pending = pool->Update(0, candidate.vertices, candidate.indices, candidate.parameters);
    REQUIRE(pending.HasError());
    CHECK(pending.ErrorValue().code.Value() == UiPaintFrameErrors::Pending.code.Value());
    lifecycleState.uiPaint.pending = false;
    CancellationSource cancellation;
    cancellation.RequestCancellation();
    const auto cancelled = pool->Update(0, candidate.vertices, candidate.indices, candidate.parameters, cancellation.Token());
    REQUIRE(cancelled.HasError());
    CHECK(cancelled.ErrorValue().code.Value() == UiPaintFrameErrors::Cancelled.code.Value());
    CHECK(lifecycleState.uiPaint.updates == 1);
    CHECK(lifecycleState.uiPaint.revision == 1);
    const auto second = pool->Update(0, candidate.vertices, candidate.indices, candidate.parameters);
    REQUIRE(second.HasValue());
    CHECK(second.Value().revision == 2);
    CHECK(lifecycleState.uiPaint.instances == identities);
    CHECK(lifecycleState.createBufferCount == 6);
    REQUIRE(pool->Shutdown().HasValue());
    CHECK(lifecycleState.destroyBufferCount == 6);
}

TEST_CASE("UI resident pool revocation rejects update after frontend destruction", "[renderer][ui-paint][pool]") {
    auto frontend = Frontend();
    auto created = UiPaintFrameBufferPool::Create(*frontend, Budget, 1);
    REQUIRE(created.HasValue());
    auto pool = std::move(created).Value();
    REQUIRE(frontend->ProcessResourceRequests().HasValue());
    CHECK(lifecycleState.createBufferCount == 3);
    CHECK(lifecycleState.shutdownCount == 0);
    frontend.reset();
    // Canonical frontend shutdown transfers whole-native cleanup to the backend, not per-handle release callbacks.
    CHECK(lifecycleState.shutdownCount == 1);
    CHECK(lifecycleState.destroyBufferCount == 0);
    Candidate candidate;
    const auto rejected = pool->Update(0, candidate.vertices, candidate.indices, candidate.parameters);
    REQUIRE(rejected.HasError());
    CHECK(rejected.ErrorValue().code.Value() == UiPaintFrameErrors::Unavailable.code.Value());
    REQUIRE(pool->Shutdown().HasValue());
    pool.reset();
    CHECK(lifecycleState.shutdownCount == 1);
    CHECK(lifecycleState.destroyBufferCount == 0);
}

TEST_CASE("UI resident pool partial creation returns canonical reservations for retry", "[renderer][ui-paint][pool]") {
    lifecycleState = {};
    lifecycleState.uiPaint.enabled = true;
    auto frontend = CreateTrackingFrontend({}, {.maximumRequestsPerDrain = 2, .maximumPendingRequests = 2});
    for (int attempt = 0; attempt < 2; ++attempt) {
        REQUIRE(UiPaintFrameBufferPool::Create(*frontend, Budget, 1).HasError());
        CHECK(frontend->UploadSnapshot().pendingRequests == 0);
        CHECK(frontend->MemorySnapshot().reservationCount == 0);
        CHECK(lifecycleState.createBufferCount == 0);
    }
}

TEST_CASE("UI resident pool rejects foreign thread before reading revocation state", "[renderer][ui-paint][pool]") {
    auto frontend = Frontend();
    REQUIRE(UiPaintFrameBufferPool::Create(*frontend, Budget, 3).HasError());
    auto created = UiPaintFrameBufferPool::Create(*frontend, Budget, 1);
    REQUIRE(created.HasValue());
    auto pool = std::move(created).Value();
    Candidate candidate;
    bool rejected{};
    std::thread foreign([&] {
        rejected = pool->Shutdown().HasError() && pool->Update(0, candidate.vertices, candidate.indices, candidate.parameters).HasError();
    });
    foreign.join();
    CHECK(rejected);
    REQUIRE(frontend->ProcessResourceRequests().HasValue());
    REQUIRE(pool->Update(0, candidate.vertices, candidate.indices, candidate.parameters).HasValue());
    REQUIRE(pool->Shutdown().HasValue());
}

TEST_CASE("UI resident pool frame-close failure retains handles for owner retry", "[renderer][ui-paint][pool]") {
    auto frontend = Frontend();
    auto created = UiPaintFrameBufferPool::Create(*frontend, Budget, 1);
    REQUIRE(created.HasValue());
    auto pool = std::move(created).Value();
    REQUIRE(frontend->ProcessResourceRequests().HasValue());
    {
        auto frame = frontend->BeginFrame({.frameNumber = 1, .outputExtent = {640, 480}});
        REQUIRE(frame.HasValue());
        REQUIRE(pool->Shutdown().HasError());
        CHECK(lifecycleState.destroyBufferCount == 0);
    }
    Candidate candidate;
    CHECK(pool->Update(0, candidate.vertices, candidate.indices, candidate.parameters).HasError());
    REQUIRE(pool->Shutdown().HasValue());
    CHECK(lifecycleState.destroyBufferCount == 3);
}

TEST_CASE("UI resident pool unsupported admission creates no residents", "[renderer][ui-paint][pool]") {
    lifecycleState = {};
    auto frontend = CreateTrackingFrontend();
    const auto rejected = UiPaintFrameBufferPool::Create(*frontend, Budget, 1);
    REQUIRE(rejected.HasError());
    CHECK(rejected.ErrorValue().code.Value() == UiPaintFrameErrors::Unavailable.code.Value());
    CHECK(lifecycleState.createBufferCount == 0);
    CHECK(frontend->UploadSnapshot().pendingRequests == 0);
    CHECK(frontend->MemorySnapshot().reservationCount == 0);
}
