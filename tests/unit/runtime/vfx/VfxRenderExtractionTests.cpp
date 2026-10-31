#include "Horo/Runtime/FrameScheduler.h"
#include "Horo/Runtime/Render/NullBackendModule.h"
#include "Horo/Runtime/Render/RenderFrontend.h"
#include "Horo/Runtime/Render/RenderGraphExecutionErrors.h"
#include "Horo/Runtime/Scene/RuntimeScene.h"
#include "Horo/Vfx/VfxWorld.h"
#include "VfxRenderTestSupport.h"
#include "support/AllocationProbe.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <limits>
#include <thread>
#include <utility>

namespace {
    using namespace Horo;
    using namespace Horo::Render;
    using namespace Horo::Vfx;
    using namespace Test::VfxRender;

    struct PreparationTrace {
        unsigned owned{};
        unsigned immutable{};
        bool failSecond{};
    };

    class PreparedCandidate final : public Runtime::SceneActivationCandidate {
    public:
        Result<void> ValidatePublication() const override {
            return Result<void>::Success();
        }

        void Shutdown() noexcept override {
            // This validation-only candidate owns no subsystem resources to retire.
        }
    };

    class VfxHostAdapter final : public Runtime::SceneActivationParticipant {
    public:
        VfxHostAdapter(PreparationTrace &trace, bool owner) : trace_(trace), owner_(owner) {}

        Result<void> PrepareOwnedScene(Runtime::RuntimeScene &scene) override {
            ++trace_.owned;
            if (!owner_)
                return trace_.failSecond ? Result<void>::Failure(MakeError(VfxErrors::IdentityInvalid)) : Result<void>::Success();
            auto binding = Binding(0);
            const auto scope = VfxIdentityScope::Create(scene.View().RuntimeId().value).Value();
            binding.effect.scope = scope;
            binding.emitter.scope = scope;
            binding.particles.buffer.scope = scope;
            auto budget = Budget();
            budget.frameSlots = 1;
            const std::array bindings{binding};
            auto world = VfxWorld::Prepare(scope, bindings, budget);
            if (world.HasError())
                return Result<void>::Failure(world.ErrorValue());
            Populate(*world.Value(), binding);
            return scene.AttachVfxWorld(std::move(world).Value(), {}, 1);
        }

        Result<std::unique_ptr<Runtime::SceneActivationCandidate>> Prepare(const Runtime::RuntimeSceneDefinition &,
                                                                           const Runtime::RuntimeSceneView &) override {
            REQUIRE(trace_.owned == 2);
            ++trace_.immutable;
            return Result<std::unique_ptr<Runtime::SceneActivationCandidate>>::Success(std::make_unique<PreparedCandidate>());
        }

    private:
        PreparationTrace &trace_;
        bool owner_;
    };

    Runtime::RuntimeSceneDefinition EmptyDefinition() {
        Runtime::SceneDefinitionBuilder builder{Runtime::SceneDefinitionId{1}, Runtime::SceneDefinitionRevision{1}};
        auto built = std::move(builder).Build();
        REQUIRE(built.HasValue());
        return std::move(built).Value();
    }

    /** @brief Verifies one stage's blend membership and records exactly one color visit per instance. */
    void CheckStageInstances(const RenderGraphVfx &payload, const RenderWorldSnapshot &snapshot, StandardPbrRasterFamily family,
                             std::array<std::uint32_t, 8> &colorVisits) {
        using enum VfxGraphStage;
        for (const auto &reference : payload.order->instances) {
            const auto blend = snapshot.Batches()[reference.batch].blend;
            if (payload.stage == TranslucentForward)
                CHECK(blend == VfxParticlePass::Translucent);
            if (payload.stage == AdditiveForward)
                CHECK(blend == VfxParticlePass::Additive);
            if (payload.stage != Shadow && payload.stage != Depth)
                ++colorVisits[reference.instance];
            if (payload.stage == GBuffer)
                CHECK(family == StandardPbrRasterFamily::Deferred);
        }
    }

}  // namespace

