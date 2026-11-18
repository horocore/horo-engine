/** @brief Actual native graph/resource fixture with two ordered views and one table-issued material generation. */
struct MultiviewPacket {
    MaterialPacket native;
    MultiviewTest::BorrowedMaterialBackend backend;
    std::unique_ptr<MaterialBindingTable> table;
    std::shared_ptr<const ResidentMaterialBinding> material;
    std::shared_ptr<const MultiviewSceneSnapshot> scene;
    std::optional<CompiledRenderGraphExecution> graph;
    std::vector<RenderGraphResourceInstance> resources;
    std::array<RenderGraphMeshInstance, 1> meshes{};
    std::vector<RenderGraphTextureViewInstance> views;
    std::array<MultiviewGraphTarget, 2> targets{};
    std::array<MultiviewGraphGeometry, 1> geometry{};
    std::array<RenderGraphPassWorkload, 1> workloads{};
    std::array<RenderGraphQueueSubmission, 1> submissions{{{{{1}, {1}, {}, {{1}, 1}}, {0, 1}}}};
    std::uint64_t parameters{}, secondView{};
    MultiviewPublication publication{1, 1, 1, 1, 11, 1};

    MultiviewPacket(NativeGraphFixture &fixture, const bool instanced, const bool motion = false,
                    const RenderTextureFormat motionFormat = RenderTextureFormat::Rgba16Float)
        : native(fixture, true, motion ? motionFormat : RenderTextureFormat::Rgba8Unorm,
                 motion ? RenderTextureUsage::Sampled | RenderTextureUsage::RenderAttachment : RenderTextureUsage::RenderAttachment) {
        if (motion)
            ++publication.frame;
        const std::size_t parameterBytes = motion ? 256 : 224;
        auto layout = MultiviewTest::Layout();
        if (motion) {
            using namespace MultiviewShaderParameterIds;
            layout.parameters = {{CurrentLocalToClip, Buffer, ShaderValueType::Float32, 4, 4, 2, 0, 64, 16, true, true},
                                 {PreviousLocalToClip, Buffer, ShaderValueType::Float32, 4, 4, 2, 128, 64, 16, true, true}};
            native.material.state.depthFormat = RenderTextureFormat::Depth32Float;
            native.material.state.depthTest = true;
            native.material.state.depthWrite = true;
            native.material.state.depthCompare = RenderCompareFunction::LessEqual;
        }
        native.material.device = NativeHandle<VkDevice>(1);
        native.material.state.viewRouting = instanced ? RenderViewRouting::VertexLayer : RenderViewRouting::SingleView;
        backend.native = &native.material;
        auto created = MaterialBindingTable::Create({1}, backend);
        REQUIRE(created.HasValue());
        table = std::move(created).Value();
        MaterialBindingDescriptor descriptor{.material = {1},
                                             .revision = 1,
                                             .pipeline = {{1}, 10, 1},
                                             .layout = std::move(layout),
                                             .resources = {{{1}, 0, MaterialBufferBinding{{{1}, 5, 1}, 0, parameterBytes}}}};
        const auto published = table->Publish(descriptor);
        REQUIRE(published.HasValue());
        material = SubmissionTest::Take(table->Acquire(published.Value().binding));
        const std::array sourceViews{MultiviewSceneView{0, Horo::Math::Mat4::Identity()},
                                     MultiviewSceneView{1, Horo::Math::Mat4::Identity()}};
        const std::array instances{MultiviewSceneInstance{{{1}, 2, 1},
                                                          material->Id(),
                                                          Horo::Math::Mat4::Identity(),
                                                          {{-0.25F, -0.25F, 0.25F}, {0.25F, 0.25F, 0.75F}}}};
        auto snapshot =
            MultiviewSceneSnapshot::Extract(publication, publication, sourceViews, instances, {32, true, true, instanced},
                                            instanced ? MultiviewExecutionPolicy::RequireInstanced : MultiviewExecutionPolicy::Multipass);
        REQUIRE(snapshot.HasValue());
        scene = std::make_shared<const MultiviewSceneSnapshot>(snapshot.Value());
        std::vector<std::byte> data(parameterBytes);
        if (motion) {
            auto priorPublication = publication;
            --priorPublication.frame;
            auto priorInstances = instances;
            priorInstances.front().localToWorld.values[12] = 0.125F;
            const auto prior = MultiviewSceneSnapshot::Extract(priorPublication, priorPublication, sourceViews, priorInstances,
                                                               {32, true, true, false}, MultiviewExecutionPolicy::Multipass);
            REQUIRE(prior.HasValue());
            const std::array ids{RenderMotionObjectId{1}};
            const MultiviewMotionFrame priorFrame{priorPublication, 1, 1000}, currentFrame{publication, 2, 2000};
            MultiviewMotionHistory history;
            REQUIRE(history.Stage(priorFrame, prior.Value(), ids));
            REQUIRE(history.Publish(priorFrame));
            REQUIRE(history.Stage(currentFrame, *scene, ids));
            REQUIRE(PackMultiviewMotionShaderParameters(material->Descriptor().layout, history, currentFrame, 0, data).HasValue());
        }
        const RenderBufferDescriptor buffer{parameterBytes, RenderBufferUsage::Uniform, RenderBufferAccess::HostVisible};
        const auto cost = fixture.resources.QueryBufferMemoryCost(buffer);
        REQUIRE(cost.HasValue());
        parameters = SubmissionTest::Take(fixture.resources.CreateBuffer(buffer, data, TestSupport::PlacementFor(cost.Value(), 8)));
        secondView = SubmissionTest::Take(
            fixture.resources.CreateTextureView({.texture = {{1}, 3, 1}, .format = native.material.state.colorFormat, .baseLayer = 1},
                                                native.texture));
        const RenderTextureHandle depthHandle = motion ? RenderTextureHandle{{1}, 7, 1} : RenderTextureHandle{};
        auto prepared = MultiviewTest::Graph({{1}, 3, 1}, {{1}, 1, 1}, {{1}, 2, 1}, {{1}, 5, 1}, depthHandle);
        const auto depth = prepared.depth;
        const auto color = prepared.color, vertex = prepared.vertex, index = prepared.index, uniform = prepared.uniform;
        graph.emplace(std::move(prepared.graph));
        const auto pass = graph->Passes().front().pass;
        resources = {{color, native.texture}, {vertex, native.vertex}, {index, native.index}, {uniform, parameters}};
        meshes = {{{{{1}, 2, 1}, native.mesh}}};
        views = {{{{1}, 4, 1}, native.view}, {{{1}, 6, 1}, secondView}};
        targets = {{{color, {}, {{1}, 4, 1}, {}, {0, 0, 8, 8}, 0}, {color, {}, {{1}, 6, 1}, {}, {0, 0, 8, 8}, 1}}};
        if (motion) {
            const RenderTextureDescriptor descriptor{.extent = {8, 8},
                                                     .format = RenderTextureFormat::Depth32Float,
                                                     .layerCount = 2,
                                                     .usage = RenderTextureUsage::Sampled | RenderTextureUsage::RenderAttachment};
            const auto cost = SubmissionTest::Take(fixture.resources.QueryTextureMemoryCost(descriptor));
            const auto texture = SubmissionTest::Take(fixture.resources.CreateTexture(descriptor, {}, TestSupport::PlacementFor(cost, 9)));
            resources.push_back({depth, texture});
            for (std::uint32_t eye = 0; eye < targets.size(); ++eye) {
                const RenderTextureViewHandle handle{{1}, 8 + eye, 1};
                const auto nativeView = SubmissionTest::Take(fixture.resources.CreateTextureView({.texture = depthHandle,
                                                                                                  .format = descriptor.format,
                                                                                                  .aspect = RenderTextureAspect::Depth,
                                                                                                  .baseLayer = eye},
                                                                                                 texture));
                views.push_back({handle, nativeView});
                targets[eye].depth = depth;
                targets[eye].depthView = handle;
                targets[eye].depthArrayLayer = eye;
            }
        }
        geometry = {{{0, vertex, index, uniform, material}}};
        workloads = {{{pass, RenderGraphMultiview{scene, publication, targets, geometry}}}};
        if (motion) {
            Workload().output = MultiviewOutput::MotionDepth;
            Workload().clearColor = {0, 0, 0, 0};
        }
    }

