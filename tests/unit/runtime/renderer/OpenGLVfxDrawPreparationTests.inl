/** @brief Real frontend/extraction with injected GL dispatch; these cases do not execute native shaders. */
namespace {
    /** @brief Restores terminal fake completion before the frontend unwinds, including fatal assertions. */
    struct SignaledCompletionOnExit final {
        ~SignaledCompletionOnExit() noexcept {
            OpenGLBackendTests::commandState.pollStatus = 0x911AU;  // GL_ALREADY_SIGNALED
        }
    };

    struct VfxDrawPreparationFixture final {
        VfxMaterialFixture material;
        std::unique_ptr<Horo::Vfx::VfxWorld> world;
        RenderGraphBuilder builder;
        VfxGraphSchedule schedule;
        VfxDrawView view;
        std::array<VfxGraphDrawBinding, 1> bindings;
    };

    VfxDrawPreparationFixture PrepareVfxDrawFixture(RenderFrontend &frontend) {
        REQUIRE(frontend.PrepareVfxScheduling({64, 2, 1'000'000, 8}).HasValue());
        auto material = PrepareVfxMaterial(frontend, {.cull = RenderCullMode::None,
                                                      .blend = RenderBlendMode::Additive,
                                                      .depthTest = true,
                                                      .depthWrite = false,
                                                      .colorFormat = RenderTextureFormat::Rgba16Float});
        auto slot = frontend.PrepareVfxFrameSlot(material.material);
        REQUIRE(slot.HasValue());
        const auto scope = Horo::Vfx::VfxIdentityScope::Create(17).Value();
        const Horo::Vfx::VfxEmitterPreparation emitter{.effect = {scope, 0, 1},
                                                       .emitter = {scope, 0, 1},
                                                       .particles = {.buffer = {scope, 0, 1}, .capacity = 16},
                                                       .material = {27},
                                                       .materialBlend = MaterialAlphaMode::Additive,
                                                       .blend = VfxParticlePass::Additive,
                                                       .topology = VfxPrimitiveTopology::CameraFacingBillboard};
        const std::array emitters{emitter};
        auto preparedWorld = Horo::Vfx::VfxWorld::Prepare(scope, emitters, {16, 64, 128, 2, 1'000'000});
        REQUIRE(preparedWorld.HasValue());
        auto world = std::move(preparedWorld).Value();
        REQUIRE(world->Play(emitter.effect).HasValue());
        auto particles = world->Particles(emitter.effect, emitter.emitter);
        REQUIRE(particles.HasValue());
        for (std::uint64_t index = 1; index <= 3; ++index)
            REQUIRE(particles.Value()->Spawn(Horo::Vfx::ParticleSimulationId::Create(index).Value()).HasValue());
        auto particleView = particles.Value()->View();
        REQUIRE(particleView.HasValue());
        for (std::size_t index = 0; index < 3; ++index) {
            particleView.Value().positionZ[index] = -2.0F - static_cast<float>(index);
            particleView.Value().sizeX[index] = 0.5F;
            particleView.Value().sizeY[index] = 0.75F;
            particleView.Value().packedColor[index] = 0x804020FF;
        }
        const auto snapshot = world->Extract(1, {}, 1).snapshot;
        auto createdBuilder = RenderGraphBuilder::Create({.maxPasses = 16, .maxResources = 16, .maxUsages = 64, .maxDependencies = 32});
        REQUIRE(createdBuilder.HasValue());
        auto builder = std::move(createdBuilder).Value();
        const auto color =
            PrepareTexture(frontend,
                           {.extent = {64, 32}, .format = RenderTextureFormat::Rgba16Float, .usage = RenderTextureUsage::RenderAttachment},
                           RenderTextureAspect::Color);
        const auto depth = PrepareTexture(frontend,
                                          {.extent = {64, 32},
                                           .format = RenderTextureFormat::Depth24Stencil8,
                                           .usage = RenderTextureUsage::RenderAttachment},
                                          RenderTextureAspect::DepthStencil);
        const auto target = frontend.CreateRenderTarget({.colorAttachment = color.view, .depthAttachment = depth.view, .extent = {64, 32}});
        REQUIRE(target.HasValue());
        REQUIRE(frontend.ProcessResourceRequests().HasValue());
        VfxGraphRequest request{.scene = 17,
                                .frame = 1,
                                .originRevision = 1,
                                .view = 2,
                                .viewGeneration = 3,
                                .maximumInstances = 64,
                                .remainingDrawCalls = 8};
        request.opaqueBegin = builder.AddPass(RenderPassKind::Graphics, RenderQueueRole::Graphics).Value();
        request.opaqueComplete = builder.AddPass(RenderPassKind::Graphics, RenderQueueRole::Graphics).Value();
        request.outputTransform = builder.AddPass(RenderPassKind::Graphics, RenderQueueRole::Graphics).Value();
        REQUIRE(builder.AddDependency({request.opaqueBegin, request.opaqueComplete}).HasValue());
        REQUIRE(builder.AddDependency({request.opaqueComplete, request.outputTransform}).HasValue());
        request.sceneColor = builder.ImportTexture(color.texture, RenderGraphResourceClass::Persistent).Value();
        request.sceneDepth = builder.ImportTexture(depth.texture, RenderGraphResourceClass::Persistent).Value();
        auto scheduled = frontend.ScheduleVfx(builder, snapshot, request);
        INFO((scheduled.HasError() ? scheduled.ErrorValue().message : "Canonical floating-point VFX recipe admitted"));
        REQUIRE(scheduled.HasValue());
        // Preserve explicit vertex/index handles for graph import; the canonical mesh owns its native dependency pins.
        std::array<MeshVertex, 4> vertices{};
        vertices[0].position = {-0.5F, -0.5F, 0};
        vertices[1].position = {0.5F, -0.5F, 0};
        vertices[2].position = {0.5F, 0.5F, 0};
        vertices[3].position = {-0.5F, 0.5F, 0};
        const std::array<std::uint32_t, 6> indices{0, 1, 2, 2, 3, 0};
        const auto vertex =
            frontend.CreateBuffer({.byteSize = sizeof(vertices), .usage = RenderBufferUsage::Vertex}, std::as_bytes(std::span{vertices}));
        const auto index =
            frontend.CreateBuffer({.byteSize = sizeof(indices), .usage = RenderBufferUsage::Index}, std::as_bytes(std::span{indices}));
        REQUIRE(vertex.HasValue());
        REQUIRE(index.HasValue());
        REQUIRE(frontend.ProcessResourceRequests().HasValue());
        const auto mesh = frontend.CreateMesh({.vertexBuffer = vertex.Value().handle,
                                               .indexBuffer = index.Value().handle,
                                               .vertexStride = sizeof(MeshVertex),
                                               .vertexCount = 4,
                                               .indexCount = 6,
                                               .localBounds = {{-0.5F, -0.5F, 0}, {0.5F, 0.5F, 0}}});
        REQUIRE(mesh.HasValue());
        REQUIRE(frontend.ProcessResourceRequests().HasValue());
        VfxGraphDrawBinding binding{.stage = VfxGraphStage::AdditiveForward,
                                    .mesh = mesh.Value().handle,
                                    .target = target.Value().handle,
                                    .vertices = builder.ImportBuffer(vertex.Value().handle, RenderGraphResourceClass::Persistent).Value(),
                                    .indices = builder.ImportBuffer(index.Value().handle, RenderGraphResourceClass::Persistent).Value(),
                                    .parameters = builder.ImportBuffer(material.buffer, RenderGraphResourceClass::Persistent).Value(),
                                    .slot = std::move(slot).Value()};
        return {std::move(material),
                std::move(world),
                std::move(builder),
                std::move(scheduled).Value(),
                {.scene = 17, .frame = 1, .originRevision = 1, .view = 2, .viewGeneration = 3},
                {std::move(binding)}};
    }

