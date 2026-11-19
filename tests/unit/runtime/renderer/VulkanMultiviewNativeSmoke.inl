#include <catch2/generators/catch_generators.hpp>

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
    const bool instanced = GENERATE(false, true);
    const bool observeExplicitly = GENERATE(true, false);
    CAPTURE(instanced, observeExplicitly);
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

    {
        RenderGraphLeaseTransfer transfer;

        struct CompleteBeforeLeaseDestruction {
            IRenderBackend &backend;
            Lease &lease;
            const RenderGraphLeaseTransfer &transfer;
            RenderCpuWait wait;

            ~CompleteBeforeLeaseDestruction() {
                backend.AbortActiveFrame();
                if (transfer.authority != RenderGraphLeaseAuthority::BackendCompletion || lease.releases != 0)
                    return;
                // Complete queued work without tearing down the device: geometry/table still own native descriptors.
                // NativePreparationFixture closes the device after those owners have been destroyed.
                const auto completed = backend.ObserveSubmission(wait);
                if (completed.HasError() || !completed.Value() || lease.releases != 1)
                    std::terminate();
            }
        } complete{*fixture.backend,
                   lease,
                   transfer,
                   {{{1}, 1}, RenderCpuWaitPurpose::DeterministicTest, std::chrono::seconds(5), support.deviceIncarnation}};

        const auto frame = SubmissionTest::Take(fixture.backend->BeginFrame({1, {8, 8}}));
        const auto result =
            fixture.backend->ExecuteGraph({frame, graph.graph, workloads, resources, &lease, false, &transfer, submissions, meshes, views});
        INFO((result.HasError() ? result.ErrorValue().message : "native multiview admitted"));
        REQUIRE(result.HasValue());
        REQUIRE(transfer.authority == RenderGraphLeaseAuthority::BackendCompletion);
        REQUIRE(fixture.backend->Present(frame).HasValue());
        if (observeExplicitly) {
            const auto completed = fixture.backend->ObserveSubmission(
                {{{1}, 1}, RenderCpuWaitPurpose::DeterministicTest, std::chrono::seconds(5), support.deviceIncarnation});
            REQUIRE(completed.HasValue());
            REQUIRE(completed.Value());
            CHECK(lease.releases == 1);
        }
    }
    CHECK(lease.releases == 1);
    // Completing submitted work must not retire the device while geometry still owns its descriptor/resource lease.
    REQUIRE(fixture.backend->QueryMaterialParameterBufferSize(fixture.native, descriptor, {1}).HasValue());
    fixture.backend->DestroyTextureView(secondView.Value());
    material.reset();
    REQUIRE(table->Shutdown().HasValue());
}

#if defined(HORO_TEST_OPENXR_LOADER)
#include "EditorVulkanViewportShader.h"
#include "Horo/Runtime/Render/ShaderArtifact.h"
#include "VulkanEditorGraphicsBridge.h"

namespace {
    /** @brief Backend-owned stereo attachment and its two graph view identities. */
    struct NativeCoverageImage {
        RenderTextureHandle texture;
        std::array<RenderGraphTextureViewInstance, 2> eyes;
        std::uint64_t instance{};
    };

    /** @brief Delegates test material realization to the selected native backend. */
    struct NativeCoverageMaterial final : IMaterialBindingBackend {
        IRenderBackend *backend{};
        std::uint64_t pipeline{};
        std::vector<MaterialResidentResource> resources;

        Result<std::unique_ptr<IResidentMaterialBinding>> Realize(const MaterialBindingDescriptor &descriptor) override {
            return backend->RealizeResolved(descriptor, pipeline, resources);
        }
    };

    /** @brief Creates an owned 8x8 stereo image with exact one-layer native views. */
    NativeCoverageImage NativeCoverageTarget(IRenderBackend &backend, const RenderTextureFormat format, const RenderTextureUsage usage,
                                             const std::uint32_t slot) {
        const RenderTextureDescriptor descriptor{.extent = {8, 8}, .format = format, .layerCount = 2, .usage = usage};
        const auto cost = SubmissionTest::Take(backend.QueryTextureMemoryCost(descriptor));
        NativeCoverageImage result{.texture = {{1}, slot, 1}};
        result.instance = SubmissionTest::Take(backend.CreateTexture(descriptor, {}, TestSupport::PlacementFor(cost, slot)));
        const auto aspect = format == RenderTextureFormat::Depth32Float ? RenderTextureAspect::Depth : RenderTextureAspect::Color;
        for (std::uint32_t eye = 0; eye < result.eyes.size(); ++eye) {
            const RenderTextureViewDescriptor view{.texture = result.texture, .format = format, .aspect = aspect, .baseLayer = eye};
            result.eyes[eye] = {{{1}, slot + eye + 1, 1}, SubmissionTest::Take(backend.CreateTextureView(view, result.instance))};
        }
        return result;
    }