    RenderGraphMultiview &Workload() {
        return std::get<RenderGraphMultiview>(workloads.front().workload);
    }

    RenderGraphExecutionRequest Request(NativeGraphFixture &fixture, RenderGraphLeaseTransfer &transfer) {
        return {{1}, *graph, workloads, resources, &fixture.lease, false, &transfer, submissions, meshes, views};
    }
};

TEST_CASE("Vulkan multiview emits real indexed multipass and instanced view indices", "[vulkan][multiview][command]") {
    bool instanced = false;
    SECTION("multipass") {}
    SECTION("instanced") {
        instanced = true;
    }
    NativeGraphFixture fixture(instanced);
    MultiviewPacket packet(fixture, instanced);
    RenderGraphLeaseTransfer transfer;
    REQUIRE(fixture.graph.BeginFrame({1}).HasValue());
    const auto result = fixture.graph.ExecuteGraph(packet.Request(fixture, transfer));
    INFO((result.HasError() ? result.ErrorValue().message : "multiview recorded"));
    REQUIRE(result.HasValue());
    REQUIRE(fixture.encoded.drawCalls.size() == (instanced ? 1 : 2));
    CHECK(fixture.encoded.drawCalls[0].firstView == 0);
    CHECK(fixture.encoded.drawCalls[0].instances == (instanced ? 2 : 1));
    if (!instanced)
        CHECK(fixture.encoded.drawCalls[1].firstView == 1);
    CHECK(fixture.encoded.renderingBegins == (instanced ? 1 : 2));
    CHECK(fixture.encoded.layers == (instanced ? 2 : 1));
    REQUIRE(fixture.graph.SubmitFrame({1}).HasValue());
    fixture.resources.DestroyMesh(packet.native.mesh);
    fixture.resources.DestroyBuffer(packet.native.vertex);
    fixture.resources.DestroyBuffer(packet.native.index);
    CHECK(fixture.lease.releases == 0);
    REQUIRE(fixture.graph.BeginShutdown().HasValue());
    CHECK(fixture.graph.PollDrain().Value() == RenderBackendDrainProgress::Pending);
    fixture.commandState.state.Complete();
    CHECK(fixture.graph.PollDrain().Value() == RenderBackendDrainProgress::Drained);
    CHECK(fixture.lease.releases == 1);
}

