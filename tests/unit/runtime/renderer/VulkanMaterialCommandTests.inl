/** @brief Owns test attachment and mesh backing using the same actual registry as graph commands. */
struct MaterialPacket {
    std::uint64_t vertex{}, index{}, mesh{}, texture{}, view{}, target{};
    Detail::VulkanPreparedMaterialBinding material;

    explicit MaterialPacket(NativeGraphFixture &fixture, const bool layered = false) {
        fixture.resourceState.state.bufferRequirements.size = 256;
        fixture.resourceState.state.imageRequirements.size = 8 * 8 * 4 * (layered ? 2 : 1);
        auto &resources = fixture.resources;
        const std::array<std::byte, sizeof(MeshVertex) * 3> bytes{};
        const auto buffer = [&](const RenderBufferUsage usage) {
            const RenderBufferDescriptor descriptor{bytes.size(), usage, RenderBufferAccess::HostVisible};
            auto plan = resources.QueryBufferMemoryCost(descriptor);
            REQUIRE(plan.HasValue());
            return SubmissionTest::Take(resources.CreateBuffer(descriptor, bytes, TestSupport::PlacementFor(plan.Value(), 1)));
        };
        vertex = buffer(RenderBufferUsage::Vertex);
        index = buffer(RenderBufferUsage::Index);
        const RenderMeshDescriptor descriptor{.vertexBuffer = {{1}, 1, 1},
                                              .indexBuffer = {{1}, 2, 1},
                                              .vertexStride = sizeof(MeshVertex),
                                              .vertexCount = 3,
                                              .indexCount = 3,
                                              .localBounds = {{-1, -1, 0}, {1, 1, 0}}};
        mesh = SubmissionTest::Take(resources.CreateMesh(descriptor, vertex, index));
        const RenderTextureDescriptor image{.extent = {8, 8},
                                            .layerCount = layered ? 2U : 1U,
                                            .usage = RenderTextureUsage::RenderAttachment};
        const auto plan = resources.QueryTextureMemoryCost(image);
        INFO((plan.HasError() ? plan.ErrorValue().message : "attachment memory requirements admitted"));
        REQUIRE(plan.HasValue());
        texture = SubmissionTest::Take(resources.CreateTexture(image, {}, TestSupport::PlacementFor(plan.Value(), 1)));
        view = SubmissionTest::Take(resources.CreateTextureView({.texture = {{1}, 3, 1}}, texture));
        target = SubmissionTest::Take(resources.CreateRenderTarget({.colorAttachment = {{1}, 4, 1}, .extent = {8, 8}}, view, 0));
        material.deviceIncarnation = 11;
        static const Detail::VulkanShaderOwnerIdentity identity{11};
        material.owner = &identity;
        material.pipelineBacking = std::make_shared<int>(1);
        material.pipeline = NativeHandle<VkPipeline>(201);
        material.layout = NativeHandle<VkPipelineLayout>(202);
        material.sets = {NativeHandle<VkDescriptorSet>(203)};
        material.state.depthFormat.reset();
        material.state.depthTest = false;
        material.state.depthWrite = false;
    }
};

