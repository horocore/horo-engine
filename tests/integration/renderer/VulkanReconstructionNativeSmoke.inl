namespace ReconstructionSmoke {
    namespace Cook = Horo::Tests::ShaderCompilerFixture;

    /** @brief Keeps typed renderer failures visible in the active GPU test. */
    template <typename Value> Value Take(Result<Value> result) {
        INFO((result.HasError() ? result.ErrorValue().message : "renderer operation succeeded"));
        REQUIRE(result.HasValue());
        return std::move(result).Value();
    }

    struct CookedScene final {
        CookedNativeReconstructionKernel reconstruction;
        ShaderManifest rasterManifest;
        CompiledShaderArtifact raster;
    };

    /** @brief Cooks the existing portable triangle material and the selected native reconstruction entry. */
    CookedScene CookScene(NativeReconstructionEntry entry) {
        const auto dxc = Cook::Installation(ShaderCompilerTool::Dxc, "HORO_TEST_SHADER_DXC");
        const auto validator = Cook::Installation(ShaderCompilerTool::SpirvTools, "HORO_TEST_SHADER_SPIRV_VAL");
        const auto cross = Cook::Installation(ShaderCompilerTool::SpirvCross, "HORO_TEST_SHADER_SPIRV_CROSS");
        if (!dxc || !validator || !cross)
            SKIP("Set the three HORO_TEST_SHADER_* paths to the reviewed Linux Vulkan compiler artifacts.");
        Cook::TemporaryDirectory directory;
        ShaderCompilerToolchainConfiguration tools;
        tools.hostPlatform = "linux-x86_64-ubuntu-26.04";
        tools.scratchRoot = directory.Path() / "native-reconstruction";
        tools.tools = {*dxc, *validator, *cross};
        const auto adapter = Take(ExternalShaderCompilerAdapter::Create(std::move(tools), std::make_shared<NativeExternalProcessRunner>()));
        auto request = Cook::Request(ShaderTargetBackend::Vulkan, ShaderPayloadFormat::SpirV16, {dxc->identity, validator->identity});
        auto &target = request.targets.front().requirement;
        target.maximumInlineConstantBytes = 128;
        target.supportsCompute = true;
        target.supportsStorageResources = true;
        auto contract = Take(DescribeNativeReconstructionShader(entry, target));
        request.manifest = std::move(contract.manifest);
        request.source = Cook::Read(std::filesystem::path{HORO_PROJECT_SOURCE_DIR} / "assets/shaders/runtime/native_reconstruction.hlsl");
        request.dependencies.clear();
        request.reflection = ShaderCompilerReflectionRequest{cross->identity, std::move(contract.reflectionNames)};
        auto batch = Take(CompileShaderTargets(request, adapter, {}));
        REQUIRE(batch.artifacts.size() == 1);

        auto raster = Cook::Request(ShaderTargetBackend::Vulkan, ShaderPayloadFormat::SpirV16, {dxc->identity, validator->identity});
        raster.reflection = ShaderCompilerReflectionRequest{cross->identity, {}};
        auto rasterBatch = Take(CompileShaderTargets(raster, adapter, {}));
        REQUIRE(rasterBatch.artifacts.size() == 1);
        return {{entry, target, std::move(batch.artifacts.front())}, std::move(raster.manifest), std::move(rasterBatch.artifacts.front())};
    }

    /** @brief Keeps the real loader and test observation alive until the actual frontend has drained. */
    struct Owner final {
        SystemLoader system;
        Test::VfxInstanced::ObservedLoader observed{system};
        RenderBackendRegistry registry;
        std::unique_ptr<RenderFrontend> frontend;

        ~Owner() {
            if (!frontend)
                return;
            if (frontend->BeginShutdown().HasError())
                std::terminate();
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            for (;;) {
                const auto progress = frontend->PollDrain();
                if (progress.HasValue() && progress.Value() == RenderBackendDrainProgress::Drained)
                    return;
                if (std::chrono::steady_clock::now() >= deadline)
                    std::terminate();
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
    };

    /** @brief Processes an ordinary renderer creation and returns its exact ready handle. */
    template <typename Handle> Handle Ready(RenderFrontend &frontend, Result<ResourceCreation<Handle>> queued) {
        const auto resource = Take(std::move(queued));
        REQUIRE(frontend.ProcessResourceRequests().HasValue());
        REQUIRE(frontend.ResourceOperationResult(resource.operation).HasValue());
        return resource.handle;
    }

    struct SceneTriangle final {
        RenderBufferHandle vertices;
        RenderBufferHandle indices;
        RenderMeshHandle mesh;
        std::unique_ptr<MaterialBindingTable> table;
        std::shared_ptr<const ResidentMaterialBinding> material;
    };

    /** @brief Realizes the existing portable raster fixture through the actual shader, mesh and material owners. */
    SceneTriangle PrepareTriangle(RenderFrontend &frontend, const CookedScene &shaders) {
        const auto &raster = shaders.raster;
        const auto &target = shaders.rasterManifest.targets.front();
        REQUIRE(raster.reflection);
        const auto &reflection = *raster.reflection;
        const auto vertex =
            Ready(frontend, frontend.CreateShaderModule({target, ShaderStage::Vertex, "VertexMain", reflection.cookedArtifactKey,
                                                         reflection.reflection.interfaceCompatibility},
                                                        raster.payload));
        const auto fragment =
            Ready(frontend, frontend.CreateShaderModule({target, ShaderStage::Fragment, "FragmentMain", reflection.cookedArtifactKey,
                                                         reflection.reflection.interfaceCompatibility},
                                                        raster.payload));
        const RenderGraphicsPipelineState state{.cull = RenderCullMode::None,
                                                .depthTest = false,
                                                .depthWrite = false,
                                                .colorFormat = RenderTextureFormat::Rgba16Float,
                                                .depthFormat = std::nullopt};
        const auto device = Take(frontend.QueryPipelineDeviceIdentity());
        const auto stateKey = Take(ComputeRenderGraphicsPipelineStateDigest(state));
        const auto pipeline =
            Ready(frontend, frontend.CreateGraphicsPipeline({
                                vertex,
                                fragment,
                                shaders.rasterManifest,
                                target,
                                reflection.reflection,
                                state,
                                {1, device.backend, device.backendModuleVersion, device.adapter, device.deviceCompatibilityDigest,
                                 device.driverIdentity, device.driverVersion, target.backend, target.payloadFormat,
                                 reflection.cookedArtifactKey, reflection.reflection.interfaceCompatibility, stateKey},
                            }));

        constexpr std::array<MeshVertex, 3> vertices{{{{-0.5F, -0.5F, 0.5F}, {0, 0, 1}, {0, 0}},
                                                      {{0.0F, 0.5F, 0.5F}, {0, 0, 1}, {0.5F, 1}},
                                                      {{0.5F, -0.5F, 0.5F}, {0, 0, 1}, {1, 0}}}};
        constexpr std::array<std::uint32_t, 3> indices{0, 1, 2};
        SceneTriangle scene;
        scene.vertices = Ready(frontend, frontend.CreateBuffer({.byteSize = sizeof(vertices),
                                                                .usage = RenderBufferUsage::Vertex,
                                                                .access = RenderBufferAccess::HostVisible},
                                                               std::as_bytes(std::span{vertices})));
        scene.indices = Ready(frontend, frontend.CreateBuffer({.byteSize = sizeof(indices),
                                                               .usage = RenderBufferUsage::Index,
                                                               .access = RenderBufferAccess::HostVisible},
                                                              std::as_bytes(std::span{indices})));
        scene.mesh = Ready(frontend, frontend.CreateMesh({.vertexBuffer = scene.vertices,
                                                          .indexBuffer = scene.indices,
                                                          .vertexStride = sizeof(MeshVertex),
                                                          .vertexCount = 3,
                                                          .indexCount = 3,
                                                          .localBounds = {{-0.5F, -0.5F, 0.5F}, {0.5F, 0.5F, 0.5F}}}));
        scene.table = Take(MaterialBindingTable::Create(pipeline.owner, frontend));
        const auto layout = Take(PrepareMaterialBindingLayout(reflection.reflection));
        const auto published = Take(scene.table->Publish({.material = {1}, .revision = 1, .pipeline = pipeline, .layout = layout}));
        scene.material = Take(scene.table->Acquire(published.binding));
        return scene;
    }

    /** @brief Prepares one renderer-owned texture and its exact whole color view. */
    auto PrepareImage(RenderFrontend &frontend, const RenderTextureDescriptor &descriptor) {
        const auto texture = Take(frontend.CreateTexture(descriptor));
        REQUIRE(frontend.ProcessResourceRequests().HasValue());
        REQUIRE(frontend.ResourceOperationResult(texture.operation).HasValue());
        const auto view = Take(
            frontend.CreateTextureView({.texture = texture.handle, .format = descriptor.format, .aspect = RenderTextureAspect::Color}));
        REQUIRE(frontend.ProcessResourceRequests().HasValue());
        REQUIRE(frontend.ResourceOperationResult(view.operation).HasValue());
        return std::pair{texture.handle, view.handle};
    }

    using Image = NativeReconstructionImage;
    using Entry = NativeReconstructionEntry;
    constexpr FramebufferExtent Source{17, 9};
    constexpr FramebufferExtent Target{64, 32};
    constexpr std::array Color{0.25F, 0.5F, 0.75F, 1.0F};
    constexpr float Depth = 0.5F;

    /** @brief Uses real canonical texture formats, with physical previous guides written by graph clear passes. */
    RenderTextureDescriptor ImageDescriptor(Image role) {
        using enum RenderTextureUsage;
        using enum RenderTextureFormat;
        const auto sampled = Sampled | RenderAttachment;
        const auto written = Sampled | Storage | CopySource;
        switch (role) {
            case Image::CurrentColor:
                return {.extent = Source, .format = Rgba16Float, .usage = sampled};
            case Image::OutputColor:
                return {.extent = Target, .format = Rgba32Float, .usage = written};
            case Image::CurrentDepth:
                return {.extent = Source, .format = R32Float, .usage = sampled};
            case Image::Motion:
                return {.extent = Source, .format = Rg16Float, .usage = sampled};
            case Image::PreviousColor:
                return {.extent = Target, .format = Rgba32Float, .usage = sampled};
            case Image::PreviousDepth:
                return {.extent = Target, .format = R32Float, .usage = sampled};
            case Image::OutputDepth:
                return {.extent = Target, .format = R32Float, .usage = written};
            case Image::Count:
                std::terminate();
        }
        std::terminate();
    }

    /** @brief Gives every sampled texture defined native contents in the same submitted graph. */
    ClearColor ImageContents(Image role) noexcept {
        switch (role) {
            case Image::CurrentColor:
                return {0.0F, 0.0F, 0.0F, 1.0F};
            case Image::PreviousColor:
                return {0.875F, 0.125F, 0.5F, 0.75F};
            case Image::CurrentDepth:
            case Image::PreviousDepth:
                return {Depth, 0.0F, 0.0F, 1.0F};
            default:
                return {0.0F, 0.0F, 0.0F, 0.0F};
        }
    }

    /** @brief Owns exact scene graph output passes and keeps every view available until command retirement. */
    void ExecuteGraph(RenderFrontend &frontend, const std::span<const RenderQueueAssignment> queues, Entry entry,
                      const std::shared_ptr<IResidentNativeReconstructionKernel> &kernel, const SceneTriangle &triangle,
                      bool foreignSceneView) {
        constexpr auto images = RenderGraphNativeReconstruction::ImageCount;
        auto builder = Take(RenderGraphBuilder::Create(
            {.maxPasses = images + 1, .maxResources = images + 2, .maxUsages = images * 2 + 2, .maxDependencies = images * 2}));
        std::vector<RenderGraphPassWorkload> workloads;
        std::vector<RenderGraphImportedState> initial;
        const auto vertex = Take(builder.ImportBuffer(triangle.vertices, RenderGraphResourceClass::Persistent));
        const auto index = Take(builder.ImportBuffer(triangle.indices, RenderGraphResourceClass::Persistent));
        const std::array draws{RenderGraphMaterialDraw{triangle.mesh, vertex, index, triangle.material}};
        const RenderGraphLogicalState readyInput{RenderGraphSynchronizationAccess::Read,
                                                 RenderGraphSynchronizationOperation::GraphicsInputBuffer,
                                                 RenderGraphPipelineScope::Graphics,
                                                 RenderGraphTextureLayout::NotApplicable,
                                                 {1}};
        initial.push_back({vertex, readyInput});
        initial.push_back({index, readyInput});
        RenderGraphNativeReconstruction workload;
        workload.dispatch = {.entry = entry,
                             .constants = {.extents = {Source.width, Source.height, Target.width, Target.height},
                                           .sample = {0.0F, 0.0F, 1.0F, entry == Entry::TemporalHistory ? 0.75F : 0.0F}}};
        workload.kernel = kernel;
        std::optional<std::size_t> scenePass;
        for (std::size_t slot = 0; slot < images; ++slot) {
            const auto role = static_cast<Image>(slot);
            if (!NativeReconstructionUsesImage(entry, role))
                continue;
            const auto [texture, view] = PrepareImage(frontend, ImageDescriptor(role));
            const auto image = Take(builder.ImportTexture(texture, RenderGraphResourceClass::Persistent));
            workload.images[slot] = {image, view};
            initial.push_back({image, {}, RenderGraphInitialContents::Discarded});
            if (!NativeReconstructionWritesImage(role)) {
                const auto clear = Take(builder.AddPass(RenderPassKind::Graphics, RenderQueueRole::Graphics));
                REQUIRE(builder.AddUsage({clear, image, RenderGraphAccess::Write, RenderGraphUsageKind::ColorAttachment}).HasValue());
                if (role == Image::CurrentColor) {
                    REQUIRE(builder.AddUsage({clear, vertex, RenderGraphAccess::Read, RenderGraphUsageKind::VertexBuffer}).HasValue());
                    REQUIRE(builder.AddUsage({clear, index, RenderGraphAccess::Read, RenderGraphUsageKind::IndexBuffer}).HasValue());
                    scenePass = workloads.size();
                    workloads.push_back({clear, RenderGraphMaterialRaster{.color = image,
                                                                          .colorView = view,
                                                                          .extent = Source,
                                                                          .colorOperations = {.clearColor = ImageContents(role)},
                                                                          .depthClear = std::nullopt,
                                                                          .draws = draws}});
                } else {
                    workloads.push_back({clear, RenderGraphColorAttachment{image, {.clearColor = ImageContents(role)}}});
                }
            }
        }

        REQUIRE(scenePass);
        if (foreignSceneView)
            std::get<RenderGraphMaterialRaster>(workloads[*scenePass].workload).colorView =
                workload.images[static_cast<std::size_t>(Image::OutputColor)].view;

        const auto reconstruct = Take(builder.AddPass(RenderPassKind::Compute, RenderQueueRole::Compute));
        for (std::size_t slot = 0; slot < images; ++slot) {
            const auto image = workload.images[slot].resource;
            if (!image.IsValid())
                continue;
            const bool writes = NativeReconstructionWritesImage(static_cast<Image>(slot));
            REQUIRE(builder
                        .AddUsage({reconstruct, image, writes ? RenderGraphAccess::Write : RenderGraphAccess::Read,
                                   writes ? RenderGraphUsageKind::Storage : RenderGraphUsageKind::Sampled})
                        .HasValue());
        }
        workloads.push_back({reconstruct, workload});
        const auto graph = Take(builder.Finalize());
        const auto schedule = Take(CompileRenderGraph(graph));
        const auto synchronization = Take(SynthesizeRenderGraphSynchronization(graph, schedule, queues, initial));
        const auto compiled = Take(CompileRenderGraphExecution(graph, schedule, synchronization, queues));
        const std::array submissions{RenderGraphQueueSubmission{RenderQueueSubmission{{1}, {1}, {}, {{1}, 1}}, {0, workloads.size()}}};
        auto frame = Take(frontend.BeginFrame({.frameNumber = 1, .outputExtent = Target}));
        const auto recorded = frame.ExecuteSubmittedGraph(compiled, workloads, submissions);
        INFO((recorded.HasError() ? recorded.ErrorValue().message : "native reconstruction recorded"));
        if (foreignSceneView) {
            REQUIRE(recorded.HasError());
            CHECK(recorded.ErrorValue().code.Value() == "render.vulkan.resource_identity_invalid");
            CHECK_FALSE(frame.Token().IsValid());
            return;
        }
        REQUIRE(recorded.HasValue());
        REQUIRE(frame.Present().HasValue());
        RequireCompletion(frontend, submissions.front().submission.signal);
    }

    /** @brief Verifies actual native color and linear depth without copying through the production frame hot path. */
    void VerifyPixels(const Test::VfxInstanced::ObservedLoader &loader, Entry entry) {
        using Readback = Test::VfxInstanced::NativeReadback;
        Readback readback{loader, Readback::Capture::ReconstructionColor};
        readback.Prepare();
        const auto pixels = readback.ReadReconstructed();
        REQUIRE(pixels.size() == Target.width * Target.height * Color.size());
        const auto sample = [&](std::size_t x, std::size_t y, std::size_t channel) {
            return pixels[(y * Target.width + x) * Color.size() + channel];
        };
        for (std::size_t channel = 0; channel < Color.size(); ++channel)
            CHECK(sample(32, 16, channel) == Catch::Approx(Color[channel]));
        for (std::size_t channel = 0; channel < 3; ++channel)
            CHECK(sample(0, 0, channel) == 0.0F);
        bool filteredEdge = false;
        for (std::size_t i = 0; i < pixels.size(); ++i) {
            const auto channel = i % Color.size();
            if (channel == 3) {
                REQUIRE(pixels[i] == Catch::Approx(1.0F));
                continue;
            }
            REQUIRE(pixels[i] >= -0.001F);
            REQUIRE(pixels[i] <= Color[channel] + 0.001F);
            filteredEdge |= pixels[i] > 0.01F && pixels[i] < Color[channel] - 0.01F;
        }
        CHECK(filteredEdge);
        if (entry == Entry::Spatial)
            return;
        Readback depth{loader, Readback::Capture::ReconstructionDepth};
        depth.Prepare();
        const auto resolvedDepth = depth.ReadReconstructed();
        REQUIRE(resolvedDepth.size() == Target.width * Target.height);
        for (const auto pixel : resolvedDepth)
            REQUIRE(pixel == Catch::Approx(Depth));
    }
}  // namespace ReconstructionSmoke

TEST_CASE("Vulkan reconstruction graph produces real spatial and temporal pixels", "[integration][renderer][vulkan][gpu][reconstruction]") {
    using namespace ReconstructionSmoke;
    const auto entry = GENERATE(Entry::Spatial, Entry::TemporalSeed, Entry::TemporalHistory);
    CAPTURE(static_cast<unsigned>(entry));
    const auto cooked = CookScene(entry);
    Owner owner;
    REQUIRE(RegisterVulkanRenderBackend(owner.registry, owner.observed).HasValue());
    REQUIRE(owner.registry.Seal().HasValue());
    RenderBackendConfig config{.requirePresentation = false, .enableValidation = true};
    config.queueAssignments = {{RenderQueueRole::Graphics, {1}}, {RenderQueueRole::Compute, {1}}, {RenderQueueRole::Transfer, {1}}};
    owner.frontend = Take(RenderFrontend::Create(owner.registry, RenderBackendId{"vulkan"}, config));
    auto &frontend = *owner.frontend;
    REQUIRE(frontend.Capabilities().support.features.Supports(RenderCapability::NativeReconstruction));
    const auto kernel = Take(frontend.RealizeNativeReconstructionKernel(cooked.reconstruction));
    REQUIRE(frontend.Capabilities().supportsMaterialRasterGraph);
    SECTION("The native raster output feeds reconstruction inside the same graph") {
        const auto scene = PrepareTriangle(frontend, cooked);
        ExecuteGraph(frontend, config.queueAssignments, entry, kernel, scene, false);
        VerifyPixels(owner.observed, entry);
    }
    SECTION("A foreign native raster view aborts before any reconstruction") {
        const auto scene = PrepareTriangle(frontend, cooked);
        ExecuteGraph(frontend, config.queueAssignments, entry, kernel, scene, true);
    }
    Test::RequireDrainedOwner(frontend);
}