TEST_CASE("Frontend admits scene-linear VFX textures while preserving unsupported format rejection", "[vfx][graph][resource]") {
    using enum RenderTextureFormat;
    auto frontend = Frontend();
    for (const auto format : {Rgba16Float, Rgba32Float}) {
        auto texture = frontend->CreateTexture({.extent = {16, 16}, .format = format, .usage = RenderTextureUsage::RenderAttachment});
        REQUIRE(texture.HasValue());
        REQUIRE(frontend->ProcessResourceRequests().Value() == 1);
        CHECK(frontend->ResourceState(texture.Value().handle).Value() == RenderResourceState::Ready);
        auto builder = Builder();
        CHECK(builder.ImportTexture(texture.Value().handle, RenderGraphResourceClass::Persistent).HasValue());
    }
    const auto unsupported =
        frontend->CreateTexture({.extent = {16, 16}, .format = Rg16Float, .usage = RenderTextureUsage::RenderAttachment});
    REQUIRE(unsupported.HasError());
    CHECK(unsupported.ErrorValue().code.Value() == "render.frontend.resource.unsupported");
    CHECK(frontend->ProcessResourceRequests().Value() == 0);
}

TEST_CASE("Live effect extraction owns immutable data and stable compacted identities", "[vfx][extraction]") {
    const std::array bindings{Binding(0)};
    auto world = World(bindings);
    Populate(*world, bindings[0], 3);
    auto particles = world->Particles(bindings[0].effect, bindings[0].emitter).Value();
    REQUIRE(particles->Kill(particles->HandleAt(0).Value()).HasValue());
    auto frame = world->Extract(1, Math::WorldCoordinate64::FromMillimeters(1000, 0, 0), 4);
    REQUIRE(frame.report.status == VfxExtractionStatus::Complete);
    REQUIRE(frame.snapshot.Instances().size() == 2);
    CHECK(frame.snapshot.Instances()[0].particle == 3);
    CHECK(frame.snapshot.Instances()[0].position.x == -1);
    REQUIRE(world->Stop(bindings[0].effect).HasValue());
    world.reset();
    CHECK(frame.snapshot.Instances()[0].particle == 3);
    CHECK(frame.snapshot.OriginRevision() == 4);
}

TEST_CASE("Prepared extraction and overload reporting allocate no frame-hot storage", "[vfx][extraction]") {
    const std::array bindings{Binding(0), Binding(1)};
    auto budget = Budget();
    budget.maximumInstances = 2;
    auto world = World(bindings, budget);
    Populate(*world, bindings[0]);
    Populate(*world, bindings[1]);
    std::size_t requests{};
    VfxExtractionReport report;
    {
        Tests::AllocationProbe::ScopedMeasurement measurement;
        for (std::uint64_t frame = 1; frame <= 128; ++frame)
            report = world->Extract(frame, {}, 1).report;
        requests = measurement.Snapshot().requests;
    }
    CHECK(requests == 0);
    CHECK(report.status == VfxExtractionStatus::BudgetExceeded);
    CHECK(report.admittedInstances == 2);
    CHECK(report.omittedInstances == 2);
    CHECK(report.omittedBatches == 1);
    CHECK(report.firstOmitted.effectSlot == 1);
}

TEST_CASE("Retained frame ring applies backpressure and supports independent concurrent readers", "[vfx][extraction]") {
    const std::array bindings{Binding(0)};
    auto budget = Budget();
    budget.frameSlots = 1;
    auto world = World(bindings, budget);
    Populate(*world, bindings[0]);
    auto retained = world->Extract(1, {}, 1).snapshot;
    CHECK(world->Extract(2, {}, 1).report.status == VfxExtractionStatus::FramesRetained);
    auto reader = retained;
    std::uint64_t observed{};
    std::jthread thread{[reader = std::move(reader), &observed] {
        observed = reader.Instances()[0].particle;
    }};
    thread.join();
    CHECK(observed == 1);
    retained = {};
    CHECK(world->Extract(2, {}, 1).report.status == VfxExtractionStatus::Complete);
}