    TransientTest::GraphSources CompileVfxDrawFixture(VfxDrawPreparationFixture &fixture) {
        auto graph = Test::RequireGraph(fixture.builder);
        auto schedule = Test::RequireSchedule(graph);
        const auto &workload = std::get<RenderGraphVfx>(fixture.schedule.Workloads()[0].workload);
        std::vector<RenderGraphImportedState> initial;
        for (const auto &resource : graph.Resources()) {
            RenderGraphLogicalState state{.access = RenderGraphSynchronizationAccess::Read,
                                          .operation = RenderGraphSynchronizationOperation::GraphicsInputBuffer,
                                          .scope = RenderGraphPipelineScope::Graphics,
                                          .layout = RenderGraphTextureLayout::NotApplicable,
                                          .queue = {1}};
            if (resource.id == workload.color) {
                state.access = RenderGraphSynchronizationAccess::ReadWrite;
                state.operation = RenderGraphSynchronizationOperation::ColorAttachment;
                state.layout = RenderGraphTextureLayout::ColorAttachment;
            } else if (resource.id == workload.depth) {
                state.operation = RenderGraphSynchronizationOperation::DepthStencilAttachment;
                state.layout = RenderGraphTextureLayout::DepthStencilAttachment;
            }
            initial.push_back({resource.id, state});
        }
        const std::array queues{RenderQueueAssignment{RenderQueueRole::Graphics, {1}}};
        const auto synchronization = SynthesizeRenderGraphSynchronization(graph, schedule, queues, initial);
        REQUIRE(synchronization.HasValue());
        auto execution = CompileRenderGraphExecution(graph, schedule, synchronization.Value(), queues);
        REQUIRE(execution.HasValue());
        auto lifetime = CompileRenderGraphLifetimePlan(graph, schedule, {});
        REQUIRE(lifetime.HasValue());
        std::vector<RenderGraphPassWorkload> workloads;
        for (const auto &pass : execution.Value().Passes()) {
            const auto found = std::ranges::find(fixture.schedule.Workloads(), pass.pass, &RenderGraphPassWorkload::pass);
            workloads.push_back(found == fixture.schedule.Workloads().end() ? RenderGraphPassWorkload{pass.pass, {}} : *found);
        }
        return {std::move(graph), std::move(schedule), std::move(lifetime).Value(), std::move(execution).Value(), std::move(workloads)};
    }
}  // namespace