    /** @brief Realizes an immutable cook using its own final layout and exact native graphics state. */
    void PrepareNativeCoverageProgram(NativePreparationFixture &fixture, const Editor::EditorVulkanMultiviewCook &cook,
                                      ShaderManifest manifest, const RenderGraphicsPipelineState &state) {
        fixture.metadata.manifest = std::move(manifest);
        fixture.metadata.target = Editor::EditorVulkanViewportShaderTarget();
        fixture.metadata.reflection = cook.metadata.reflection;
        fixture.metadata.artifact = cook.metadata.compiledArtifactKey;
        const auto stages = SubmissionTest::Take(DecodeShaderArtifactStages(cook.payload, fixture.metadata.target.backend,
                                                                            fixture.metadata.target.payloadFormat, 16U * 1024U * 1024U));
        REQUIRE(stages.stageCount == 2);
        for (const auto &stage : std::span{stages.stages}.first(stages.stageCount)) {
            const auto native = fixture.Shader(stage.stage, {stage.bytes.begin(), stage.bytes.end()});
            (stage.stage == ShaderStage::Vertex ? fixture.vertex : fixture.fragment) = native;
        }
        fixture.PreparePipeline(state.viewRouting, &state);
    }

    /** @brief Waits only at the native test boundary, keeping the graph lease alive through submission completion. */
    void SubmitNativeCoverageGraph(IRenderBackend &backend, RenderGraphBuilder &builder,
                                   const std::span<const RenderGraphImportedState> initial,
                                   const std::span<const RenderGraphPassWorkload> workloads,
                                   const std::span<const RenderGraphResourceInstance> resources, const std::uint64_t sequence,
                                   const std::span<const RenderGraphMeshInstance> meshes = {},
                                   const std::span<const RenderGraphTextureViewInstance> views = {}) {
        const auto authored = SubmissionTest::Take(builder.Finalize());
        const auto compiled = CompileRenderGraph(authored);
        INFO((compiled.HasError() ? compiled.ErrorValue().message : "coverage schedule compiled"));
        REQUIRE(compiled.HasValue());
        const auto &schedule = compiled.Value();
        const std::array queues{RenderQueueAssignment{RenderQueueRole::Graphics, {1}}};
        const auto synchronization = SynthesizeRenderGraphSynchronization(authored, schedule, queues, initial);
        INFO((synchronization.HasError() ? synchronization.ErrorValue().message : "coverage synchronization compiled"));
        REQUIRE(synchronization.HasValue());
        const auto graph = SubmissionTest::Take(CompileRenderGraphExecution(authored, schedule, synchronization.Value(), queues));
        const std::array submissions{RenderGraphQueueSubmission{{{1}, {sequence}, {}, {{1}, sequence}}, {0, workloads.size()}}};

        struct Lease final : IRenderGraphResourceLease {
            std::size_t released{};

            void Release() noexcept override {
                ++released;
            }
        } lease;

        RenderGraphLeaseTransfer transfer;
        const RenderCpuWait wait{{{1}, sequence},
                                 RenderCpuWaitPurpose::DeterministicTest,
                                 std::chrono::seconds(5),
                                 backend.Capabilities().support.deviceIncarnation};

        struct Finish final {
            IRenderBackend &backend;
            const RenderGraphLeaseTransfer &transfer;
            Lease &lease;
            RenderCpuWait wait;

            ~Finish() {
                backend.AbortActiveFrame();
                if (transfer.authority == RenderGraphLeaseAuthority::BackendCompletion && lease.released == 0) {
                    const auto result = backend.ObserveSubmission(wait);
                    if (result.HasError() || !result.Value() || lease.released != 1)
                        std::terminate();
                }
            }
        } finish{backend, transfer, lease, wait};

        const auto frame = SubmissionTest::Take(backend.BeginFrame({sequence, {8, 8}}));
        const auto executed =
            backend.ExecuteGraph({frame, graph, workloads, resources, &lease, false, &transfer, submissions, meshes, views});
        INFO((executed.HasError() ? executed.ErrorValue().message : "native coverage graph admitted"));
        REQUIRE(executed.HasValue());
        REQUIRE(transfer.authority == RenderGraphLeaseAuthority::BackendCompletion);
        REQUIRE(backend.Present(frame).HasValue());
        REQUIRE(SubmissionTest::Take(backend.ObserveSubmission(wait)));
        CHECK(lease.released == 1);
    }