TEST_CASE("Vulkan motion graph records paired floating and depth attachments only with exact forward state",
          "[vulkan][multiview][motion][command]") {
    auto format = RenderTextureFormat::Rgba16Float;
    bool admitted = true;
    bool changeClear = false;
    bool changeWrite = false;
    SECTION("motion plus matching depth") {}
    SECTION("ordinary color format") {
        format = RenderTextureFormat::Rgba8Unorm;
        admitted = false;
    }
    SECTION("clear cannot imply valid motion") {
        changeClear = true;
        admitted = false;
    }
    SECTION("pipeline must write matching depth") {
        changeWrite = true;
        admitted = false;
    }
    NativeGraphFixture fixture;
    MultiviewPacket packet(fixture, false, true, format);
    if (changeClear)
        packet.Workload().clearColor.alpha = 1.0F;
    if (changeWrite)
        packet.native.material.state.depthWrite = false;
    RenderGraphLeaseTransfer transfer;
    REQUIRE(fixture.graph.BeginFrame({1}).HasValue());
    const auto recorded = fixture.graph.ExecuteGraph(packet.Request(fixture, transfer));
    INFO((recorded.HasError() ? recorded.ErrorValue().message : "motion recorded"));
    CHECK(recorded.HasValue() == admitted);
    CHECK(fixture.encoded.drawCalls.size() == (admitted ? 2 : 0));
    CHECK(fixture.encoded.renderingBegins == (admitted ? 2 : 0));
    if (admitted)
        REQUIRE(fixture.graph.SubmitFrame({1}).HasValue());
    else
        fixture.graph.AbortFrame({1});
}