TEST_CASE("World rejects stale, foreign, conflicting and nonfinite extraction inputs", "[vfx][extraction]") {
    std::array bindings{Binding(0)};
    auto world = World(bindings);
    Populate(*world, bindings[0]);
    auto next = world->Stop(bindings[0].effect);
    REQUIRE(next.HasValue());
    CHECK(world->Play(bindings[0].effect).HasError());
    REQUIRE(world->Play(next.Value()).HasValue());
    CHECK(world->Extract(0, {}, 1).report.status == VfxExtractionStatus::InvalidFrame);
    CHECK(world->Extract(1, {}, 0).report.status == VfxExtractionStatus::InvalidFrame);
    bindings[0].materialBlend = MaterialAlphaMode::Opaque;
    CHECK(VfxWorld::Prepare(Scope(), bindings, Budget()).HasError());
    bindings[0] = Binding(0, VfxParticlePass::Translucent);
    bindings[0].shadowCaster = true;
    CHECK(VfxWorld::Prepare(Scope(), bindings, Budget()).HasError());
    REQUIRE(world->Shutdown().HasValue());
    CHECK(world->Extract(1, {}, 1).report.status == VfxExtractionStatus::ShutDown);
}

TEST_CASE("Every blend class is consumed by frontend typed graph passes for every raster family", "[vfx][graph]") {
    const std::array bindings{Binding(0, VfxParticlePass::Opaque), Binding(1, VfxParticlePass::Masked),
                              Binding(2, VfxParticlePass::Translucent), Binding(3, VfxParticlePass::Additive)};
    auto world = World(bindings);
    for (const auto &binding : bindings)
        Populate(*world, binding);
    const auto snapshot = world->Extract(1, {}, 1).snapshot;
    auto frontend = Frontend();
    for (const auto family :
         {StandardPbrRasterFamily::Forward, StandardPbrRasterFamily::ClusteredForward, StandardPbrRasterFamily::Deferred}) {
        auto builder = Builder();
        auto request = Request(*frontend, builder);
        request.family = family;
        auto scheduled = frontend->ScheduleVfx(builder, snapshot, request);
        REQUIRE(scheduled.HasValue());
        CHECK(scheduled.Value().instances == 8);
        std::array<std::uint32_t, 8> colorVisits{};
        for (const auto &work : scheduled.Value().Workloads()) {
            const auto &payload = std::get<RenderGraphVfx>(work.workload);
            CheckStageInstances(payload, snapshot, family, colorVisits);
        }
        CHECK(std::ranges::all_of(colorVisits, [](auto count) {
            return count == 1;
        }));
        auto graph = builder.Finalize();
        REQUIRE(graph.HasValue());
        CHECK(CompileRenderGraph(graph.Value()).HasValue());
    }
}

TEST_CASE("Alpha view radix ordering uses stable identities without changing simulation and additive stays unsorted", "[vfx][graph]") {
    const std::array bindings{Binding(0, VfxParticlePass::Translucent), Binding(1, VfxParticlePass::Translucent), Binding(2)};
    auto world = World(bindings);
    for (const auto &binding : bindings)
        Populate(*world, binding);
    const auto snapshot = world->Extract(1, {}, 1).snapshot;
    auto frontend = Frontend();
    auto builder = Builder();
    auto request = Request(*frontend, builder);
    auto scheduled = frontend->ScheduleVfx(builder, snapshot, request);
    REQUIRE(scheduled.HasValue());
    const auto &alpha = std::get<RenderGraphVfx>(scheduled.Value().Workloads()[0].workload);
    REQUIRE(alpha.order->instances.size() == 4);
    CHECK(alpha.order->instances[0].instance == 1);
    CHECK(alpha.order->instances[1].instance == 3);
    CHECK(alpha.order->instances[2].instance == 0);
    CHECK(alpha.order->instances[3].instance == 2);
    const auto &additive = std::get<RenderGraphVfx>(scheduled.Value().Workloads()[1].workload);
    CHECK(additive.order->instances[0].instance == 4);
    CHECK(additive.order->instances[1].instance == 5);
    CHECK(snapshot.Instances()[0].particle == 1);
    CHECK(world->Particles(bindings[0].effect, bindings[0].emitter).Value()->HandleAt(0).Value().particle.Value() == 1);
    CHECK(scheduled.Value().drawCalls == 5);
}

