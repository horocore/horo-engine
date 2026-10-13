#include "Horo/Runtime/Render/RenderGraphExecutionErrors.h"
#include "VfxRenderTestSupport.h"
#include "support/AllocationProbe.h"

#include <catch2/generators/catch_generators.hpp>
#include <utility>

using namespace Horo;
using namespace Horo::Render;
using namespace Horo::Vfx;
using namespace Test::VfxRender;

TEST_CASE("Prepared per-view scheduling and draw rejection allocate no frame-hot storage", "[vfx][graph][allocation]") {
    const std::array bindings{Binding(0, VfxParticlePass::Translucent), Binding(1)};
    auto world = World(bindings);
    for (const auto &binding : bindings)
        Populate(*world, binding);
    const auto snapshot = world->Extract(1, {}, 1).snapshot;
    auto frontend = Frontend();
    const RenderFrontend &scheduler = *frontend;
    for (std::uint64_t view = 1; view <= 128; ++view) {
        auto builder = Builder();
        auto request = Request(*frontend, builder);
        request.view = view;
        VfxGraphSchedulingOutcome scheduled;
        std::size_t requests{};
        {
            Tests::AllocationProbe::ScopedMeasurement measurement;
            scheduled = scheduler.ScheduleVfx(builder, snapshot, request);
            requests = measurement.Snapshot().requests;
        }
        REQUIRE(scheduled.HasValue());
        CHECK(requests == 0);
        CHECK(scheduled.Value().drawCalls == 2);
    }
    auto rejectedBuilder = Builder();
    auto request = Request(*frontend, rejectedBuilder);
    request.remainingDrawCalls = 1;
    VfxGraphSchedulingOutcome rejected;
    std::size_t requests{};
    {
        Tests::AllocationProbe::ScopedMeasurement measurement;
        rejected = scheduler.ScheduleVfx(rejectedBuilder, snapshot, request);
        requests = measurement.Snapshot().requests;
    }
    CHECK(requests == 0);
    CHECK(rejected.FailureDescriptor() == &RenderGraphExecutionErrors::VfxDrawBudgetExceeded);
    const auto graph = std::move(rejectedBuilder).Finalize();
    REQUIRE(graph.HasValue());
    CHECK(graph.Value().Passes().size() == 3);
}

TEST_CASE("Prepared view leases enforce aggregate backpressure and survive frontend destruction", "[vfx][graph][lifetime]") {
    const std::array bindings{Binding(0, VfxParticlePass::Translucent)};
    auto world = World(bindings);
    Populate(*world, bindings[0]);
    const auto snapshot = world->Extract(1, {}, 1).snapshot;
    auto frontend = Frontend();
    REQUIRE(frontend->PrepareVfxScheduling({64, 2, 1'000'000}).HasValue());
    auto firstBuilder = Builder();
    auto firstRequest = Request(*frontend, firstBuilder);
    auto first = frontend->ScheduleVfx(firstBuilder, snapshot, firstRequest);
    REQUIRE(first.HasValue());
    auto secondBuilder = Builder();
    auto secondRequest = Request(*frontend, secondBuilder);
    secondRequest.view = 2;
    secondRequest.forward = {0, 0, 1};
    auto second = frontend->ScheduleVfx(secondBuilder, snapshot, secondRequest);
    REQUIRE(second.HasValue());
    const auto &firstOrder = *std::get<RenderGraphVfx>(first.Value().Workloads()[0].workload).order;
    const auto &secondOrder = *std::get<RenderGraphVfx>(second.Value().Workloads()[0].workload).order;
    REQUIRE(firstOrder.instances.size() == 2);
    CHECK(firstOrder.instances[0].instance == secondOrder.instances[1].instance);
    CHECK(firstOrder.view == 1);
    CHECK(secondOrder.view == 2);
    auto copy = first;
    first = {};
    auto thirdBuilder = Builder();
    auto thirdRequest = Request(*frontend, thirdBuilder);
    VfxGraphSchedulingOutcome blocked;
    std::size_t requests{};
    {
        Tests::AllocationProbe::ScopedMeasurement measurement;
        blocked = frontend->ScheduleVfx(thirdBuilder, snapshot, thirdRequest);
        requests = measurement.Snapshot().requests;
    }
    CHECK(requests == 0);
    CHECK(blocked.FailureDescriptor() == &RenderGraphExecutionErrors::VfxViewsRetained);
    CHECK(frontend->PrepareVfxScheduling({64, 3, 1'000'000}).HasError());
    copy = {};
    REQUIRE(frontend->ScheduleVfx(thirdBuilder, snapshot, thirdRequest).HasValue());
    frontend.reset();
    world.reset();
    const auto &retained = std::get<RenderGraphVfx>(second.Value().Workloads()[0].workload);
    CHECK(retained.order->view == 2);
    CHECK(retained.snapshot.Instances().size() == 2);
}