    /** @brief Seeds known native predecessor samples through the production graph depth attachment operation. */
    void SeedNativeCoverageDepth(IRenderBackend &backend, const NativeCoverageImage &depth) {
        auto builder = SubmissionTest::Take(RenderGraphBuilder::Create());
        const auto pass = SubmissionTest::Take(builder.AddPass(RenderPassKind::Graphics, RenderQueueRole::Graphics));
        const auto resource = SubmissionTest::Take(builder.ImportTexture(depth.texture, RenderGraphResourceClass::Persistent));
        REQUIRE(builder.AddUsage({pass, resource, RenderGraphAccess::Write, RenderGraphUsageKind::DepthStencilAttachment}).HasValue());
        const std::array initial{RenderGraphImportedState{resource, {}, RenderGraphInitialContents::Discarded}};
        const std::array workloads{RenderGraphPassWorkload{pass, RenderGraphDepthAttachment{resource, 0.5F}}};
        const std::array resources{RenderGraphResourceInstance{resource, depth.instance}};
        SubmitNativeCoverageGraph(backend, builder, initial, workloads, resources, 1);
    }

    /** @brief Owns a test material table and resources whose native backing is borrowed until queue completion. */
    struct NativeCoverageDraw {
        NativeCoverageMaterial adapter;
        std::unique_ptr<MaterialBindingTable> table;
        std::shared_ptr<const ResidentMaterialBinding> material;
        std::shared_ptr<const MultiviewSceneSnapshot> scene;
        RenderBufferHandle uniform;
        std::uint64_t nativeUniform{};
    };

    /** @brief Publishes one genuine uniform and the exact sampled predecessor generations through material binding. */
    void PublishNativeCoverageDraw(NativeTransformFixture &fixture, NativeCoverageDraw &draw, std::vector<std::byte> bytes,
                                   const MultiviewPublication publication, const std::span<const MultiviewSceneView> eyes,
                                   const RenderMeshHandle mesh, const Math::Mat4 &model, const NativeCoverageImage *prior = nullptr) {
        const auto layout = SubmissionTest::Take(PrepareMaterialBindingLayout(fixture.metadata.reflection));
        MaterialBindingDescriptor descriptor{.material = {draw.uniform.slot},
                                             .revision = 1,
                                             .pipeline = {{1}, draw.uniform.slot, 1},
                                             .layout = layout,
                                             .resources = {{{1}, 0, MaterialBufferBinding{draw.uniform, 0, bytes.size()}}}};
        const auto nativeSize = SubmissionTest::Take(fixture.backend->QueryMaterialParameterBufferSize(fixture.native, descriptor, {1}));
        REQUIRE(nativeSize >= bytes.size());
        bytes.resize(nativeSize, std::byte{});
        descriptor.resources.front().value = MaterialBufferBinding{draw.uniform, 0, bytes.size()};
        draw.nativeUniform = fixture.Buffer(bytes, RenderBufferUsage::Uniform);
        draw.adapter.backend = fixture.backend.get();
        draw.adapter.pipeline = fixture.native;
        draw.adapter.resources.push_back({MaterialResidentResourceKind::Buffer, draw.nativeUniform, 0, bytes.size()});
        if (prior) {
            const auto &bindings = MultiviewShaderParameterIds::PreviousEyeDepthBindings;
            for (std::size_t eye = 0; eye < prior->eyes.size(); ++eye) {
                descriptor.resources.push_back({bindings[eye], 0, prior->eyes[eye].view});
                draw.adapter.resources.push_back({MaterialResidentResourceKind::TextureView, prior->eyes[eye].instance});
            }
        }
        draw.table = SubmissionTest::Take(MaterialBindingTable::Create(draw.uniform.owner, draw.adapter));
        const auto selection = draw.table->Publish(descriptor);
        INFO((selection.HasError() ? selection.ErrorValue().message : "native material published"));
        REQUIRE(selection.HasValue());
        draw.material = SubmissionTest::Take(draw.table->Acquire(selection.Value().binding));
        const std::array instances{MultiviewSceneInstance{mesh, draw.material->Id(), model, {{-1, -1, 0}, {1, 1, 0}}}};
        const auto capabilities = ResolveMultiviewCapabilities(fixture.backend->Capabilities().support);
        draw.scene = std::make_shared<const MultiviewSceneSnapshot>(SubmissionTest::Take(
            MultiviewSceneSnapshot::Extract(publication, publication, eyes, instances, capabilities, MultiviewExecutionPolicy::Multipass)));
    }
}  // namespace

