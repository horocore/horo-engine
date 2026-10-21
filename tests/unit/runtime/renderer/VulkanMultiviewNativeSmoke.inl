namespace {
    /** @brief Reads only the shipped block's final SPIR-V packing; no source-language matrix-order assumption. */
    MaterialBindingLayout NativeMultiviewLayout(const std::vector<std::uint8_t> &bytes) {
        REQUIRE(bytes.size() % 4 == 0);
        std::vector<std::uint32_t> words(bytes.size() / 4);
        std::memcpy(words.data(), bytes.data(), bytes.size());
        REQUIRE(words.size() >= 5);
        std::uint32_t block{};
        std::uint32_t matrixTimesVector{}, vectorTimesMatrix{};
        for (std::size_t offset = 5; offset < words.size();) {
            const auto count = words[offset] >> 16;
            REQUIRE(count > 0);
            REQUIRE(count <= words.size() - offset);
            // SPIR-V opcodes 145/144 are matrix*column-vector and row-vector*matrix respectively.
            // Final-ABI typed matrix values must not silently be consumed with transposed math.
            const auto opcode = words[offset] & 0xffffU;
            if (opcode == 145)
                ++matrixTimesVector;
            if (opcode == 144)
                ++vectorTimesMatrix;
            if (opcode == 6 && count >= 4 && words[offset + 2] == 0) {
                const char *name = reinterpret_cast<const char *>(words.data() + offset + 3);
                const auto *end = static_cast<const char *>(std::memchr(name, 0, (count - 3) * 4));
                REQUIRE(end != nullptr);
                if (std::string_view(name, static_cast<std::size_t>(end - name)) == "WorldToClip")
                    block = words[offset + 1];
            }
            offset += count;
        }
        REQUIRE(block != 0);
        CHECK(matrixTimesVector == 2);
        CHECK(vectorTimesMatrix == 0);
        auto layout = MultiviewTest::Layout();
        layout.parameters[0].arrayCount = 32;
        layout.parameters[2].arrayCount = 32;
        std::array<std::uint32_t, 3> types{};
        std::array<bool, 3> offsets{};
        for (std::size_t offset = 5; offset < words.size(); offset += words[offset] >> 16) {
            const auto count = words[offset] >> 16, opcode = words[offset] & 0xffffU;
            if (opcode == 30 && count == 5 && words[offset + 1] == block)
                std::copy_n(words.data() + offset + 2, 3, types.begin());
            if (opcode != 72 || count < 4 || words[offset + 1] != block || words[offset + 2] >= 3)
                continue;
            auto &parameter = layout.parameters[words[offset + 2]];
            const auto decoration = words[offset + 3];
            if (decoration == 4 || decoration == 5)
                parameter.columnMajor = decoration == 5;
            if (count < 5)
                continue;
            if (decoration == 35) {
                parameter.byteOffset = words[offset + 4];
                offsets[words[offset + 2]] = true;
            }
            if (decoration == 7)
                parameter.matrixStride = words[offset + 4];
        }
        for (std::size_t offset = 5; offset < words.size(); offset += words[offset] >> 16) {
            const auto count = words[offset] >> 16, opcode = words[offset] & 0xffffU;
            if (opcode != 71 || count < 4 || words[offset + 2] != 6)
                continue;
            for (std::size_t index = 0; index < types.size(); ++index)
                if (types[index] == words[offset + 1])
                    layout.parameters[index].arrayStride = words[offset + 3];
        }
        REQUIRE(std::ranges::all_of(offsets, [](bool value) {
            return value;
        }));
        return layout;
    }

    struct NativeMultiviewMaterial final : IMaterialBindingBackend {
        IRenderBackend *backend{};
        std::uint64_t pipeline{}, buffer{};
        std::size_t bytes{};

        Result<std::unique_ptr<IResidentMaterialBinding>> Realize(const MaterialBindingDescriptor &descriptor) override {
            const std::array resources{MaterialResidentResource{MaterialResidentResourceKind::Buffer, buffer, 0, bytes}};
            return backend->RealizeResolved(descriptor, pipeline, resources);
        }
    };
}  // namespace