TEST_CASE("Failed VFX graph admission leaves existing recipe records intact", "[vfx][graph]") {
    const std::array bindings{Binding(0, VfxParticlePass::Translucent)};
    auto world = World(bindings);
    Populate(*world, bindings[0]);
    auto snapshot = world->Extract(1, {}, 1).snapshot;
    auto frontend = Frontend();
    for (int failure = 0; failure < 4; ++failure) {
        auto builder = Builder(failure == 3 ? 3 : 32);
        auto request = Request(*frontend, builder);
        if (failure == 0)
            request.frame = 2;
        if (failure == 1)
            request.opaqueBegin.id = RenderPassId{999};
        if (failure == 2)
            request.remainingDrawCalls = 0;
        CHECK(frontend->ScheduleVfx(builder, snapshot, request).HasError());
        auto graph = builder.Finalize();
        REQUIRE(graph.HasValue());
        CHECK(graph.Value().Passes().size() == 3);
        CHECK(graph.Value().Dependencies().size() == 2);
        CHECK(graph.Value().Usages().empty());
    }
}

TEST_CASE("RuntimeScene owns live VFX and renderer workload lease survives scene replacement", "[vfx][scene][graph]") {
    Runtime::SceneDefinitionBuilder definition{Runtime::SceneDefinitionId{1}, Runtime::SceneDefinitionRevision{1}};
    auto built = std::move(definition).Build();
    REQUIRE(built.HasValue());
    auto created = Runtime::RuntimeScene::Create(built.Value(), {17});
    REQUIRE(created.HasValue());
    auto scene = std::move(created).Value();
    const std::array bindings{Binding(0)};
    auto world = World(bindings);
    Populate(*world, bindings[0]);
    const auto attached = scene->AttachVfxWorld(std::move(world), {}, 1);
    REQUIRE(attached.HasValue());
    const auto extracted = scene->CaptureVfx(1);
    auto frontend = Frontend();
    auto builder = Builder();
    auto request = Request(*frontend, builder);
    auto scheduled = frontend->ScheduleVfx(builder, extracted.snapshot, request);
    REQUIRE(scheduled.HasValue());
    scene.reset();
    const auto retainedWorkloads = scheduled.Value().Workloads();
    const auto &payload = std::get<RenderGraphVfx>(retainedWorkloads.front().workload);
    CHECK(payload.snapshot.Instances().size() == 2);
    CHECK(payload.snapshot.Instances()[0].particle == 1);
}

TEST_CASE("Native VFX execution rejects the complete graph before earlier work can encode", "[vfx][graph]") {
    const std::array bindings{Binding(0)};
    auto world = World(bindings);
    Populate(*world, bindings[0]);
    const auto snapshot = world->Extract(1, {}, 1).snapshot;
    auto frontend = Frontend();
    auto builder = Builder();
    auto request = Request(*frontend, builder);
    auto scheduled = frontend->ScheduleVfx(builder, snapshot, request);
    REQUIRE(scheduled.HasValue());
    auto graph = builder.Finalize();
    REQUIRE(graph.HasValue());
    auto schedule = CompileRenderGraph(graph.Value());
    REQUIRE(schedule.HasValue());
    const std::array queues{RenderQueueAssignment{RenderQueueRole::Graphics, RenderQueueId{1}}};
    const RenderGraphLogicalState colorState{RenderGraphSynchronizationAccess::Write, RenderGraphSynchronizationOperation::ColorAttachment,
                                             RenderGraphPipelineScope::Graphics, RenderGraphTextureLayout::ColorAttachment,
                                             RenderQueueId{1}};
    const RenderGraphLogicalState depthState{RenderGraphSynchronizationAccess::Write,
                                             RenderGraphSynchronizationOperation::DepthStencilAttachment,
                                             RenderGraphPipelineScope::Graphics, RenderGraphTextureLayout::DepthStencilAttachment,
                                             RenderQueueId{1}};
    const std::array importedStates{RenderGraphImportedState{request.sceneColor, colorState},
                                    RenderGraphImportedState{request.sceneDepth, depthState},
                                    RenderGraphImportedState{request.shadowDepth, depthState},
                                    RenderGraphImportedState{request.gBuffer, colorState}};
    auto synchronization = SynthesizeRenderGraphSynchronization(graph.Value(), schedule.Value(), queues, importedStates);
    const auto synchronizationDiagnostic =
        synchronization.HasError() ? synchronization.ErrorValue().code.Value() : std::string_view{"Synchronization accepted"};
    INFO(synchronizationDiagnostic);
    REQUIRE(synchronization.HasValue());
    auto execution = CompileRenderGraphExecution(graph.Value(), schedule.Value(), synchronization.Value(), queues);
    REQUIRE(execution.HasValue());
    std::vector<RenderGraphPassWorkload> workloads;
    for (const auto &pass : execution.Value().Passes()) {
        const auto found = std::ranges::find(scheduled.Value().Workloads(), pass.pass, &RenderGraphPassWorkload::pass);
        workloads.push_back(found == scheduled.Value().Workloads().end() ? RenderGraphPassWorkload{pass.pass, std::monostate{}} : *found);
    }
    auto begun = frontend->BeginFrame({.frameNumber = 1, .outputExtent = {16, 16}});
    REQUIRE(begun.HasValue());
    auto frame = std::move(begun).Value();
    auto encoded = frame.ExecuteGraph(execution.Value(), workloads);
    REQUIRE(encoded.HasError());
    CHECK(encoded.ErrorValue().code.Value() == RenderGraphExecutionErrors::VfxExecutionUnavailable.code.Value());
    CHECK(frame.Present().HasError());
}