TEST_CASE("Vulkan material commands share graph ticket completion and retain retired native backing", "[vulkan][material][command]") {
    NativeGraphFixture fixture;
    MaterialPacket packet(fixture);
    const auto destroyedBuffers = fixture.resourceState.state.destroyBufferCount;
    const auto destroyedImages = fixture.resourceState.state.destroyImageCount;
    const std::array draws{RenderMaterialDrawInstance{packet.mesh, &packet.material}};
    RenderGraphLeaseTransfer transfer;
    const RenderQueueSubmission submission{{1}, {1}, {}, {{1}, 1}};
    REQUIRE(fixture.graph.BeginFrame({1}).HasValue());
    auto result = fixture.graph.ExecuteMaterialPass({{1}, packet.target, {8, 8}, {}, {}, draws, &fixture.lease, &transfer, submission});
    INFO((result.HasError() ? result.ErrorValue().message : "material recorded"));
    REQUIRE(result.HasValue());
    CHECK(fixture.encoded.pipelineBinds == 1);
    CHECK(fixture.encoded.descriptorBinds == 1);
    CHECK(fixture.encoded.vertexBinds == 1);
    CHECK(fixture.encoded.indexBinds == 1);
    CHECK(fixture.encoded.draws == 1);
    CHECK(transfer.authority == RenderGraphLeaseAuthority::BackendCompletion);
    REQUIRE(fixture.graph.SubmitFrame({1}).HasValue());
    fixture.resources.DestroyRenderTarget(packet.target);
    fixture.resources.DestroyTextureView(packet.view);
    fixture.resources.DestroyTexture(packet.texture);
    fixture.resources.DestroyMesh(packet.mesh);
    fixture.resources.DestroyBuffer(packet.vertex);
    fixture.resources.DestroyBuffer(packet.index);
    CHECK(fixture.resourceState.state.destroyBufferCount == destroyedBuffers);
    CHECK(fixture.resourceState.state.destroyImageCount == destroyedImages);
    CHECK(fixture.lease.releases == 0);
    fixture.commandState.state.Complete();
    REQUIRE(fixture.graph.ObserveSubmission({{{1}, 1}, RenderCpuWaitPurpose::Poll, {}, 11}).Value());
    CHECK(fixture.resourceState.state.destroyBufferCount == destroyedBuffers + 2);
    CHECK(fixture.resourceState.state.destroyImageCount == destroyedImages + 1);
    CHECK(fixture.lease.releases == 1);
}

TEST_CASE("Vulkan material admission rejects foreign devices and malformed requests before native recording",
          "[vulkan][material][command]") {
    NativeGraphFixture fixture;
    MaterialPacket packet(fixture);
    std::array draws{RenderMaterialDrawInstance{packet.mesh, &packet.material}};
    RenderGraphLeaseTransfer transfer;
    RenderMaterialExecutionRequest
        request{{1}, packet.target, {8, 8}, {}, {}, draws, &fixture.lease, &transfer, RenderQueueSubmission{{1}, {1}, {}, {{1}, 1}}};
    SECTION("foreign device") {
        packet.material.deviceIncarnation = 12;
    }
    SECTION("missing submission") {
        request.submission.reset();
    }
    SECTION("missing lease") {
        request.lease = nullptr;
    }
    SECTION("stale mesh") {
        draws[0].mesh = 9999;
    }
    SECTION("attachment format") {
        packet.material.state.colorFormat = RenderTextureFormat::Rgba16Float;
    }
    SECTION("undefined attachment load") {
        request.color.loadOperation = AttachmentLoadOperation::Load;
    }
    REQUIRE(fixture.graph.BeginFrame({1}).HasValue());
    REQUIRE(fixture.graph.ExecuteMaterialPass(request).HasError());
    CHECK(fixture.commandState.state.beginCalls == 0);
    CHECK(fixture.encoded.draws == 0);
    CHECK(fixture.lease.releases == 0);
    CHECK(transfer.authority == RenderGraphLeaseAuthority::Frontend);
    fixture.graph.AbortFrame({1});
}

namespace {
    struct ShadowPacket {
        std::uint64_t texture{}, view{}, target{};
        Detail::VulkanPreparedMaterialBinding material;
        Detail::VulkanCommandRetention capture;
        Detail::VulkanMaterialTarget backing;