TEST_CASE("VFX draw preparation seals exact extraction view range and publication", "[renderer][opengl][vfx][unit]") {
    OpenGLResourceTests::ResourcePresentationPort port;
    auto frontend = Create(port);
    auto fixture = PrepareVfxDrawFixture(*frontend);
    const auto uploads = native.uploads;
    Result<void> prepared = Result<void>::Success();
    std::size_t allocations{};
    {
        Horo::Tests::AllocationProbe::ScopedMeasurement measurement;
        prepared = frontend->PrepareVfxDraws(fixture.builder, fixture.schedule, fixture.view, fixture.bindings);
        allocations = measurement.Snapshot().requests;
    }
    REQUIRE(prepared.HasValue());
    CHECK(allocations == 0);
    CHECK(native.uploads == uploads + 1);
    const auto &workload = std::get<RenderGraphVfx>(fixture.schedule.Workloads()[0].workload);
    REQUIRE(workload.order->draws.size() == 1);
    const auto &draw = workload.order->draws[0];
    CHECK(draw.instanceCount == 3);
    CHECK(draw.slot->ValidateDraw(workload, draw).HasValue());
    auto changed = draw;
    ++changed.revision;
    CHECK(changed.slot->ValidateDraw(workload, changed).HasError());
    changed = {};
    const std::array<VfxPackedDrawInstance, 1> instances{};
    CHECK(frontend->UpdateVfxFrame(fixture.bindings[0].slot, 2, instances).HasError());
    fixture.world.reset();
    CHECK(draw.slot->ValidateDraw(workload, draw).HasValue());
}