TEST_CASE("Aggregate draw admission counts alpha interleaving and exact solid pass multiplicity", "[vfx][graph]") {
    const std::array bindings{Binding(0, VfxParticlePass::Translucent), Binding(1, VfxParticlePass::Translucent)};
    auto budget = Budget();
    budget.maximumDrawCalls = 3;
    auto limited = World(bindings, budget);
    for (const auto &binding : bindings)
        Populate(*limited, binding);
    const auto omitted = limited->Extract(1, {}, 1);
    CHECK(omitted.report.status == VfxExtractionStatus::BudgetExceeded);
    CHECK(omitted.report.reservedDrawCalls == 2);
    CHECK(omitted.report.omittedBatches == 1);
    auto world = World(bindings);
    for (const auto &binding : bindings)
        Populate(*world, binding);
    auto snapshot = world->Extract(1, {}, 1).snapshot;
    auto frontend = Frontend();
    auto builder = Builder();
    auto request = Request(*frontend, builder);
    request.remainingDrawCalls = 3;
    auto rejected = frontend->ScheduleVfx(builder, snapshot, request);
    REQUIRE(rejected.HasError());
    CHECK(rejected.ErrorValue().code.Value() == RenderGraphExecutionErrors::VfxDrawBudgetExceeded.code.Value());
    request.remainingDrawCalls = 4;
    auto exact = frontend->ScheduleVfx(builder, snapshot, request);
    REQUIRE(exact.HasValue());
    CHECK(exact.Value().drawCalls == 4);
}

TEST_CASE("Canonical emitter and render origins preserve local millimeters far from world zero", "[vfx][extraction]") {
    std::array bindings{Binding(0)};
    constexpr std::int64_t base = 9'000'000'000'000'000'000;
    bindings[0].simulationOrigin = Math::WorldCoordinate64::FromMillimeters(base + 1, base, base);
    auto world = World(bindings);
    Populate(*world, bindings[0], 1);
    auto extracted = world->Extract(1, Math::WorldCoordinate64::FromMillimeters(base, base, base), 2);
    REQUIRE(extracted.report.status == VfxExtractionStatus::Complete);
    CHECK(Math::NearlyEqual(extracted.snapshot.Instances()[0].position.x, 0.001F));
}

