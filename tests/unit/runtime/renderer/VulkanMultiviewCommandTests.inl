/** @brief Deterministic adapter borrows a real prepared native packet for canonical table-issued generations. */
struct MultiviewTestBackend final : IMaterialBindingBackend {
    struct Lease final : IResidentMaterialBinding {
        const IResidentMaterialBinding *native;

        explicit Lease(const IResidentMaterialBinding &value) : native(&value) {}

        const IResidentMaterialBinding &NativeBinding() const noexcept override {
            return *native;
        }
    };

    const IResidentMaterialBinding *native{};

    Result<std::unique_ptr<IResidentMaterialBinding>> Realize(const MaterialBindingDescriptor &) override {
        return Result<std::unique_ptr<IResidentMaterialBinding>>::Success(std::make_unique<Lease>(*native));
    }
};

/** @brief Actual native graph/resource fixture with two ordered views and one table-issued material generation. */
struct MultiviewPacket {
    MaterialPacket native;
    MultiviewTestBackend backend;
    std::unique_ptr<MaterialBindingTable> table;
    std::shared_ptr<const ResidentMaterialBinding> material;
    std::shared_ptr<const MultiviewSceneSnapshot> scene;
    std::optional<CompiledRenderGraphExecution> graph;
    std::array<RenderGraphResourceInstance, 4> resources{};
    std::array<RenderGraphMeshInstance, 1> meshes{};
    std::array<RenderGraphTextureViewInstance, 2> views{};
    std::array<MultiviewGraphTarget, 2> targets{};
    std::array<MultiviewGraphGeometry, 1> geometry{};
    std::array<RenderGraphPassWorkload, 1> workloads{};
    std::array<RenderGraphQueueSubmission, 1> submissions{{{{{1}, {1}, {}, {{1}, 1}}, {0, 1}}}};
    std::uint64_t parameters{}, secondView{};
    MultiviewPublication publication{1, 1, 1, 1, 11, 1};

    MultiviewPacket(NativeGraphFixture &fixture, const bool instanced) : native(fixture, true) {
        native.material.device = NativeHandle<VkDevice>(1);
        native.material.state.viewRouting = instanced ? RenderViewRouting::VertexLayer : RenderViewRouting::SingleView;
        backend.native = &native.material;
        auto created = MaterialBindingTable::Create({1}, backend);
        REQUIRE(created.HasValue());
        table = std::move(created).Value();
        MaterialBindingDescriptor descriptor{.material = {1},
                                             .revision = 1,
                                             .pipeline = {{1}, 10, 1},
                                             .layout = MultiviewTest::Layout(),
                                             .resources = {{{1}, 0, MaterialBufferBinding{{{1}, 5, 1}, 0, 224}}}};
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
        std::array<std::byte, 224> data{};
        const RenderBufferDescriptor buffer{224, RenderBufferUsage::Uniform, RenderBufferAccess::HostVisible};
        const auto cost = fixture.resources.QueryBufferMemoryCost(buffer);
        REQUIRE(cost.HasValue());
        parameters = SubmissionTest::Take(fixture.resources.CreateBuffer(buffer, data, TestSupport::PlacementFor(cost.Value(), 8)));
        secondView = SubmissionTest::Take(fixture.resources.CreateTextureView({.texture = {{1}, 3, 1}, .baseLayer = 1}, native.texture));
        auto prepared = MultiviewTest::Graph({{1}, 3, 1}, {{1}, 1, 1}, {{1}, 2, 1}, {{1}, 5, 1});
        const auto color = prepared.color, vertex = prepared.vertex, index = prepared.index, uniform = prepared.uniform;
        graph.emplace(std::move(prepared.graph));
        const auto pass = graph->Passes().front().pass;
        resources = {{{color, native.texture}, {vertex, native.vertex}, {index, native.index}, {uniform, parameters}}};
        meshes = {{{{{1}, 2, 1}, native.mesh}}};
        views = {{{{{1}, 4, 1}, native.view}, {{{1}, 6, 1}, secondView}}};
        targets = {{{color, {}, {{1}, 4, 1}, {}, {0, 0, 8, 8}, 0}, {color, {}, {{1}, 6, 1}, {}, {0, 0, 8, 8}, 1}}};
        geometry = {{{0, vertex, index, uniform, material}}};
        workloads = {{{pass, RenderGraphMultiview{scene, publication, targets, geometry}}}};
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
    INFO(result.HasError() ? result.ErrorValue().message : "multiview recorded");
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