namespace {
    /** @brief Renders exact shared current geometry, then transfers every requested classified sample in the same submission. */
    void ExecuteNativeCoverageProbe(NativeTransformFixture &fixture, const NativeCoverageDraw &projection,
                                    const NativeCoverageDraw &coverage, const NativeCoverageImage &prior, const NativeCoverageImage &color,
                                    const NativeCoverageImage &mask, const NativeCoverageImage &depth, const RenderMeshHandle mesh,
                                    const std::uint64_t staging) {
        auto builder = SubmissionTest::Take(RenderGraphBuilder::Create());
        const auto projectionPass = SubmissionTest::Take(builder.AddPass(RenderPassKind::Graphics, RenderQueueRole::Graphics));
        const auto coveragePass = SubmissionTest::Take(builder.AddPass(RenderPassKind::Graphics, RenderQueueRole::Graphics));
        const auto copyPass = SubmissionTest::Take(builder.AddPass(RenderPassKind::Copy, RenderQueueRole::Graphics));
        REQUIRE(builder.AddDependency({projectionPass, coveragePass}).HasValue());
        REQUIRE(builder.AddDependency({coveragePass, copyPass}).HasValue());
        std::vector<RenderGraphResourceInstance> resources;
        std::vector<RenderGraphImportedState> initial;
        std::vector<RenderGraphTextureViewInstance> views;
        const RenderGraphLogicalState input{RenderGraphSynchronizationAccess::Read,
                                            RenderGraphSynchronizationOperation::GraphicsInputBuffer,
                                            RenderGraphPipelineScope::Graphics,
                                            RenderGraphTextureLayout::NotApplicable,
                                            {1}};
        const auto buffer = [&](const RenderBufferHandle handle, const std::uint64_t native,
                                const RenderGraphInitialContents contents = RenderGraphInitialContents::Preserved) {
            const auto id = SubmissionTest::Take(builder.ImportBuffer(handle, RenderGraphResourceClass::Persistent));
            resources.push_back({id, native});
            initial.push_back({id, contents == RenderGraphInitialContents::Discarded ? RenderGraphLogicalState{} : input, contents});
            return id;
        };
        const auto importImage = [&](const NativeCoverageImage &image, const bool preserved = false) {
            const auto id = SubmissionTest::Take(builder.ImportTexture(image.texture, RenderGraphResourceClass::Persistent));
            resources.push_back({id, image.instance});
            views.insert(views.end(), image.eyes.begin(), image.eyes.end());
            const RenderGraphLogicalState retainedDepth{RenderGraphSynchronizationAccess::Write,
                                                        RenderGraphSynchronizationOperation::DepthStencilAttachment,
                                                        RenderGraphPipelineScope::Graphics,
                                                        RenderGraphTextureLayout::DepthStencilAttachment,
                                                        {1}};
            initial.push_back({id, preserved ? retainedDepth : RenderGraphLogicalState{},
                               preserved ? RenderGraphInitialContents::Preserved : RenderGraphInitialContents::Discarded});
            return id;
        };
        const auto use = [&](const RenderGraphPassRef pass, const RenderGraphResourceId resource, const RenderGraphUsageKind kind,
                             const RenderGraphAccess access = RenderGraphAccess::Read) {
            REQUIRE(builder.AddUsage({pass, resource, access, kind}).HasValue());
        };
        const auto vertex = buffer({{1}, 7, 1}, fixture.vertices);
        const auto index = buffer({{1}, 8, 1}, fixture.indices);
        const auto priorId = importImage(prior, true);
        const auto maskId = importImage(mask);
        const auto colorId = importImage(color);
        const auto depthId = importImage(depth);
        use(projectionPass, colorId, RenderGraphUsageKind::ColorAttachment, RenderGraphAccess::Write);
        use(coveragePass, maskId, RenderGraphUsageKind::ColorAttachment, RenderGraphAccess::Write);
        use(coveragePass, depthId, RenderGraphUsageKind::DepthStencilAttachment, RenderGraphAccess::Write);
        use(coveragePass, priorId, RenderGraphUsageKind::Sampled);
        const auto geometry = [&](const NativeCoverageDraw &draw, const RenderGraphPassRef pass) {
            const auto uniform = buffer(draw.uniform, draw.nativeUniform);
            use(pass, vertex, RenderGraphUsageKind::VertexBuffer);
            use(pass, index, RenderGraphUsageKind::IndexBuffer);
            use(pass, uniform, RenderGraphUsageKind::UniformBuffer);
            return std::array{MultiviewGraphGeometry{0, vertex, index, uniform, draw.material}};
        };
        const auto ordinaryGeometry = geometry(projection, projectionPass);
        const auto coverageGeometry = geometry(coverage, coveragePass);
        std::array<MultiviewGraphTarget, 2> ordinaryTargets{}, coverageTargets{};
        for (std::uint32_t eye = 0; eye < ordinaryTargets.size(); ++eye) {
            ordinaryTargets[eye] = {colorId, {}, color.eyes[eye].view, {}, {0, 0, 8, 8}, eye};
            coverageTargets[eye] =
                {maskId, depthId, mask.eyes[eye].view, depth.eyes[eye].view, {0, 0, 8, 8}, eye, eye, priorId, prior.eyes[eye].view, eye};
        }
        const auto stagingId = buffer({{1}, 93, 1}, staging, RenderGraphInitialContents::Discarded);
        const RenderGraphStereoCoverageCopy copy{maskId, stagingId, {{{4, 4, 1, 1}, {3, 4, 2, 1}}}};
        use(copyPass, maskId, RenderGraphUsageKind::CopySource);
        use(copyPass, stagingId, RenderGraphUsageKind::CopyDestination, RenderGraphAccess::Write);
        const std::array workloads{RenderGraphPassWorkload{projectionPass,
                                                           RenderGraphMultiview{projection.scene, projection.scene->Publication(),
                                                                                ordinaryTargets, ordinaryGeometry}},
                                   RenderGraphPassWorkload{coveragePass,
                                                           RenderGraphMultiview{.scene = coverage.scene,
                                                                                .active = coverage.scene->Publication(),
                                                                                .targets = coverageTargets,
                                                                                .geometry = coverageGeometry,
                                                                                .clearColor = {0, 0, 0, 0},
                                                                                .output = MultiviewOutput::ReprojectionCoverage}},
                                   RenderGraphPassWorkload{copyPass, copy}};
        const std::array meshes{RenderGraphMeshInstance{mesh, fixture.mesh}};
        SubmitNativeCoverageGraph(*fixture.backend, builder, initial, workloads, resources, 2, meshes, views);
    }

