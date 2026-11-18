namespace ReconstructionSmoke {
    using SubmissionTest::Take;
    namespace Cook = Horo::Tests::ShaderCompilerFixture;

    /** @brief Cooks through the existing locked adapter and admits the exact SPIR-V shader. */
    CookedNativeReconstructionKernel CookSpatial() {
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
        constexpr auto entry = NativeReconstructionEntry::Spatial;
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
}  // namespace ReconstructionSmoke

TEST_CASE("Vulkan reconstruction graph produces real spatial upsample pixels", "[integration][renderer][vulkan][gpu][reconstruction]") {
    using namespace ReconstructionSmoke;
    using enum RenderTextureUsage;
    const auto cooked = CookSpatial();
    Owner owner;
    REQUIRE(RegisterVulkanRenderBackend(owner.registry, owner.observed).HasValue());
    REQUIRE(owner.registry.Seal().HasValue());
    RenderBackendConfig config{.requirePresentation = false, .enableValidation = true};
    config.queueAssignments = {{RenderQueueRole::Graphics, {1}}, {RenderQueueRole::Compute, {1}}, {RenderQueueRole::Transfer, {1}}};
    owner.frontend = Take(RenderFrontend::Create(owner.registry, RenderBackendId{"vulkan"}, config));
    auto &frontend = *owner.frontend;
    REQUIRE(frontend.Capabilities().support.features.Supports(RenderCapability::NativeReconstruction));
    const auto kernel = Take(frontend.RealizeNativeReconstructionKernel(cooked));
    const RenderTextureDescriptor source{.extent = {17, 9},
                                         .format = RenderTextureFormat::Rgba16Float,
                                         .usage = Sampled | RenderAttachment};
    const RenderTextureDescriptor destination{.extent = {64, 32},
                                              .format = RenderTextureFormat::Rgba32Float,
                                              .usage = Sampled | Storage | CopySource};
    const auto [input, inputView] = PrepareImage(frontend, source);
    const auto [output, outputView] = PrepareImage(frontend, destination);

    auto builder = Take(RenderGraphBuilder::Create({.maxPasses = 2, .maxResources = 2, .maxUsages = 3, .maxDependencies = 2}));
    const auto clear = Take(builder.AddPass(RenderPassKind::Graphics, RenderQueueRole::Graphics));
    const auto reconstruct = Take(builder.AddPass(RenderPassKind::Compute, RenderQueueRole::Compute));
    const auto sourceImage = Take(builder.ImportTexture(input, RenderGraphResourceClass::Persistent));
    const auto outputImage = Take(builder.ImportTexture(output, RenderGraphResourceClass::Persistent));
    REQUIRE(builder.AddUsage({clear, sourceImage, RenderGraphAccess::Write, RenderGraphUsageKind::ColorAttachment}).HasValue());
    REQUIRE(builder.AddUsage({reconstruct, sourceImage, RenderGraphAccess::Read, RenderGraphUsageKind::Sampled}).HasValue());
    REQUIRE(builder.AddUsage({reconstruct, outputImage, RenderGraphAccess::Write, RenderGraphUsageKind::Storage}).HasValue());
    const auto graph = Take(builder.Finalize());
    const auto schedule = Take(CompileRenderGraph(graph));
    const std::array initial{RenderGraphImportedState{sourceImage, {}, RenderGraphInitialContents::Discarded},
                             RenderGraphImportedState{outputImage, {}, RenderGraphInitialContents::Discarded}};
    const auto synchronization = Take(SynthesizeRenderGraphSynchronization(graph, schedule, config.queueAssignments, initial));
    const auto compiled = Take(CompileRenderGraphExecution(graph, schedule, synchronization, config.queueAssignments));

    constexpr std::array expected{0.25F, 0.5F, 0.75F, 1.0F};
    RenderGraphNativeReconstruction workload;
    workload.dispatch = {.entry = NativeReconstructionEntry::Spatial,
                         .constants = {.extents = {source.extent.width, source.extent.height, destination.extent.width,
                                                   destination.extent.height},
                                       .sample = {0.0F, 0.0F, 1.0F, 0.0F}}};
    workload.images[static_cast<std::size_t>(NativeReconstructionImage::CurrentColor)] = {sourceImage, inputView};
    workload.images[static_cast<std::size_t>(NativeReconstructionImage::OutputColor)] = {outputImage, outputView};
    workload.kernel = kernel;
    const std::array workloads{RenderGraphPassWorkload{clear, RenderGraphColorAttachment{sourceImage,
                                                                                         {.clearColor = {expected[0], expected[1],
                                                                                                         expected[2], 1.0F}}}},
                               RenderGraphPassWorkload{reconstruct, workload}};
    const std::array submissions{RenderGraphQueueSubmission{RenderQueueSubmission{{1}, {1}, {}, {{1}, 1}}, {0, 2}}};
    auto frame = Take(frontend.BeginFrame({.frameNumber = 1, .outputExtent = destination.extent}));
    const auto recorded = frame.ExecuteSubmittedGraph(compiled, workloads, submissions);
    INFO((recorded.HasError() ? recorded.ErrorValue().message : "native reconstruction recorded"));
    REQUIRE(recorded.HasValue());
    REQUIRE(frame.Present().HasValue());
    RequireCompletion(frontend, submissions.front().submission.signal);
    {
        Test::VfxInstanced::NativeReadback readback{owner.observed, true};
        readback.Prepare();
        const auto pixels = readback.ReadReconstructed();
        REQUIRE(pixels.size() == 64 * 32 * expected.size());
        for (std::size_t i = 0; i < pixels.size(); ++i)
            REQUIRE(pixels[i] == Catch::Approx(expected[i % expected.size()]));
    }
    Test::RequireDrainedOwner(frontend);
}