TEST_CASE("Released renderer views do not pin extraction frames and capacity failure preserves the recipe", "[vfx][graph][capacity]") {
    const std::array bindings{Binding(0)};
    auto budget = Budget();
    budget.frameSlots = 1;
    auto world = World(bindings, budget);
    Populate(*world, bindings[0]);
    auto snapshot = world->Extract(1, {}, 1).snapshot;
    auto frontend = Frontend();
    auto builder = Builder();
    auto request = Request(*frontend, builder);
    auto scheduled = frontend->ScheduleVfx(builder, snapshot, request);
    REQUIRE(scheduled.HasValue());
    snapshot = {};
    CHECK(world->Extract(2, {}, 1).report.status == VfxExtractionStatus::FramesRetained);
    scheduled = {};
    snapshot = world->Extract(2, {}, 1).snapshot;
    REQUIRE(snapshot.Frame() == 2);
    REQUIRE(frontend->PrepareVfxScheduling({1, 1, 1'000'000}).HasValue());
    auto limitedBuilder = Builder();
    auto limitedRequest = Request(*frontend, limitedBuilder);
    limitedRequest.frame = 2;
    VfxGraphSchedulingOutcome limited;
    std::size_t requests{};
    {
        Tests::AllocationProbe::ScopedMeasurement measurement;
        limited = frontend->ScheduleVfx(limitedBuilder, snapshot, limitedRequest);
        requests = measurement.Snapshot().requests;
    }
    CHECK(requests == 0);
    CHECK(limited.FailureDescriptor() == &RenderGraphExecutionErrors::VfxSchedulingCapacity);
    auto graph = std::move(limitedBuilder).Finalize();
    REQUIRE(graph.HasValue());
    CHECK(graph.Value().Passes().size() == 3);
}

TEST_CASE("Closed or moved live particle buffers reject extraction without allocation", "[vfx][allocation][lifecycle]") {
    const bool moveStorage = GENERATE(false, true);
    const std::array bindings{Binding(0)};
    auto world = World(bindings);
    Populate(*world, bindings[0]);
    auto borrowed = world->Particles(bindings[0].effect, bindings[0].emitter);
    REQUIRE(borrowed.HasValue());
    CpuParticleBuffer &buffer = *borrowed.Value();
    CpuParticleSoAConstView view;
    CpuParticleHandle handle;
    REQUIRE(buffer.TryReadView(view) == CpuParticleReadStatus::Complete);
    REQUIRE(buffer.TryHandleAt(0, handle) == CpuParticleReadStatus::Complete);
    std::unique_ptr<CpuParticleBuffer> parked;
    if (moveStorage)
        parked = std::make_unique<CpuParticleBuffer>(std::move(buffer));
    else
        REQUIRE(buffer.Shutdown().HasValue());

    VfxExtractionReport report;
    CpuParticleReadStatus viewStatus{};
    CpuParticleReadStatus identityStatus{};
    std::size_t requests{};
    {
        Tests::AllocationProbe::ScopedMeasurement measurement;
        viewStatus = buffer.TryReadView(view);
        identityStatus = buffer.TryHandleAt(0, handle);
        report = world->Extract(1, {}, 1).report;
        requests = measurement.Snapshot().requests;
    }
    CHECK(requests == 0);
    CHECK(viewStatus == CpuParticleReadStatus::ShutDown);
    CHECK(identityStatus == CpuParticleReadStatus::ShutDown);
    CHECK(view.positionX.empty());
    CHECK_FALSE(handle.IsValid());
    CHECK(report.status == VfxExtractionStatus::InvalidParticleData);
    const auto projectedView = std::as_const(buffer).View();
    const auto projectedHandle = buffer.HandleAt(0);
    REQUIRE(projectedView.HasError());
    REQUIRE(projectedHandle.HasError());
    CHECK(projectedView.ErrorValue().code.Value() == VfxErrors::ParticleBufferShutDown.code.Value());
    CHECK(projectedHandle.ErrorValue().code.Value() == VfxErrors::ParticleBufferShutDown.code.Value());
}
