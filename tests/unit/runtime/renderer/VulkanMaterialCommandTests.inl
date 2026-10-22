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