TEST_CASE("Vulkan native device executes the shipped view-indexed shader in multipass and layered stereo",
          "[vulkan][multiview][native-smoke]") {
    const char *vertexPath = std::getenv("HORO_MULTIVIEW_VERTEX");
    const char *layerPath = std::getenv("HORO_MULTIVIEW_LAYER_VERTEX");
    const char *fragmentPath = std::getenv("HORO_MULTIVIEW_FRAGMENT");
    if (!vertexPath || !layerPath || !fragmentPath)
        SKIP("Cook the shipped multiview shader for native qualification.");
    bool instanced = false;
    SECTION("multipass") {}
    SECTION("instanced") {
        instanced = true;
    }
    NativeTransformFixture fixture;
    fixture.Initialize();
    const auto capabilities = ResolveMultiviewCapabilities(fixture.backend->Capabilities().support);
    REQUIRE(capabilities.multipass);
    REQUIRE(capabilities.indexedShaders);
    if (instanced)
        REQUIRE(capabilities.instancedTargetRouting);
    const auto vertexBytes = ReadShader(instanced ? layerPath : vertexPath);
    const auto fragmentBytes = ReadShader(fragmentPath);
    auto layout = NativeMultiviewLayout(vertexBytes);
    fixture.metadata.matrixParameters = true;
    fixture.metadata.manifest.sourceIdentity = "assets/shaders/runtime/multiview_mesh.hlsl";
    fixture.metadata.manifest.entryPoints = {{ShaderStage::Vertex, "VSMain"}, {ShaderStage::Fragment, "PSMain"}};
    fixture.metadata.manifest.bindings = {
        {{1}, ShaderResourceKind::UniformBuffer, ShaderResourceAccess::ReadOnly, 1, ShaderStageVisibility::Vertex}};
    fixture.metadata.manifest.parameters = {{{1}, {1}, ShaderValueType::Float32, 4, 4, 32},
                                            {{2}, {1}, ShaderValueType::Float32, 4, 4, 1},
                                            {{3}, {1}, ShaderValueType::Uint32, 1, 1, 32}};
    fixture.metadata.manifest.specializationInputs.clear();
    ShaderReflectionCandidate candidate{.backend = ShaderTargetBackend::Vulkan,
                                        .interfaceSchemaVersion = 1,
                                        .bindings = layout.resources,
                                        .parameters = layout.parameters,
                                        .stageInterface = {{ShaderStage::Vertex, ShaderInterfaceDirection::Input, 0,
                                                            ShaderValueType::Float32, 1, 3, 1}},
                                        .targetBindings = layout.targetBindings};
    fixture.metadata.reflection =
        SubmissionTest::Take(NormalizeShaderReflection(fixture.metadata.manifest, fixture.metadata.target, candidate));
    std::vector<std::uint8_t> artifact = vertexBytes;
    artifact.insert(artifact.end(), fragmentBytes.begin(), fragmentBytes.end());
    fixture.metadata.artifact = ComputeSha256(std::as_bytes(std::span{artifact}));
    fixture.vertex = fixture.Shader(ShaderStage::Vertex, vertexBytes);
    fixture.fragment = fixture.Shader(ShaderStage::Fragment, fragmentBytes);
    fixture.PreparePipeline(instanced ? RenderViewRouting::VertexLayer : RenderViewRouting::SingleView);
    fixture.PrepareDraw(true);
    auto secondView = fixture.backend->CreateTextureView({.texture = {{1}, 9, 1}, .baseLayer = 1}, fixture.output);
    REQUIRE(secondView.HasValue());
    auto left = Horo::Math::Mat4::Identity(), right = Horo::Math::Mat4::Identity(), model = Horo::Math::Mat4::Identity();
    left.values[12] = -0.125F;
    right.values[12] = 0.25F;
    model.values[0] = 0.5F;
    model.values[5] = 0.5F;
    model.values[14] = 0.5F;
    const std::array sourceViews{MultiviewSceneView{0, left}, MultiviewSceneView{1, right}};
    const std::array<std::uint32_t, 2> layers{0, instanced ? 1U : 0U};
    auto packed = SubmissionTest::Take(PackMultiviewShaderParameters(layout, sourceViews, model, layers));
    auto descriptor = fixture.metadata.Material();
    const auto nativeBytes = fixture.backend->QueryMaterialParameterBufferSize(fixture.native, descriptor, {1});
    REQUIRE(nativeBytes.HasValue());
    packed.bytes.resize(nativeBytes.Value(), std::byte{});
    fixture.buffer = fixture.Buffer(packed.bytes, RenderBufferUsage::Uniform);
    descriptor.resources.front().value = MaterialBufferBinding{{{1}, 5, 1}, 0, packed.bytes.size()};
    NativeMultiviewMaterial adapter;
    adapter.backend = fixture.backend.get();
    adapter.pipeline = fixture.native;
    adapter.buffer = fixture.buffer;
    adapter.bytes = packed.bytes.size();
    auto table = SubmissionTest::Take(MaterialBindingTable::Create({1}, adapter));
    const auto selection = SubmissionTest::Take(table->Publish(descriptor));
    auto material = SubmissionTest::Take(table->Acquire(selection.binding));
    const auto &support = fixture.backend->Capabilities().support;
    const MultiviewPublication publication{1, 1, 1, 1, support.deviceIncarnation, support.capabilityRevision};
    const std::array instances{MultiviewSceneInstance{{{1}, 11, 1}, material->Id(), model, {{-1, -1, 0}, {1, 1, 0}}}};
    auto extracted = SubmissionTest::Take(
        MultiviewSceneSnapshot::Extract(publication, publication, sourceViews, instances, capabilities,
                                        instanced ? MultiviewExecutionPolicy::RequireInstanced : MultiviewExecutionPolicy::Multipass));
    auto scene = std::make_shared<const MultiviewSceneSnapshot>(extracted);
    auto graph = MultiviewTest::Graph({{1}, 9, 1}, {{1}, 7, 1}, {{1}, 8, 1}, {{1}, 5, 1});
    const std::array targets{MultiviewGraphTarget{graph.color, {}, {{1}, 10, 1}, {}, {0, 0, 8, 8}, 0},
                             MultiviewGraphTarget{graph.color, {}, {{1}, 12, 1}, {}, {0, 0, 8, 8}, 1}};
    const std::array geometry{MultiviewGraphGeometry{0, graph.vertex, graph.index, graph.uniform, material}};
    const std::array workloads{
        RenderGraphPassWorkload{graph.graph.Passes().front().pass, RenderGraphMultiview{scene, publication, targets, geometry}}};
    const std::array<RenderGraphResourceInstance, 4> resources{
        {{graph.color, fixture.output}, {graph.vertex, fixture.vertices}, {graph.index, fixture.indices}, {graph.uniform, fixture.buffer}}};
    const std::array meshes{RenderGraphMeshInstance{{{1}, 11, 1}, fixture.mesh}};
    const std::array views{RenderGraphTextureViewInstance{{{1}, 10, 1}, fixture.outputView},
                           RenderGraphTextureViewInstance{{{1}, 12, 1}, secondView.Value()}};
    const std::array submissions{RenderGraphQueueSubmission{{{1}, {1}, {}, {{1}, 1}}, {0, 1}}};

    struct Lease final : IRenderGraphResourceLease {
        unsigned releases{};

        void Release() noexcept override {
            ++releases;
        }
    } lease;

    struct DrainBeforeLeaseDestruction {
        IRenderBackend &backend;

        ~DrainBeforeLeaseDestruction() {
            if (backend.BeginShutdown().HasError())
                std::terminate();
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            for (;;) {
                const auto state = backend.PollDrain();
                if (state.HasValue() && state.Value() == RenderBackendDrainProgress::Drained)
                    return;
                if (std::chrono::steady_clock::now() >= deadline)
                    std::terminate();
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
    } drain{*fixture.backend};

    const auto frame = SubmissionTest::Take(fixture.backend->BeginFrame({1, {8, 8}}));
    RenderGraphLeaseTransfer transfer;
    const auto result =
        fixture.backend->ExecuteGraph({frame, graph.graph, workloads, resources, &lease, false, &transfer, submissions, meshes, views});
    INFO(result.HasError() ? result.ErrorValue().message : "native multiview admitted");
    REQUIRE(result.HasValue());
    REQUIRE(transfer.authority == RenderGraphLeaseAuthority::BackendCompletion);
    REQUIRE(fixture.backend->Present(frame).HasValue());
    const auto completed = fixture.backend->ObserveSubmission(
        {{{1}, 1}, RenderCpuWaitPurpose::DeterministicTest, std::chrono::seconds(5), support.deviceIncarnation});
    REQUIRE(completed.HasValue());
    REQUIRE(completed.Value());
    CHECK(lease.releases == 1);
    fixture.backend->DestroyTextureView(secondView.Value());
    material.reset();
    REQUIRE(table->Shutdown().HasValue());
}