        ShadowPacket(NativeGraphFixture &fixture, const MaterialPacket &mesh) {
            const RenderTextureDescriptor image{.extent = {8, 8},
                                                .format = RenderTextureFormat::Depth32Float,
                                                .usage = RenderTextureUsage::Sampled | RenderTextureUsage::RenderAttachment};
            auto plan = fixture.resources.QueryTextureMemoryCost(image);
            REQUIRE(plan.HasValue());
            texture = SubmissionTest::Take(fixture.resources.CreateTexture(image, {}, TestSupport::PlacementFor(plan.Value(), 1)));
            view = SubmissionTest::Take(fixture.resources.CreateTextureView({.texture = {{1}, 3, 1},
                                                                             .format = RenderTextureFormat::Depth32Float,
                                                                             .aspect = RenderTextureAspect::Depth},
                                                                            texture));
            target =
                SubmissionTest::Take(fixture.resources.CreateRenderTarget({.depthAttachment = {{1}, 4, 1}, .extent = {8, 8}}, 0, view));
            material.deviceIncarnation = mesh.material.deviceIncarnation;
            material.owner = mesh.material.owner;
            material.pipelineBacking = mesh.material.pipelineBacking;
            material.pipeline = mesh.material.pipeline;
            material.layout = mesh.material.layout;
            material.state.depthOnly = true;
            material.state.depthFormat = RenderTextureFormat::Depth32Float;
            capture.resources.reserve(1);
            auto captured = fixture.resources.MaterialCapture().CaptureMaterialTarget(target, capture);
            REQUIRE(captured.HasValue());
            backing = captured.Value();
        }

        void SampleWith(Detail::VulkanPreparedMaterialBinding &lit) {
            lit.resources.push_back({.view = backing.depthView,
                                     .image = backing.depth.image,
                                     .textureDescriptor = backing.depth.textureDescriptor,
                                     .state = backing.depth.state,
                                     .backing = capture.resources.front()});
        }
    };
}  // namespace

TEST_CASE("Vulkan material batch records depth atlas tiles before sampling with one submission lease", "[vulkan][shadow][material]") {
    NativeGraphFixture fixture;
    MaterialPacket packet(fixture);
    ShadowPacket shadow(fixture, packet);
    shadow.SampleWith(packet.material);
    const std::array shadowDraws{RenderMaterialDrawInstance{packet.mesh, &shadow.material}};
    const std::array litDraws{RenderMaterialDrawInstance{packet.mesh, &packet.material}};
    std::array<RenderMaterialExecutionRequest, 2> preceding;
    preceding[0] = {.frame = {1},
                    .target = shadow.target,
                    .extent = {8, 8},
                    .depthClear = 1.0F,
                    .draws = shadowDraws,
                    .depthOnly = true,
                    .region = RenderMaterialRegion{1, 1, 2, 6}};
    preceding[1] = {.frame = {1},
                    .target = shadow.target,
                    .extent = {8, 8},
                    .depthClear = std::nullopt,
                    .draws = shadowDraws,
                    .depthOnly = true,
                    .region = RenderMaterialRegion{5, 1, 2, 6}};
    RenderGraphLeaseTransfer transfer;
    RenderMaterialExecutionRequest request{.frame = {1},
                                           .target = packet.target,
                                           .extent = {8, 8},
                                           .draws = litDraws,
                                           .lease = &fixture.lease,
                                           .transfer = &transfer,
                                           .submission = RenderQueueSubmission{{1}, {1}, {}, {{1}, 1}},
                                           .precedingPasses = preceding};
    REQUIRE(fixture.graph.BeginFrame({1}).HasValue());
    auto recorded = fixture.graph.ExecuteMaterialPass(request);
    INFO((recorded.HasError() ? recorded.ErrorValue().message : "atlas and lit recorded"));
    REQUIRE(recorded.HasValue());
    REQUIRE(fixture.encoded.renderingCalls.size() == 3);
    CHECK(fixture.encoded.renderingCalls[0].colors == 0);
    CHECK(fixture.encoded.renderingCalls[0].load == VK_ATTACHMENT_LOAD_OP_CLEAR);
    CHECK(fixture.encoded.renderingCalls[0].extent.width == 8);
    CHECK(fixture.encoded.renderingCalls[1].load == VK_ATTACHMENT_LOAD_OP_LOAD);
    CHECK(fixture.encoded.renderingCalls[2].colors == 1);
    CHECK(fixture.encoded.viewports[0].x == 1.0F);
    CHECK(fixture.encoded.viewports[1].x == 5.0F);
    CHECK(fixture.encoded.scissors[1].extent.width == 2);
    CHECK(std::ranges::any_of(fixture.encoded.imageBarriers, [](const auto &barrier) {
        return barrier.oldLayout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL &&
               barrier.newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }));
    CHECK(fixture.encoded.draws == 3);
    CHECK(transfer.authority == RenderGraphLeaseAuthority::BackendCompletion);
    REQUIRE(fixture.graph.SubmitFrame({1}).HasValue());
    CHECK(fixture.lease.releases == 0);
    fixture.commandState.state.Complete();
    REQUIRE(fixture.graph.ObserveSubmission({{{1}, 1}, RenderCpuWaitPurpose::Poll, {}, 11}).Value());
    CHECK(fixture.lease.releases == 1);
}

