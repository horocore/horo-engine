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
        std::vector<VfxGraphDrawBinding> bindings;
        std::vector<VfxMaterialFixture> extraMaterials;
    };

    VfxDrawPreparationFixture PrepareVfxDrawFixture(RenderFrontend &frontend, std::uint32_t particleCount = 3) {
        const auto maximumInstances = std::max(64U, particleCount);
        REQUIRE(frontend.PrepareVfxScheduling({maximumInstances, 2, 1'000'000, 8}).HasValue());
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
                                                       .particles = {.buffer = {scope, 0, 1}, .capacity = std::max(16U, particleCount)},
                                                       .material = {27},
                                                       .materialBlend = MaterialAlphaMode::Additive,
                                                       .blend = VfxParticlePass::Additive,
                                                       .topology = VfxPrimitiveTopology::CameraFacingBillboard};
        const std::array emitters{emitter};
        auto preparedWorld = Horo::Vfx::VfxWorld::Prepare(scope, emitters, {16, maximumInstances, 128, 2, 1'000'000});
        REQUIRE(preparedWorld.HasValue());
        auto world = std::move(preparedWorld).Value();
        REQUIRE(world->Play(emitter.effect).HasValue());
        auto particles = world->Particles(emitter.effect, emitter.emitter);
        REQUIRE(particles.HasValue());
        for (std::uint64_t index = 1; index <= particleCount; ++index)
            REQUIRE(particles.Value()->Spawn(Horo::Vfx::ParticleSimulationId::Create(index).Value()).HasValue());
        auto particleView = particles.Value()->View();
        REQUIRE(particleView.HasValue());
        for (std::size_t index = 0; index < particleCount; ++index) {
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
                                .maximumInstances = maximumInstances,
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
        std::vector<VfxGraphDrawBinding> bindings;
        bindings.push_back(std::move(binding));
        std::vector<VfxMaterialFixture> extraMaterials;
        for (std::uint32_t first = MaximumVfxDrawInstances; first < particleCount; first += MaximumVfxDrawInstances) {
            auto extra = PrepareVfxMaterial(frontend, {.cull = RenderCullMode::None,
                                                       .blend = RenderBlendMode::Additive,
                                                       .depthTest = true,
                                                       .depthWrite = false,
                                                       .colorFormat = RenderTextureFormat::Rgba16Float});
            auto extraSlot = frontend.PrepareVfxFrameSlot(extra.material);
            REQUIRE(extraSlot.HasValue());
            auto next = bindings.front();
            next.firstOrder = first;
            next.parameters = builder.ImportBuffer(extra.buffer, RenderGraphResourceClass::Persistent).Value();
            next.slot = std::move(extraSlot).Value();
            bindings.push_back(std::move(next));
            extraMaterials.push_back(std::move(extra));
        }
        return {std::move(material),
                std::move(world),
                std::move(builder),
                std::move(scheduled).Value(),
                {.scene = 17, .frame = 1, .originRevision = 1, .view = 2, .viewGeneration = 3},
                std::move(bindings),
                std::move(extraMaterials)};
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

    /** @brief Fixed protocol observations of consecutive chunk uploads; captures no native/GPU execution evidence. */
    struct VfxChunkUploadCapture final {
        static constexpr std::size_t Bytes = sizeof(VfxDrawUniformBlock);
        std::array<std::array<std::byte, Bytes>, 2> chunks{};
        std::size_t calls{};
        bool valid{true};

        VfxChunkUploadCapture() {
            native.onUploadBytes = [this](const std::uint32_t target, const std::span<const std::byte> bytes) noexcept {
                valid = valid && target == 0x8892U && bytes.size() == Bytes;  // Actual GL_ARRAY_BUFFER upload route.
                if (bytes.size() == Bytes)
                    std::ranges::copy(bytes, chunks[calls % chunks.size()].begin());
                ++calls;
            };
        }

        ~VfxChunkUploadCapture() {
            native.onUploadBytes = {};
        }
    };

    /** @brief Seeds distinct genuine accepted slot revisions before testing a later multi-chunk failure. */
    void SeedVfxChunks(RenderFrontend &frontend, VfxDrawPreparationFixture &fixture) {
        const std::array<VfxPackedDrawInstance, 1> initial{};
        for (auto &binding : fixture.bindings)
            REQUIRE(frontend.UpdateVfxFrame(binding.slot, 1, initial).HasValue());
    }

    /** @brief Computes expected protocol source bytes via canonical public packers, outside measured preparation. */
    std::array<std::byte, VfxChunkUploadCapture::Bytes> ExpectedVfxChunk(const VfxDrawPreparationFixture &fixture,
                                                                         const std::uint32_t first) {
        const auto &workload = std::get<RenderGraphVfx>(fixture.schedule.Workloads()[0].workload);
        const auto count = std::min<std::size_t>(MaximumVfxDrawInstances, workload.snapshot.Instances().size() - first);
        std::array<VfxPackedDrawInstance, MaximumVfxDrawInstances> instances{};
        for (std::size_t index = 0; index < count; ++index) {
            const auto packed = PackVfxDrawInstance(workload.snapshot.Instances()[first + index],
                                                    VfxPrimitiveTopology::CameraFacingBillboard, fixture.view);
            REQUIRE(packed.HasValue());
            instances[index] = packed.Value();
        }
        std::array<std::byte, VfxChunkUploadCapture::Bytes> bytes{};
        REQUIRE(PackVfxShaderParameters(fixture.bindings.front().slot->Material()->Descriptor().layout, std::span{instances}.first(count),
                                        bytes)
                    .HasValue());
        return bytes;
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

TEST_CASE("VFX multi-chunk staging rejects later geometry before any native write", "[renderer][opengl][vfx][unit]") {
    OpenGLResourceTests::ResourcePresentationPort port;
    auto frontend = Create(port);
    auto fixture = PrepareVfxDrawFixture(*frontend, 129);
    REQUIRE(fixture.bindings.size() == 2);
    VfxChunkUploadCapture capture;
    SeedVfxChunks(*frontend, fixture);
    const auto before = capture.chunks;
    const auto uploads = native.uploads;
    const auto mesh = fixture.bindings[1].mesh;
    fixture.bindings[1].mesh = {};
    CHECK(frontend->PrepareVfxDraws(fixture.builder, fixture.schedule, fixture.view, fixture.bindings).HasError());
    CHECK(native.uploads == uploads);
    CHECK(capture.chunks == before);
    CHECK(capture.valid);
    for (const auto &binding : fixture.bindings)
        CHECK(binding.slot->Revision() == 1);
    const auto &workload = std::get<RenderGraphVfx>(fixture.schedule.Workloads()[0].workload);
    CHECK_FALSE(workload.order->nativePrepared);
    CHECK_FALSE(workload.order->preparingDraws);
    CHECK(workload.order->draws.empty());
    fixture.bindings[1].mesh = mesh;
    REQUIRE(frontend->PrepareVfxDraws(fixture.builder, fixture.schedule, fixture.view, fixture.bindings).HasValue());
    CHECK(native.uploads == uploads + 2);
}

TEST_CASE("VFX multi-chunk staging preserves canonical bytes without preparation allocations", "[renderer][opengl][vfx][unit]") {
    OpenGLResourceTests::ResourcePresentationPort port;
    auto frontend = Create(port);
    auto fixture = PrepareVfxDrawFixture(*frontend, 129);
    const std::array expected{ExpectedVfxChunk(fixture, 0), ExpectedVfxChunk(fixture, 128)};
    VfxChunkUploadCapture capture;
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
    CHECK(native.uploads == uploads + 2);
    CHECK(capture.calls == 2);
    CHECK(capture.valid);
    CHECK(capture.chunks == expected);
    const auto &workload = std::get<RenderGraphVfx>(fixture.schedule.Workloads()[0].workload);
    REQUIRE(workload.order->draws.size() == 2);
    CHECK(workload.order->draws[0].instanceCount == 128);
    CHECK(workload.order->draws[1].instanceCount == 1);
}

TEST_CASE("VFX first upload cannot overwrite a future claimed chunk staging", "[renderer][opengl][vfx][unit]") {
    OpenGLResourceTests::ResourcePresentationPort port;
    auto frontend = Create(port);
    auto fixture = PrepareVfxDrawFixture(*frontend, 129);
    const auto expected = ExpectedVfxChunk(fixture, 128);
    VfxChunkUploadCapture capture;
    SeedVfxChunks(*frontend, fixture);
    const auto before = capture.chunks[1];
    const std::array<VfxPackedDrawInstance, 1> replacement{};
    bool pending{}, untouched{};
    native.onUpload = [&] {
        const auto nested = frontend->UpdateVfxFrame(fixture.bindings[1].slot, 2, replacement);
        pending = nested.HasError() && nested.ErrorValue().code.Value() == VfxFrameErrors::Pending.code.Value();
        untouched = capture.chunks[1] == before;
    };
    REQUIRE(frontend->PrepareVfxDraws(fixture.builder, fixture.schedule, fixture.view, fixture.bindings).HasValue());
    CHECK(pending);
    CHECK(untouched);
    CHECK(capture.valid);
    CHECK(capture.chunks[1] == expected);
    CHECK(fixture.bindings[1].slot->Revision() == 2);
}

TEST_CASE("VFX later native chunk failure releases staging claims for exact canonical retry", "[renderer][opengl][vfx][unit]") {
    OpenGLResourceTests::ResourcePresentationPort port;
    auto frontend = Create(port);
    auto fixture = PrepareVfxDrawFixture(*frontend, 129);
    VfxChunkUploadCapture capture;
    SeedVfxChunks(*frontend, fixture);
    const auto uploads = native.uploads;
    native.onUpload = [] {
        native.onUpload = [] {
            native.failUpload = true;
        };
    };
    CHECK(frontend->PrepareVfxDraws(fixture.builder, fixture.schedule, fixture.view, fixture.bindings).HasError());
    CHECK(native.uploads == uploads + 2);
    CHECK(fixture.bindings[0].slot->Revision() == 2);
    CHECK(fixture.bindings[1].slot->Revision() == 0);
    const auto &workload = std::get<RenderGraphVfx>(fixture.schedule.Workloads()[0].workload);
    CHECK_FALSE(workload.order->nativePrepared);
    CHECK_FALSE(workload.order->preparingDraws);
    CHECK(workload.order->draws.empty());
    // A moved-from std::function need not be empty; explicitly disarm the injector before retrying.
    native.onUpload = {};
    native.failUpload = false;
    const auto retried = frontend->PrepareVfxDraws(fixture.builder, fixture.schedule, fixture.view, fixture.bindings);
    INFO((retried.HasError()
              ? retried.ErrorValue().domain.Value() + "/" + retried.ErrorValue().code.Value() + ": " + retried.ErrorValue().message
              : "Exact canonical multi-chunk retry accepted"));
    REQUIRE(retried.HasValue());
    CHECK(native.uploads == uploads + 4);
    CHECK(fixture.bindings[0].slot->Revision() == 3);
    CHECK(fixture.bindings[1].slot->Revision() == 2);
    CHECK(capture.valid);
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
    bool ownerClosedByCallback = false;
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
        bool fenceDeferred = false;
        native.onDraw = [&] {
            const auto before = OpenGLBackendTests::commandState.fenceCount;
            ownerClosedByCallback = frontend->BeginShutdown().HasValue();
            fenceDeferred = OpenGLBackendTests::commandState.fenceCount == before;
            const auto pending = frontend->PollDrain();
            CHECK(pending.HasValue());
            CHECK(pending.Value() == RenderBackendDrainProgress::Pending);
        };
        auto begun = frontend->BeginFrame({1, {64, 32}});
        REQUIRE(begun.HasValue());
        auto frame = std::move(begun).Value();
        CHECK(frame.ExecuteGraph(source.execution, source.workloads).HasError());
        CHECK(ownerClosedByCallback);
        CHECK(fenceDeferred);
        CHECK(OpenGLBackendTests::commandState.fenceCount > 0);
    }
    CHECK(native.draws == 1);
    CHECK(native.instances == 3);
    source.workloads.clear();
    fixture.schedule = {};
    fixture.world.reset();
    // A callback-closed owner already dropped its inventory. Other sections still own
    // both the internal slot and the pending backend completion lease at this point.
    CHECK(retained.use_count() == (ownerClosedByCallback ? 1 : 2));
    REQUIRE(frontend->BeginShutdown().HasValue());
    const auto pending = frontend->PollDrain();
    REQUIRE(pending.HasValue());
    CHECK(pending.Value() == RenderBackendDrainProgress::Pending);
    // Caller workload/schedule aliases are gone and BeginShutdown dropped the inventory;
    // only the accepted backend completion lease may retain this exact order now.
    CHECK(retained.use_count() == 1);
    OpenGLBackendTests::commandState.pollStatus = 0x911AU;  // GL_ALREADY_SIGNALED
    Horo::Render::Test::RequireDrainedOwner(*frontend);
    CHECK(retained.expired());  // Terminal shutdown also retires the frontend-owned scheduling inventory.
}

/** @brief Proves an unsubmitted internal order no longer owns native material leases when backend shutdown starts. */
TEST_CASE("VFX shutdown releases unsubmitted internal material leases before native teardown", "[renderer][opengl][vfx][unit][shutdown]") {
    OpenGLResourceTests::ResourcePresentationPort port;
    auto frontend = Create(port);
    std::weak_ptr<const ResidentMaterialBinding> material;
    std::weak_ptr<const VfxDrawOrder> order;
    {
        auto fixture = PrepareVfxDrawFixture(*frontend);
        REQUIRE(frontend->PrepareVfxDraws(fixture.builder, fixture.schedule, fixture.view, fixture.bindings).HasValue());
        material = fixture.bindings[0].slot->Material();
        order = std::get<RenderGraphVfx>(fixture.schedule.records[0].workload).order;
    }
    REQUIRE_FALSE(material.expired());
    REQUIRE_FALSE(order.expired());
    REQUIRE(frontend->BeginShutdown().HasValue());
    CHECK(material.expired());
    CHECK(order.expired());
    Horo::Render::Test::RequireDrainedOwner(*frontend);
}

/** @brief Proves actual frontend graph retention survives shutdown until the injected protocol completion arrives. */
TEST_CASE("VFX shutdown preserves accepted graph material leases until observed completion", "[renderer][opengl][vfx][unit][shutdown]") {
    OpenGLResourceTests::ResourcePresentationPort port;
    auto frontend = Create(port);
    const SignaledCompletionOnExit completionOnExit;
    std::weak_ptr<const ResidentMaterialBinding> material;
    std::weak_ptr<const VfxDrawOrder> order;
    {
        auto fixture = PrepareVfxDrawFixture(*frontend);
        REQUIRE(frontend->PrepareVfxDraws(fixture.builder, fixture.schedule, fixture.view, fixture.bindings).HasValue());
        material = fixture.bindings[0].slot->Material();
        order = std::get<RenderGraphVfx>(fixture.schedule.records[0].workload).order;
        auto source = CompileVfxDrawFixture(fixture);
        OpenGLBackendTests::commandState.pollStatus = 0x911BU;  // Injected GL_TIMEOUT_EXPIRED; protocol evidence only.
        auto begun = frontend->BeginFrame({1, {64, 32}});
        REQUIRE(begun.HasValue());
        auto frame = std::move(begun).Value();
        REQUIRE(frame.ExecuteGraph(source.execution, source.workloads).HasValue());
        REQUIRE(frame.Present().HasValue());
    }
    REQUIRE(frontend->BeginShutdown().HasValue());
    const auto pending = frontend->PollDrain();
    REQUIRE(pending.HasValue());
    CHECK(pending.Value() == RenderBackendDrainProgress::Pending);
    CHECK_FALSE(material.expired());
    CHECK_FALSE(order.expired());
    OpenGLBackendTests::commandState.pollStatus = 0x911AU;  // Injected GL_ALREADY_SIGNALED releases the genuine graph lease.
    Horo::Render::Test::RequireDrainedOwner(*frontend);
    CHECK(material.expired());
    CHECK(order.expired());
}