TEST_CASE("VFX draw preparation publishes no partial graph on native failure or callback recipe replacement",
          "[renderer][opengl][vfx][unit]") {
    OpenGLResourceTests::ResourcePresentationPort port;
    auto frontend = Create(port);
    auto fixture = PrepareVfxDrawFixture(*frontend);
    SECTION("Partial native upload") {
        native.failUpload = true;
    }
    SECTION("Callback replaces recipe identity") {
        native.onUpload = [&] {
            fixture.schedule.records[0].pass = {};
        };
    }
    CHECK(frontend->PrepareVfxDraws(fixture.builder, fixture.schedule, fixture.view, fixture.bindings).HasError());
    const auto &workload = std::get<RenderGraphVfx>(fixture.schedule.records[0].workload);
    CHECK_FALSE(workload.order->nativePrepared);
    CHECK_FALSE(workload.order->preparingDraws);
    CHECK(workload.order->draws.empty());
    native.failUpload = false;
    const std::array<VfxPackedDrawInstance, 1> instances{};
    const auto next = fixture.bindings[0].slot->Revision() + 1;
    CHECK(frontend->UpdateVfxFrame(fixture.bindings[0].slot, next, instances).HasValue());
}

TEST_CASE("VFX graph encodes the accepted instance count and retains the exact order through native completion",
          "[renderer][opengl][vfx][unit]") {
    OpenGLResourceTests::ResourcePresentationPort port;
    auto frontend = Create(port);
    const SignaledCompletionOnExit completionOnExit;
    REQUIRE(frontend->Capabilities().support.features.Supports(RenderCapability::VfxForwardInstancing));
    auto fixture = PrepareVfxDrawFixture(*frontend);
    REQUIRE(frontend->PrepareVfxDraws(fixture.builder, fixture.schedule, fixture.view, fixture.bindings).HasValue());
    auto source = CompileVfxDrawFixture(fixture);
    const std::weak_ptr<const VfxDrawOrder> retained = std::get<RenderGraphVfx>(fixture.schedule.records[0].workload).order;
    OpenGLBackendTests::commandState.pollStatus = 0x911BU;  // GL_TIMEOUT_EXPIRED
    SECTION("Successful encoding") {
        auto begun = frontend->BeginFrame({1, {64, 32}});
        REQUIRE(begun.HasValue());
        auto frame = std::move(begun).Value();
        const auto executed = frame.ExecuteGraph(source.execution, source.workloads);
        INFO((executed.HasError() ? executed.ErrorValue().message : "Exact VFX graph encoding accepted"));
        REQUIRE(executed.HasValue());
        REQUIRE(frame.Present().HasValue());
    }
    SECTION("Partial encoding abort") {
        native.failDraw = true;
        auto begun = frontend->BeginFrame({1, {64, 32}});
        REQUIRE(begun.HasValue());
        auto frame = std::move(begun).Value();
        CHECK(frame.ExecuteGraph(source.execution, source.workloads).HasError());
    }
    SECTION("Callback closes owner before native call returns") {
        bool closed = false, fenceDeferred = false;
        native.onDraw = [&] {
            const auto before = OpenGLBackendTests::commandState.fenceCount;
            closed = frontend->BeginShutdown().HasValue();
            fenceDeferred = OpenGLBackendTests::commandState.fenceCount == before;
            const auto pending = frontend->PollDrain();
            CHECK(pending.HasValue());
            CHECK(pending.Value() == RenderBackendDrainProgress::Pending);
        };
        auto begun = frontend->BeginFrame({1, {64, 32}});
        REQUIRE(begun.HasValue());
        auto frame = std::move(begun).Value();
        CHECK(frame.ExecuteGraph(source.execution, source.workloads).HasError());
        CHECK(closed);
        CHECK(fenceDeferred);
        CHECK(OpenGLBackendTests::commandState.fenceCount > 0);
    }
    CHECK(native.draws == 1);
    CHECK(native.instances == 3);
    source.workloads.clear();
    fixture.schedule = {};
    fixture.world.reset();
    CHECK(retained.use_count() > 1);  // Pool ownership alone does not prove a submitted revision is still borrowed.
    REQUIRE(frontend->BeginShutdown().HasValue());
    const auto pending = frontend->PollDrain();
    REQUIRE(pending.HasValue());
    CHECK(pending.Value() == RenderBackendDrainProgress::Pending);
    CHECK(retained.use_count() > 1);
    OpenGLBackendTests::commandState.pollStatus = 0x911AU;  // GL_ALREADY_SIGNALED
    Horo::Render::Test::RequireDrainedOwner(*frontend);
    CHECK(retained.use_count() == 1);  // Only the canonical reusable scheduling slot remains.
}