    /** @brief Packs known projected depths through the exact reflected material layout used by the native program. */
    std::vector<std::byte> NativeCoverageUniform(const MaterialBindingLayout &layout) {
        using namespace MultiviewShaderParameterIds;
        constexpr auto matrixSize = 16U;
        constexpr auto maximumValues = MultiviewExtractionLimits::HardMaximumViews * matrixSize;
        std::array<float, maximumValues> current{}, previous{};
        const auto place = [](std::span<float, matrixSize> matrix, const float depth) {
            for (const auto index : {0, 5, 10, 15})
                matrix[index] = 1.0F;
            matrix[11] = depth;
        };
        for (std::size_t eye = 0; eye < 2; ++eye) {
            place(std::span<float, matrixSize>{current.data() + eye * matrixSize, matrixSize}, 0.5F);
            place(std::span<float, matrixSize>{previous.data() + eye * matrixSize, matrixSize}, eye == 0 ? 0.5F : 0.75F);
        }
        const std::array values{MaterialParameterValue{CurrentLocalToClip, std::span<const float>{current}},
                                MaterialParameterValue{PreviousLocalToClip, std::span<const float>{previous}}};
        return SubmissionTest::Take(PackMaterialParameterBinding(layout, Buffer, values)).bytes;
    }
}  // namespace