TEST_CASE("Vulkan material batch rejects depth sampler feedback and out of bounds atlas regions before recording",
          "[vulkan][shadow][material]") {
    NativeGraphFixture fixture;
    MaterialPacket packet(fixture);
    ShadowPacket shadow(fixture, packet);
    const std::array shadowDraws{RenderMaterialDrawInstance{packet.mesh, &shadow.material}};
    const std::array litDraws{RenderMaterialDrawInstance{packet.mesh, &packet.material}};
    std::array<RenderMaterialExecutionRequest, 1> preceding{{{.frame = {1},
                                                              .target = shadow.target,
                                                              .extent = {8, 8},
                                                              .depthClear = 1.0F,
                                                              .draws = shadowDraws,
                                                              .depthOnly = true,
                                                              .region = RenderMaterialRegion{1, 1, 2, 6}}}};
    SECTION("same pass feedback") {
        shadow.SampleWith(shadow.material);
    }
    SECTION("out of bounds tile") {
        preceding[0].region->x = 8;
    }
    RenderGraphLeaseTransfer transfer;
    const RenderMaterialExecutionRequest request{.frame = {1},
                                                 .target = packet.target,
                                                 .extent = {8, 8},
                                                 .draws = litDraws,
                                                 .lease = &fixture.lease,
                                                 .transfer = &transfer,
                                                 .submission = RenderQueueSubmission{{1}, {1}, {}, {{1}, 1}},
                                                 .precedingPasses = preceding};
    REQUIRE(fixture.graph.BeginFrame({1}).HasValue());
    CHECK(fixture.graph.ExecuteMaterialPass(request).HasError());
    CHECK(fixture.encoded.renderingBegins == 0);
    CHECK(fixture.commandState.state.beginCalls == 0);
    CHECK(transfer.authority == RenderGraphLeaseAuthority::Frontend);
    fixture.graph.AbortFrame({1});
}

TEST_CASE("Vulkan material batch rejects foreign preceding frame before any native recording", "[vulkan][shadow][material]") {
    NativeGraphFixture fixture;
    MaterialPacket packet(fixture);
    ShadowPacket shadow(fixture, packet);
    const std::array shadowDraws{RenderMaterialDrawInstance{packet.mesh, &shadow.material}};
    const std::array litDraws{RenderMaterialDrawInstance{packet.mesh, &packet.material}};
    const std::array preceding{RenderMaterialExecutionRequest{.frame = {2},
                                                              .target = shadow.target,
                                                              .extent = {8, 8},
                                                              .depthClear = 1.0F,
                                                              .draws = shadowDraws,
                                                              .depthOnly = true}};
    RenderGraphLeaseTransfer transfer;
    const RenderMaterialExecutionRequest request{.frame = {1},
                                                 .target = packet.target,
                                                 .extent = {8, 8},
                                                 .draws = litDraws,
                                                 .lease = &fixture.lease,
                                                 .transfer = &transfer,
                                                 .submission = RenderQueueSubmission{{1}, {1}, {}, {{1}, 1}},
                                                 .precedingPasses = preceding};
    REQUIRE(fixture.graph.BeginFrame({1}).HasValue());
    CHECK(fixture.graph.ExecuteMaterialPass(request).HasError());
    CHECK(fixture.encoded.renderingBegins == 0);
    CHECK(fixture.encoded.draws == 0);
    CHECK(fixture.commandState.state.beginCalls == 0);
    CHECK(transfer.authority == RenderGraphLeaseAuthority::Frontend);
    fixture.graph.AbortFrame({1});
}