TEST_CASE("Normal scene preparation completes owners before immutable capture and feeds frontend extraction", "[vfx][scene][graph]") {
    PreparationTrace trace;
    Runtime::RuntimeSceneService service;
    REQUIRE(service.AddActivationParticipant(std::make_unique<VfxHostAdapter>(trace, true)).HasValue());
    REQUIRE(service.AddActivationParticipant(std::make_unique<VfxHostAdapter>(trace, false)).HasValue());
    REQUIRE(service.Startup({}).HasValue());
    REQUIRE(service.QueuePreparation(EmptyDefinition()).HasValue());
    CHECK(trace.owned == 2);
    CHECK(trace.immutable == 2);
    CHECK_FALSE(service.ActiveScene().has_value());
    const CancellationToken cancellation;
    Runtime::FrameContext context{.frameNumber = 1, .cancellation = cancellation};
    REQUIRE(service.OnPhase(Runtime::RuntimePhase::CommitDeferredLifecycleChanges, context).HasValue());
    REQUIRE(service.ActiveScene().has_value());
    REQUIRE(service.OnPhase(Runtime::RuntimePhase::RenderExtraction, context).HasValue());
    auto retained = service.VfxFrame().snapshot;
    REQUIRE(retained.Instances().size() == 2);
    context.frameNumber = 2;
    REQUIRE(service.OnPhase(Runtime::RuntimePhase::RenderExtraction, context).HasValue());
    CHECK(service.VfxFrame().report.status == VfxExtractionStatus::FramesRetained);
    CHECK(service.VfxFrame().snapshot.Frame() == 0);
    CHECK(retained.Frame() == 1);
    retained = {};
    auto frontend = Frontend();
    for (std::uint64_t frame = 3; frame <= 4; ++frame) {
        context.frameNumber = frame;
        REQUIRE(service.OnPhase(Runtime::RuntimePhase::RenderExtraction, context).HasValue());
        REQUIRE(service.VfxFrame().report.status == VfxExtractionStatus::Complete);
        auto builder = Builder();
        auto request = Request(*frontend, builder);
        request.scene = service.ActiveScene()->RuntimeId().value;
        request.frame = frame;
        auto scheduled = frontend->ScheduleVfx(builder, service.VfxFrame().snapshot, request);
        REQUIRE(scheduled.HasValue());
        CHECK(scheduled.Value().instances == 2);
    }
    auto cloned = service.CloneActive({999});
    REQUIRE(cloned.HasError());
    CHECK(cloned.ErrorValue().code.Value() == VfxErrors::SceneCloneUnavailable.code.Value());
    service.Shutdown();
    CHECK(service.VfxFrame().snapshot.Frame() == 0);
}

TEST_CASE("Failed unpublished VFX owner preparation cannot leak into the active scene", "[vfx][scene]") {
    PreparationTrace trace{.failSecond = true};
    Runtime::RuntimeSceneService service;
    REQUIRE(service.AddActivationParticipant(std::make_unique<VfxHostAdapter>(trace, true)).HasValue());
    REQUIRE(service.AddActivationParticipant(std::make_unique<VfxHostAdapter>(trace, false)).HasValue());
    REQUIRE(service.Startup({}).HasValue());
    CHECK(service.QueuePreparation(EmptyDefinition()).HasError());
    CHECK(trace.owned == 2);
    CHECK(trace.immutable == 0);
    CHECK_FALSE(service.ActiveScene().has_value());
    const CancellationToken cancellation;
    REQUIRE(service.OnPhase(Runtime::RuntimePhase::RenderExtraction, Runtime::FrameContext{.frameNumber = 1, .cancellation = cancellation})
                .HasValue());
    CHECK(service.VfxFrame().snapshot.Frame() == 0);
    service.Shutdown();
}

TEST_CASE("Exhausted effect generation stops permanently without wrapping or invalidating retained frames", "[vfx][extraction]") {
    std::array bindings{Binding(0)};
    bindings[0].effect.generation = std::numeric_limits<std::uint32_t>::max();
    auto world = World(bindings);
    Populate(*world, bindings[0]);
    auto retained = world->Extract(1, {}, 1).snapshot;
    auto stopped = world->Stop(bindings[0].effect);
    REQUIRE(stopped.HasError());
    CHECK(stopped.ErrorValue().code.Value() == VfxErrors::GenerationExhausted.code.Value());
    CHECK(world->Play(bindings[0].effect).HasError());
    CHECK(world->Extract(2, {}, 1).snapshot.Instances().empty());
    CHECK(retained.Instances().size() == 2);
}