TEST_CASE("Vulkan native coverage pixels classify exact stereo depth and copy through the completed graph",
          "[vulkan][multiview][coverage][native-smoke]") {
    const char *ordinaryPayload = std::getenv("HORO_NATIVE_MULTIVIEW_PAYLOAD");
    const char *ordinaryMetadata = std::getenv("HORO_NATIVE_MULTIVIEW_REFLECTION");
    const char *coveragePayload = std::getenv("HORO_NATIVE_COVERAGE_PAYLOAD");
    const char *coverageMetadata = std::getenv("HORO_NATIVE_COVERAGE_REFLECTION");
    if (!ordinaryPayload || !ordinaryMetadata || !coveragePayload || !coverageMetadata)
        SKIP("Cook both shipped multipass multiview programs for native qualification.");
    const auto ordinaryCook =
        SubmissionTest::Take(Editor::LoadEditorVulkanMultiviewCook(ReadShader(ordinaryPayload), ReadShader(ordinaryMetadata)));
    const auto coverageCook =
        SubmissionTest::Take(Editor::LoadEditorVulkanMultiviewCoverageCook(ReadShader(coveragePayload), ReadShader(coverageMetadata)));
    VulkanEditorGraphicsBridge bridge;
    NativeTransformFixture fixture;
    fixture.Initialize(&bridge);
    REQUIRE(bridge.Port());
    REQUIRE(bridge.Port()->SupportsStereoCoverageReadback());
    auto &backend = *fixture.backend;
    const auto &support = backend.Capabilities().support;
    const auto attachment = RenderTextureUsage::RenderAttachment;
    const auto sampled = RenderTextureUsage::Sampled;
    const auto color = NativeCoverageTarget(backend, RenderTextureFormat::Rgba8Unorm, attachment, 20);
    const auto mask =
        NativeCoverageTarget(backend, RenderTextureFormat::R32Float, attachment | sampled | RenderTextureUsage::CopySource, 30);
    const auto depth = NativeCoverageTarget(backend, RenderTextureFormat::Depth32Float, attachment, 40);
    const auto previous = NativeCoverageTarget(backend, RenderTextureFormat::Depth32Float, attachment | sampled, 50);
    SeedNativeCoverageDepth(backend, previous);

    const auto model = Math::Mat4::Identity();
    auto clip = Math::Mat4::Identity();
    clip.values[14] = 0.5F;
    const std::array eyes{MultiviewSceneView{0, clip}, MultiviewSceneView{1, clip}};
    const MultiviewPublication publication{2, 1, 1, 1, support.deviceIncarnation, support.capabilityRevision};
    const RenderMeshHandle mesh{{1}, 11, 1};
    fixture.PrepareDraw();
    NativeCoverageDraw projection{.uniform = {{1}, 70, 1}}, coverage{.uniform = {{1}, 71, 1}};
    const RenderGraphicsPipelineState ordinaryState{.cull = RenderCullMode::None,
                                                    .depthTest = false,
                                                    .depthWrite = false,
                                                    .depthFormat = std::nullopt};
    PrepareNativeCoverageProgram(fixture, ordinaryCook, Editor::EditorVulkanMultiviewShaderManifest(), ordinaryState);
    const auto ordinaryLayout = SubmissionTest::Take(PrepareMaterialBindingLayout(ordinaryCook.metadata.reflection));
    const std::array layers{0U, 0U};
    auto ordinaryUniform = SubmissionTest::Take(PackMultiviewShaderParameters(ordinaryLayout, eyes, model, layers)).bytes;
    PublishNativeCoverageDraw(fixture, projection, std::move(ordinaryUniform), publication, eyes, mesh, model);
    const RenderGraphicsPipelineState coverageState{.cull = RenderCullMode::None,
                                                    .colorFormat = RenderTextureFormat::R32Float,
                                                    .depthFormat = RenderTextureFormat::Depth32Float};
    PrepareNativeCoverageProgram(fixture, coverageCook, Editor::EditorVulkanMultiviewCoverageShaderManifest(), coverageState);
    auto maskUniform = NativeCoverageUniform(SubmissionTest::Take(PrepareMaterialBindingLayout(coverageCook.metadata.reflection)));
    PublishNativeCoverageDraw(fixture, coverage, std::move(maskUniform), publication, eyes, mesh, model, &previous);

    std::array<float, 3> pixels{};
    const auto staging = fixture.Buffer(std::as_bytes(std::span{pixels}), RenderBufferUsage::CopyDestination);
    ExecuteNativeCoverageProbe(fixture, projection, coverage, previous, color, mask, depth, mesh, staging);
    CHECK_FALSE(
        SubmissionTest::Take(bridge.Port()->ReadCompletedStereoCoverage(staging, {{1}, 1}, std::as_writable_bytes(std::span{pixels}))));
    REQUIRE(SubmissionTest::Take(bridge.Port()->ReadCompletedStereoCoverage(staging, {{1}, 2}, std::as_writable_bytes(std::span{pixels}))));
    using Code = Editor::EditorVulkanReprojectionCoverage;
    CHECK(pixels ==
          std::array{static_cast<float>(Code::DepthConsistent), static_cast<float>(Code::Occluded), static_cast<float>(Code::Occluded)});
}
#endif
