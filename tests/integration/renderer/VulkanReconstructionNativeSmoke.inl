namespace ReconstructionSmoke {
    namespace Cook = Horo::Tests::ShaderCompilerFixture;

    /** @brief Keeps typed renderer failures visible in the active GPU test. */
    template <typename Value> Value Take(Result<Value> result) {
        INFO((result.HasError() ? result.ErrorValue().message : "renderer operation succeeded"));
        REQUIRE(result.HasValue());
        return std::move(result).Value();
    }

    /** @brief Cooks through the existing locked adapter and admits the exact SPIR-V shader. */
    CookedNativeReconstructionKernel CookEntry(NativeReconstructionEntry entry) {
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
        return {entry, target, std::move(batch.artifacts.front())};
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
                return {Color[0], Color[1], Color[2], Color[3]};
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
                      const std::shared_ptr<IResidentNativeReconstructionKernel> &kernel) {
        constexpr auto images = RenderGraphNativeReconstruction::ImageCount;
        auto builder = Take(RenderGraphBuilder::Create(
            {.maxPasses = images + 1, .maxResources = images, .maxUsages = images * 2, .maxDependencies = images * 2}));
        std::vector<RenderGraphPassWorkload> workloads;
        std::vector<RenderGraphImportedState> initial;
        RenderGraphNativeReconstruction workload;
        workload.dispatch = {.entry = entry,
                             .constants = {.extents = {Source.width, Source.height, Target.width, Target.height},
                                           .sample = {0.0F, 0.0F, 1.0F, entry == Entry::TemporalHistory ? 0.75F : 0.0F}}};
        workload.kernel = kernel;
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
                workloads.push_back({clear, RenderGraphColorAttachment{image, {.clearColor = ImageContents(role)}}});
            }
        }

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
        for (std::size_t i = 0; i < pixels.size(); ++i)
            REQUIRE(pixels[i] == Catch::Approx(Color[i % Color.size()]));
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
    const auto cooked = CookEntry(entry);
    Owner owner;
    REQUIRE(RegisterVulkanRenderBackend(owner.registry, owner.observed).HasValue());
    REQUIRE(owner.registry.Seal().HasValue());
    RenderBackendConfig config{.requirePresentation = false, .enableValidation = true};
    config.queueAssignments = {{RenderQueueRole::Graphics, {1}}, {RenderQueueRole::Compute, {1}}, {RenderQueueRole::Transfer, {1}}};
    owner.frontend = Take(RenderFrontend::Create(owner.registry, RenderBackendId{"vulkan"}, config));
    auto &frontend = *owner.frontend;
    REQUIRE(frontend.Capabilities().support.features.Supports(RenderCapability::NativeReconstruction));
    const auto kernel = Take(frontend.RealizeNativeReconstructionKernel(cooked));
    ExecuteGraph(frontend, config.queueAssignments, entry, kernel);
    VerifyPixels(owner.observed, entry);
    Test::RequireDrainedOwner(frontend);
}
