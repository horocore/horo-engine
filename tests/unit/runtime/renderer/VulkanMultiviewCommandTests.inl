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
    MultiviewPublication publication{1, 2, 1, 1, 11, 1};

    MultiviewPacket(NativeGraphFixture &fixture, const bool instanced, const bool motion = false,
                    const RenderTextureFormat motionFormat = RenderTextureFormat::Rgba16Float, const std::uint32_t slotBase = 0,
                    std::span<const RenderTextureViewHandle> predecessor = {})
        : native(fixture, true, motion ? motionFormat : RenderTextureFormat::Rgba8Unorm,
                 motion ? RenderTextureUsage::Sampled | RenderTextureUsage::RenderAttachment : RenderTextureUsage::RenderAttachment,
                 slotBase) {
        const RenderBufferHandle uniformHandle{{1}, slotBase + 5, 1};
        const RenderTextureHandle colorHandle{{1}, slotBase + 3, 1};
        const RenderTextureViewHandle firstViewHandle{{1}, slotBase + 4, 1}, secondViewHandle{{1}, slotBase + 6, 1};
        const RenderMeshHandle meshHandle{{1}, slotBase + 2, 1};
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
                                             .resources = {{{1}, 0, MaterialBufferBinding{uniformHandle, 0, parameterBytes}}}};
        for (std::size_t eye = 0; eye < predecessor.size(); ++eye) {
            const auto id = MultiviewShaderParameterIds::PreviousEyeDepthBindings[eye];
            descriptor.layout.resources.push_back(
                {id, ShaderResourceKind::SampledTexture, ShaderResourceAccess::ReadOnly, 1, ShaderStageVisibility::Fragment});
            descriptor.layout.targetBindings.push_back({id, 0, 0, static_cast<std::uint32_t>(eye + 1), "", {}, true});
            descriptor.resources.push_back({id, 0, predecessor[eye]});
        }
        const auto published = table->Publish(descriptor);
        REQUIRE(published.HasValue());
        material = SubmissionTest::Take(table->Acquire(published.Value().binding));
        const std::array sourceViews{MultiviewSceneView{0, Horo::Math::Mat4::Identity()},
                                     MultiviewSceneView{1, Horo::Math::Mat4::Identity()}};
        const std::array instances{MultiviewSceneInstance{meshHandle,
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
            fixture.resources.CreateTextureView({.texture = colorHandle, .format = native.material.state.colorFormat, .baseLayer = 1},
                                                native.texture));
        const RenderTextureHandle depthHandle = motion ? RenderTextureHandle{{1}, slotBase + 7, 1} : RenderTextureHandle{};
        auto prepared = MultiviewTest::Graph(colorHandle, {{1}, slotBase + 1, 1}, {{1}, slotBase + 2, 1}, uniformHandle, depthHandle);
        const auto depth = prepared.depth;
        const auto color = prepared.color, vertex = prepared.vertex, index = prepared.index, uniform = prepared.uniform;
        graph.emplace(std::move(prepared.graph));
        const auto pass = graph->Passes().front().pass;
        resources = {{color, native.texture}, {vertex, native.vertex}, {index, native.index}, {uniform, parameters}};
        meshes = {{{meshHandle, native.mesh}}};
        views = {{firstViewHandle, native.view}, {secondViewHandle, secondView}};
        targets = {{{color, {}, firstViewHandle, {}, {0, 0, 8, 8}, 0}, {color, {}, secondViewHandle, {}, {0, 0, 8, 8}, 1}}};
        if (motion) {
            const RenderTextureDescriptor descriptor{.extent = {8, 8},
                                                     .format = RenderTextureFormat::Depth32Float,
                                                     .layerCount = 2,
                                                     .usage = RenderTextureUsage::Sampled | RenderTextureUsage::RenderAttachment};
            const auto cost = SubmissionTest::Take(fixture.resources.QueryTextureMemoryCost(descriptor));
            const auto texture = SubmissionTest::Take(fixture.resources.CreateTexture(descriptor, {}, TestSupport::PlacementFor(cost, 9)));
            resources.push_back({depth, texture});
            for (std::uint32_t eye = 0; eye < targets.size(); ++eye) {
                const RenderTextureViewHandle handle{{1}, slotBase + 8 + eye, 1};
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

TEST_CASE("Vulkan stores requested XR projection alpha in both acquired eye attachments", "[vulkan][multiview][command][alpha]") {
    bool premultiplied = false;
    SECTION("ordinary opaque projection") {}
    SECTION("premultiplied projection clears to transparent black") {
        premultiplied = true;
    }
    NativeGraphFixture fixture(false);
    MultiviewPacket packet(fixture, false);
    if (premultiplied)
        packet.Workload().clearColor = {0, 0, 0, 0};
    RenderGraphLeaseTransfer transfer;
    REQUIRE(fixture.graph.BeginFrame({1}).HasValue());
    REQUIRE(fixture.graph.ExecuteGraph(packet.Request(fixture, transfer)).HasValue());
    const auto &calls = fixture.encoded.renderingCalls;
    REQUIRE(calls.size() == 2);
    REQUIRE(calls[0].colorView != VK_NULL_HANDLE);
    REQUIRE(calls[1].colorView != VK_NULL_HANDLE);
    CHECK(calls[0].colorView != calls[1].colorView);
    const std::array expected = premultiplied ? std::array{0.0F, 0.0F, 0.0F, 0.0F} : std::array{0.035F, 0.045F, 0.070F, 1.0F};
    for (const auto &call : calls) {
        CHECK(call.colorLoad == VK_ATTACHMENT_LOAD_OP_CLEAR);
        CHECK(call.colorStore == VK_ATTACHMENT_STORE_OP_STORE);
        const auto &rgba = call.colorClear.color.float32;
        const std::array actual{rgba[0], rgba[1], rgba[2], rgba[3]};
        CHECK(actual == expected);
    }
    REQUIRE(fixture.graph.SubmitFrame({1}).HasValue());
    fixture.commandState.state.Complete();
    REQUIRE(fixture.graph.BeginShutdown().HasValue());
    CHECK(fixture.graph.PollDrain().Value() == RenderBackendDrainProgress::Drained);
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

/** @brief Rebinds an admitted packet's logical graph identities for the combined graph without changing native targets. */
void AppendMultiviewPacket(RenderGraphBuilder &builder, MultiviewPacket &packet, const RenderGraphPassRef pass,
                           std::vector<RenderGraphResourceInstance> &resources, std::vector<RenderGraphImportedState> &initial,
                           const MultiviewPacket *sharedGeometry = nullptr) {
    std::vector<RenderGraphResourceId> imported;
    for (const auto &source : packet.graph->Resources()) {
        if (sharedGeometry && (source.id == packet.geometry.front().vertices || source.id == packet.geometry.front().indices)) {
            imported.push_back(source.id == packet.geometry.front().vertices ? sharedGeometry->geometry.front().vertices
                                                                             : sharedGeometry->geometry.front().indices);
            continue;
        }
        const auto id =
            source.kind == RenderGraphResourceKind::Texture
                ? SubmissionTest::Take(builder.ImportTexture(std::get<RenderTextureHandle>(source.binding), source.resourceClass))
                : SubmissionTest::Take(builder.ImportBuffer(std::get<RenderBufferHandle>(source.binding), source.resourceClass));
        imported.push_back(id);
        const auto state = packet.graph->ImportedStates()[source.id.value - 1];
        initial.push_back({id, state.state, state.contents});
        const auto instance = std::ranges::find(packet.resources, source.id, &RenderGraphResourceInstance::resource);
        REQUIRE(instance != packet.resources.end());
        resources.push_back({id, instance->instance});
        if (source.kind == RenderGraphResourceKind::Texture)
            REQUIRE(builder.ExportResource(id).HasValue());
    }
    const auto remap = [&imported](const RenderGraphResourceId id) {
        return id.IsValid() ? imported[id.value - 1] : id;
    };
    for (const auto &use : packet.graph->Usages())
        REQUIRE(builder.AddUsage({pass, remap(use.resource), use.access, use.kind}).HasValue());
    for (auto &target : packet.targets) {
        target.color = remap(target.color);
        target.depth = remap(target.depth);
    }
    for (auto &draw : packet.geometry) {
        draw.vertices = remap(draw.vertices);
        draw.indices = remap(draw.indices);
        draw.parameters = remap(draw.parameters);
    }
}

TEST_CASE("Vulkan projection and asymmetric motion depth use their own prepared attachments in one graphics submission",
          "[vulkan][multiview][motion][command]") {
    bool duplicate = false;
    bool invalidDepth = false;
    bool differentView = false;
    SECTION("projection plus motion and depth") {}
    SECTION("duplicate projection cannot reuse the prepared recorder") {
        duplicate = true;
    }
    SECTION("wrong second eye attachment view rejects both passes before recording") {
        invalidDepth = true;
    }
    SECTION("different current eye projection rejects the pair") {
        differentView = true;
    }

    NativeGraphFixture fixture;
    MultiviewPacket color(fixture, false);
    MultiviewPacket aux(fixture, false, !duplicate, RenderTextureFormat::Rgba16Float, 16);
    aux.targets.front().rectangle = {0, 0, 6, 8};
    aux.targets.back().rectangle = {0, 0, 8, 5};
    if (invalidDepth)
        aux.targets.back().depthView = aux.targets.front().depthView;
    if (differentView) {
        std::array eyes{aux.scene->Views().front(), aux.scene->Views().back()};
        eyes.back().worldToClip.values[12] = 0.125F;
        auto other = MultiviewSceneSnapshot::Extract(aux.publication, aux.publication, eyes, aux.scene->Instances(),
                                                     {32, true, true, false}, MultiviewExecutionPolicy::Multipass);
        REQUIRE(other.HasValue());
        aux.Workload().scene = std::make_shared<const MultiviewSceneSnapshot>(std::move(other).Value());
    }
    auto builder = SubmissionTest::Take(RenderGraphBuilder::Create());
    const auto projection = SubmissionTest::Take(builder.AddPass(RenderPassKind::Graphics, RenderQueueRole::Graphics));
    const auto auxiliary = SubmissionTest::Take(builder.AddPass(RenderPassKind::Graphics, RenderQueueRole::Graphics));
    REQUIRE(builder.AddDependency({projection, auxiliary}).HasValue());
    std::vector<RenderGraphResourceInstance> resources;
    std::vector<RenderGraphImportedState> initial;
    AppendMultiviewPacket(builder, color, projection, resources, initial);
    AppendMultiviewPacket(builder, aux, auxiliary, resources, initial);
    const auto authored = SubmissionTest::Take(builder.Finalize());
    const auto schedule = SubmissionTest::Take(CompileRenderGraph(authored));
    const std::array queues{RenderQueueAssignment{RenderQueueRole::Graphics, {1}}};
    const auto sync = SubmissionTest::Take(SynthesizeRenderGraphSynchronization(authored, schedule, queues, initial));
    const auto graph = SubmissionTest::Take(CompileRenderGraphExecution(authored, schedule, sync, queues));
    const std::array workloads{RenderGraphPassWorkload{projection, color.Workload()}, RenderGraphPassWorkload{auxiliary, aux.Workload()}};
    const std::array meshes{color.meshes.front(), aux.meshes.front()};
    auto views = color.views;
    views.insert(views.end(), aux.views.begin(), aux.views.end());
    const std::array<RenderGraphQueueSubmission, 1> submissions{{{{{1}, {1}, {}, {{1}, 1}}, {0, 2}}}};
    RenderGraphLeaseTransfer transfer;
    REQUIRE(fixture.graph.BeginFrame({1}).HasValue());
    const RenderGraphExecutionRequest request{{1},   graph,     workloads,   resources, &fixture.lease,
                                              false, &transfer, submissions, meshes,    views};
    const auto recorded = fixture.graph.ExecuteGraph(request);
    if (duplicate || invalidDepth || differentView) {
        CHECK(recorded.HasError());
        CHECK(fixture.encoded.renderingCalls.empty());
        CHECK(fixture.commandState.state.beginCalls == 0);
        fixture.graph.AbortFrame({1});
        return;
    }
    INFO((recorded.HasError() ? recorded.ErrorValue().message : "paired graph recorded"));
    REQUIRE(recorded.HasValue());
    const auto &calls = fixture.encoded.renderingCalls;
    REQUIRE(calls.size() == 4);
    CHECK_FALSE(calls[0].depth);
    CHECK_FALSE(calls[1].depth);
    CHECK(calls[0].colorView != calls[1].colorView);
    CHECK(calls[0].colorClear.color.float32[3] == 1.0F);
    CHECK(calls[2].colorView != calls[3].colorView);
    CHECK(calls[0].colorView != calls[2].colorView);
    CHECK(calls[1].colorView != calls[3].colorView);
    CHECK(calls[2].depth);
    CHECK(calls[3].depth);
    CHECK(calls[2].extent.width == 6);
    CHECK(calls[2].extent.height == 8);
    CHECK(calls[3].extent.width == 8);
    CHECK(calls[3].extent.height == 5);
    CHECK(calls[2].colorClear.color.float32[3] == 0.0F);
    CHECK(calls[3].colorClear.color.float32[3] == 0.0F);
    REQUIRE(fixture.encoded.drawCalls.size() == 4);
    CHECK(fixture.encoded.drawCalls[0].firstView == 0);
    CHECK(fixture.encoded.drawCalls[1].firstView == 1);
    CHECK(fixture.encoded.drawCalls[2].firstView == 0);
    CHECK(fixture.encoded.drawCalls[3].firstView == 1);
    REQUIRE(fixture.graph.SubmitFrame({1}).HasValue());
    CHECK(fixture.commandState.state.accepted.size() == 1);
    CHECK(fixture.commandState.state.waitCalls == 0);
    fixture.commandState.state.Complete();
    REQUIRE(fixture.graph.BeginShutdown().HasValue());
    CHECK(fixture.graph.PollDrain().Value() == RenderBackendDrainProgress::Drained);
}

TEST_CASE("Vulkan coverage admits exact completed predecessor depth and ordered stereo descriptors",
          "[vulkan][multiview][coverage][command]") {
    enum class Prior {
        Completed,
        Undefined,
        WrongDescriptor,
        WrongLayer,
    };
    const auto input = GENERATE(Prior::Completed, Prior::Undefined, Prior::WrongDescriptor, Prior::WrongLayer);
    NativeGraphFixture fixture;
    MultiviewPacket predecessor(fixture, false, true, RenderTextureFormat::Rgba16Float, 32);
    RenderGraphLeaseTransfer priorTransfer;
    const auto priorRequest = predecessor.Request(fixture, priorTransfer);
    const auto priorResource = predecessor.targets.front().depth;
    std::vector<Detail::VulkanGraphResource> priorBindings(priorRequest.resources.size());
    Detail::VulkanCommandRetention retained;
    retained.resources.reserve(priorBindings.size());
    REQUIRE(fixture.resources.CaptureGraphResources(priorRequest, priorBindings, retained).HasValue());
    const auto &priorImage = priorBindings[priorResource.value - 1];
    if (input != Prior::Undefined) {
        REQUIRE(fixture.graph.BeginFrame({1}).HasValue());
        REQUIRE(fixture.graph.ExecuteGraph(priorRequest).HasValue());
        REQUIRE(fixture.graph.SubmitFrame({1}).HasValue());
        fixture.commandState.state.Complete();
        REQUIRE(fixture.graph.ObserveSubmission({{{1}, 1}, RenderCpuWaitPurpose::Poll, {}, 11}).Value());
        REQUIRE(priorImage.state->contentsDefined);
    }

    const std::array priorEyes{predecessor.targets.front().depthView, predecessor.targets.back().depthView};
    MultiviewPacket projection(fixture, false);
    MultiviewPacket coverage(fixture, false, true, RenderTextureFormat::R32Float, 16, priorEyes);
    auto instances = projection.scene->Instances().front();
    instances.material = coverage.material->Id();
    const auto snapshot = SubmissionTest::Take(
        MultiviewSceneSnapshot::Extract(projection.publication, projection.publication, projection.scene->Views(), std::span{&instances, 1},
                                        {32, true, true, false}, MultiviewExecutionPolicy::Multipass));
    coverage.Workload().scene = std::make_shared<const MultiviewSceneSnapshot>(snapshot);
    coverage.Workload().output = MultiviewOutput::ReprojectionCoverage;
    coverage.targets.front().rectangle = {1, 1, 5, 6};
    coverage.targets.back().rectangle = {0, 0, 7, 4};

    auto builder = SubmissionTest::Take(RenderGraphBuilder::Create());
    const auto colorPass = SubmissionTest::Take(builder.AddPass(RenderPassKind::Graphics, RenderQueueRole::Graphics));
    const auto coveragePass = SubmissionTest::Take(builder.AddPass(RenderPassKind::Graphics, RenderQueueRole::Graphics));
    REQUIRE(builder.AddDependency({colorPass, coveragePass}).HasValue());
    std::vector<RenderGraphResourceInstance> resources;
    std::vector<RenderGraphImportedState> initial;
    AppendMultiviewPacket(builder, projection, colorPass, resources, initial);
    AppendMultiviewPacket(builder, coverage, coveragePass, resources, initial, &projection);
    const auto priorHandle = std::get<RenderTextureHandle>(predecessor.graph->Resources()[priorResource.value - 1].binding);
    const auto source = SubmissionTest::Take(builder.ImportTexture(priorHandle, RenderGraphResourceClass::Persistent));
    REQUIRE(builder.AddUsage({coveragePass, source, RenderGraphAccess::Read, RenderGraphUsageKind::Sampled}).HasValue());
    const RenderGraphLogicalState priorState{RenderGraphSynchronizationAccess::Write,
                                             RenderGraphSynchronizationOperation::DepthStencilAttachment,
                                             RenderGraphPipelineScope::Graphics,
                                             RenderGraphTextureLayout::DepthStencilAttachment,
                                             {1}};
    initial.push_back({source, priorState});
    const auto nativePrior = std::ranges::find(predecessor.resources, priorResource, &RenderGraphResourceInstance::resource);
    REQUIRE(nativePrior != predecessor.resources.end());
    resources.push_back({source, nativePrior->instance});
    auto views = projection.views;
    views.insert(views.end(), coverage.views.begin(), coverage.views.end());
    for (std::uint32_t eye = 0; eye < priorEyes.size(); ++eye) {
        const auto &eyeView = *std::ranges::find(predecessor.views, priorEyes[eye], &RenderGraphTextureViewInstance::view);
        views.push_back(eyeView);
        auto &target = coverage.targets[eye];
        target.predecessorDepth = source;
        target.predecessorDepthView = eyeView.view;
        target.predecessorDepthLayer = input == Prior::WrongLayer ? 1 - eye : eye;
        const auto native = SubmissionTest::Take(fixture.resources.MaterialCapture().ResolveDrawView(eyeView.instance));
        coverage.native.material.resources.push_back({.view = native.view,
                                                      .image = native.image,
                                                      .state = priorImage.state,
                                                      .backing = retained.resources.back(),
                                                      .binding = MultiviewShaderParameterIds::PreviousEyeDepthBindings[eye]});
    }
    if (input == Prior::WrongDescriptor)
        std::swap(coverage.native.material.resources.front().view, coverage.native.material.resources.back().view);
    const auto authored = SubmissionTest::Take(builder.Finalize());
    const auto schedule = SubmissionTest::Take(CompileRenderGraph(authored));
    const std::array queues{RenderQueueAssignment{RenderQueueRole::Graphics, {1}}};
    const auto sync = SubmissionTest::Take(SynthesizeRenderGraphSynchronization(authored, schedule, queues, initial));
    const auto graph = SubmissionTest::Take(CompileRenderGraphExecution(authored, schedule, sync, queues));
    const std::array workloads{RenderGraphPassWorkload{colorPass, projection.Workload()},
                               RenderGraphPassWorkload{coveragePass, coverage.Workload()}};
    const std::array submissions{RenderGraphQueueSubmission{{{1}, {2}, {}, {{1}, 2}}, {0, 2}}};
    RenderGraphLeaseTransfer transfer;
    const RenderGraphExecutionRequest request{{2},   graph,     workloads,   resources,         &fixture.lease,
                                              false, &transfer, submissions, projection.meshes, views};
    const auto before = fixture.encoded.renderingCalls.size();
    const auto releases = fixture.lease.releases;
    REQUIRE(fixture.graph.BeginFrame({2}).HasValue());
    const auto recorded = fixture.graph.ExecuteGraph(request);
    if (input != Prior::Completed) {
        REQUIRE(recorded.HasError());
        if (input == Prior::Undefined)
            CHECK(recorded.ErrorValue().message.find("Undefined native storage") != std::string::npos);
        if (input == Prior::WrongDescriptor)
            CHECK(recorded.ErrorValue().message.find("exact retained predecessor eye generation") != std::string::npos);
        CHECK(fixture.encoded.renderingCalls.size() == before);
        fixture.graph.AbortFrame({2});
        return;
    }
    INFO((recorded.HasError() ? recorded.ErrorValue().message : "coverage recorded"));
    REQUIRE(recorded.HasValue());
    const auto calls = std::span{fixture.encoded.renderingCalls}.subspan(before);
    REQUIRE(calls.size() == 4);
    CHECK(calls[2].extent.width == 5);
    CHECK(calls[3].extent.height == 4);
    CHECK(calls[2].colorClear.color.float32[0] == 0.0F);
    CHECK(calls[3].colorClear.color.float32[0] == 0.0F);
    CHECK(fixture.encoded.drawCalls[before + 2].firstView == 0);
    CHECK(fixture.encoded.drawCalls[before + 3].firstView == 1);
    REQUIRE(fixture.graph.SubmitFrame({2}).HasValue());
    CHECK(fixture.lease.releases == releases);
    fixture.commandState.state.Complete();
    REQUIRE(fixture.graph.ObserveSubmission({{{1}, 2}, RenderCpuWaitPurpose::Poll, {}, 11}).Value());
    CHECK(fixture.lease.releases == releases + 1);
    CHECK(fixture.commandState.state.waitCalls == 0);
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
        ++packet.Workload().active.frame;
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