TEST_CASE("Vulkan multiview rejects late invalid views materials work bounds and stale owners before any native recording",
          "[vulkan][multiview][command]") {
    NativeGraphFixture fixture;
    MultiviewPacket packet(fixture, false);
    SECTION("invalid last layer") {
        packet.targets.back().arrayLayer = 2;
    }
    SECTION("wrong view generation") {
        packet.views.back().instance = 9999;
    }
    SECTION("material device") {
        packet.native.material.deviceIncarnation = 12;
    }
    SECTION("routing contract") {
        packet.native.material.state.viewRouting = RenderViewRouting::VertexLayer;
    }
    SECTION("primitive capacity") {
        packet.Workload().limits.maximumExpandedIndices = 5;
    }
    SECTION("publication replacement") {
        packet.Workload().active.frame = 2;
    }
    SECTION("graph input mapping") {
        packet.resources[1].instance = packet.native.index;
    }
    SECTION("wrong uniform graph dependency") {
        // Exact parameter dependencies may not silently refer to a different logical graph buffer.
        packet.geometry.front().parameters = packet.geometry.front().indices;
    }
    RenderGraphLeaseTransfer transfer;
    REQUIRE(fixture.graph.BeginFrame({1}).HasValue());
    CHECK(fixture.graph.ExecuteGraph(packet.Request(fixture, transfer)).HasError());
    CHECK(fixture.encoded.draws == 0);
    CHECK(fixture.encoded.renderingBegins == 0);
    CHECK(fixture.commandState.state.beginCalls == 0);
    CHECK(fixture.commandState.state.accepted.empty());
    fixture.graph.AbortFrame({1});
}

TEST_CASE("Vulkan multiview publishes whole contents only for complete successfully submitted writes",
          "[vulkan][multiview][contents][rollback]") {
    NativeGraphFixture fixture;
    MultiviewPacket packet(fixture, false);
    bool partial = false;
    bool abort = false;
    bool failSubmit = false;
    SECTION("complete write") {}
    SECTION("partial rectangle stays undefined") {
        partial = true;
        packet.targets.back().rectangle.width = 4;
    }
    SECTION("aborted complete write stays undefined") {
        abort = true;
    }
    SECTION("failed complete submit stays undefined") {
        failSubmit = true;
    }
    RenderGraphLeaseTransfer transfer;
    auto request = packet.Request(fixture, transfer);
    std::array<Detail::VulkanGraphResource, 4> captured{};
    Detail::VulkanCommandRetention observation;
    observation.resources.reserve(4);
    REQUIRE(fixture.resources.CaptureGraphResources(request, captured, observation).HasValue());
    const auto *state = captured.front().state;
    REQUIRE(state);
    REQUIRE_FALSE(state->contentsDefined);
    REQUIRE(fixture.graph.BeginFrame({1}).HasValue());
    REQUIRE(fixture.graph.ExecuteGraph(request).HasValue());
    CHECK_FALSE(state->contentsDefined);  // Recording alone never publishes planned attachment contents.
    if (abort) {
        fixture.graph.AbortFrame({1});
    } else {
        if (failSubmit)
            fixture.commandState.state.submitResult = VK_ERROR_OUT_OF_HOST_MEMORY;
        const auto submitted = fixture.graph.SubmitFrame({1});
        CHECK(submitted.HasValue() == !failSubmit);
    }
    CHECK(state->contentsDefined == (!partial && !abort && !failSubmit));
    CHECK(fixture.commandState.state.waitCalls == 0);
}
