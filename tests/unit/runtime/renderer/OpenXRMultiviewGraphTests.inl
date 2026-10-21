TEST_CASE("OpenXR multiview host graph consumes actual acquisitions and preserves native release obligations",
          "[unit][xr][vulkan][multiview][integration]") {
    using namespace Horo::XR;
    NativeGraphFixture native;
    NativeXRSessionFixture session{false};
    XRTest::SwapchainScript xr;
    RenderBackendRegistry registry;
    REQUIRE(registry.Register({RenderBackendId{"xr-bridge-test"}, "XR bridge test", std::make_unique<ExternalGraphProvider>(native)})
                .HasValue());
    REQUIRE(registry.Seal().HasValue());
    auto frontend = SubmissionTest::Take(RenderFrontend::Create(registry, RenderBackendId{"xr-bridge-test"}, {}));
    NativeXR::OpenXRVulkanSwapchainImages images{*frontend, session.graphics, session.graphics.binding, {1}};
    NativeXR::OpenXRSwapchain swapchain{session.owner, images};
    REQUIRE(images.Bind(swapchain).HasValue());

    struct Cleanup {
        NativeGraphFixture &native;
        RenderFrontend &frontend;
        NativeXR::OpenXRSwapchain &owner;

        ~Cleanup() {
            native.commandState.state.Complete();
            (void)frontend.ObserveSubmission({.completion = {{1}, 1}, .expectedDeviceIncarnation = 11});
            (void)owner.Close();
            (void)frontend.BeginShutdown();
            (void)frontend.PollDrain();
            (void)owner.Close();
        }
    } cleanup{native, *frontend, swapchain};

    const NativeXR::NativeSwapchainRequest request{.target = {session.identity, {1, 1}},
                                                   .texture = {.extent = {640, 480},
                                                               .layerCount = 2,
                                                               .usage = RenderTextureUsage::RenderAttachment}};
    REQUIRE(swapchain.Create(request, XRTest::SwapchainScript::GetProc).HasValue());
    REQUIRE(swapchain.Acquire().HasValue());
    REQUIRE(swapchain.Wait(0).Value());
    const auto acquisition = SubmissionTest::Take(swapchain.Import());
    const auto origin = Horo::Tests::IdentityValue<XRWorldOriginRevision>(1);
    const auto revision = Horo::Tests::IdentityValue<XRViewConfigurationRevision>(1);
    const auto prediction = Horo::Tests::IdentityValue<XRRenderPredictionTime>(1);
    const XRViewConfigurationId configuration{session.identity, {2, 1}};
    const XRCoordinateSpace local{{session.identity, {3, 1}}, XRSpaceKind::Local, origin};
    const XRCoordinateSpace world{{session.identity, {4, 1}}, XRSpaceKind::World, origin};
    XRPoseDescriptor pose{.session = session.identity,
                          .source = {{session.identity, {5, 1}}, XRSpaceKind::View, origin},
                          .target = local,
                          .purpose = XRPosePurpose::PresentationPrediction,
                          .time = {.runtimeSample = Horo::Tests::IdentityValue<XRRuntimeTime>(1), .renderPrediction = prediction},
                          .components = {.positionMeters = {.value = Horo::Math::Vec3{}, .validity = XRPoseComponentValidity::Tracked},
                                         .orientation = {.value = Horo::Math::Quaternion::Identity(),
                                                         .validity = XRPoseComponentValidity::Tracked},
                                         .confidence = XRTrackingConfidence::High,
                                         .loss = XRTrackingLossState::None}};
    std::array<XRViewDescriptor, 2> sourceViews{};
    std::array<XRExternalRenderTargetDescriptor, 2> sourceTargets{};
    for (std::uint32_t index = 0; index < 2; ++index) {
        sourceViews[index] = {.id = {session.identity, {10 + index, 1}},
                              .order = index,
                              .role = index == 0 ? XRViewRole::PrimaryLeft : XRViewRole::PrimaryRight,
                              .pose = pose,
                              .projection = {-1, 1, -1, 1},
                              .recommendedExtent = {640, 480},
                              .maximumExtent = {640, 480}};
        sourceTargets[index] = {.view = sourceViews[index].id,
                                .target = request.target,
                                .image = acquisition.image,
                                .allocationExtent = {640, 480},
                                .renderRectangle = {0, 0, 640, 480},
                                .arrayLayer = index,
                                .arrayLayerCount = 2};
    }
    const std::array acquired{acquisition.image};
    const XRViewRenderPlanDescriptor descriptor{{.session = session.identity,
                                                 .id = configuration,
                                                 .revision = revision,
                                                 .type = XRViewConfigurationType::PrimaryStereo,
                                                 .viewCount = 2},
                                                prediction,
                                                sourceViews,
                                                sourceTargets,
                                                acquired};
    const auto plan = SubmissionTest::Take(XRViewRenderPlan::Create(descriptor, session.identity, configuration, revision, origin, 2, {}));
    pose.source = local;
    pose.target = world;
    const auto transform = SubmissionTest::Take(XRPoseSample::Create(pose, session.identity, origin));
    const std::array transforms{transform, transform};
    const XRMultiviewOwnerState active{session.identity,
                                       configuration,
                                       revision,
                                       origin,
                                       acquired,
                                       {1, 1, 1, 1, 11, 1},
                                       ResolveMultiviewCapabilities(frontend->Capabilities().support)};
    const auto viewScene = SubmissionTest::Take(XRMultiviewScene::Create({plan, transforms, {}, active.renderer}, active));
    native.resourceState.state.bufferRequirements.size = 4096;
    const std::array<MeshVertex, 3> vertexData{{{{-0.2F, -0.2F, -0.75F}}, {{0.2F, -0.2F, -0.75F}}, {{0, 0.2F, -0.75F}}}};
    const std::array<std::uint32_t, 3> indexData{0, 1, 2};
    const auto buffer = [&](const std::span<const std::byte> bytes, const RenderBufferUsage usage) {
        auto created = SubmissionTest::Take(frontend->CreateBuffer({bytes.size(), usage, RenderBufferAccess::HostVisible}, bytes));
        REQUIRE(frontend->ProcessResourceRequests().HasValue());
        REQUIRE(frontend->ResourceOperationResult(created.operation).HasValue());
        return created.handle;
    };
    const auto vertices = buffer(std::as_bytes(std::span{vertexData}), RenderBufferUsage::Vertex);
    const auto indices = buffer(std::as_bytes(std::span{indexData}), RenderBufferUsage::Index);
    const RenderMeshDescriptor meshDescriptor{.vertexBuffer = vertices,
                                              .indexBuffer = indices,
                                              .vertexStride = sizeof(MeshVertex),
                                              .vertexCount = 3,
                                              .indexCount = 3,
                                              .localBounds = {{-0.2F, -0.2F, -0.75F}, {0.2F, 0.2F, -0.75F}}};
    const auto mesh = SubmissionTest::Take(frontend->CreateMesh(meshDescriptor));
    REQUIRE(frontend->ProcessResourceRequests().HasValue());
    REQUIRE(frontend->ResourceOperationResult(mesh.operation).HasValue());
    const std::array<std::uint32_t, 2> routing{};
    auto packed = SubmissionTest::Take(
        PackMultiviewShaderParameters(MultiviewTest::Layout(), viewScene.Scene().Views(), Horo::Math::Mat4::Identity(), routing));
    packed.bytes.resize(224, std::byte{});
    const auto uniform = buffer(packed.bytes, RenderBufferUsage::Uniform);
    Detail::VulkanPreparedMaterialBinding nativeMaterial;
    nativeMaterial.device = NativeHandle<VkDevice>(1);
    nativeMaterial.deviceIncarnation = 11;
    nativeMaterial.owner = &nativeMaterial;
    nativeMaterial.pipelineBacking = std::make_shared<int>(1);
    nativeMaterial.pipeline = NativeHandle<VkPipeline>(201);
    nativeMaterial.layout = NativeHandle<VkPipelineLayout>(202);
    nativeMaterial.sets = {NativeHandle<VkDescriptorSet>(203)};
    nativeMaterial.state.depthFormat.reset();
    nativeMaterial.state.depthTest = false;
    nativeMaterial.state.depthWrite = false;
    MultiviewTest::BorrowedMaterialBackend materialBackend;
    materialBackend.native = &nativeMaterial;
    auto materials = SubmissionTest::Take(MaterialBindingTable::Create(acquisition.texture.owner, materialBackend));
    const MaterialBindingDescriptor binding{.material = {1},
                                            .revision = 1,
                                            .pipeline = {acquisition.texture.owner, 99, 1},
                                            .layout = MultiviewTest::Layout(),
                                            .resources = {{{1}, 0, MaterialBufferBinding{uniform, 0, 224}}}};
    const auto selection = SubmissionTest::Take(materials->Publish(binding));
    const auto material = SubmissionTest::Take(materials->Acquire(selection.binding));
    const std::array instances{
        MultiviewSceneInstance{mesh.handle, material->Id(), Horo::Math::Mat4::Identity(), meshDescriptor.localBounds}};
    const auto scene = SubmissionTest::Take(viewScene.ExtractInstances(instances, active, MultiviewExecutionPolicy::Multipass));
    const std::array geometry{NativeXR::OpenXRMultiviewGeometry{0, meshDescriptor, material}};
    std::array<NativeXR::OpenXRMultiviewTargetViews, 2> views{};
    for (std::uint32_t index = 0; index < 2; ++index) {
        auto view = SubmissionTest::Take(frontend->CreateTextureView({.texture = acquisition.texture, .baseLayer = index}));
        REQUIRE(frontend->ProcessResourceRequests().HasValue());
        views[index].color = view.handle;
    }
    std::array<NativeXR::OpenXRSwapchain *, 1> owners{&swapchain};
    auto prepared = SubmissionTest::Take(NativeXR::OpenXRMultiviewGraph::Create(*frontend, scene, active, owners, geometry, views, {1}));
    CHECK(prepared.Graph().ExternalReleases().size() == 1);
    auto frame = SubmissionTest::Take(frontend->BeginFrame({1, {640, 480}}));
    const auto result = prepared.Execute(frame, active, {{1}, {1}, {}, {{1}, 1}});
    INFO(result.HasError() ? result.ErrorValue().message : "actual acquired images rendered");
    REQUIRE(result.HasValue());
    REQUIRE(frame.Present().HasValue());
    CHECK(native.encoded.renderingBegins == 2);
    REQUIRE(native.encoded.drawCalls.size() == 2);
    CHECK(native.encoded.drawCalls[0].firstView == 0);
    CHECK(native.encoded.drawCalls[1].firstView == 1);
    REQUIRE(frontend->ReleaseMesh(mesh.handle).HasValue());
    REQUIRE(frontend->ReleaseBuffer(vertices).HasValue());
    REQUIRE(frontend->ReleaseBuffer(indices).HasValue());
    REQUIRE(frontend->ReleaseBuffer(uniform).HasValue());
    for (const auto &view : views)
        REQUIRE(frontend->ReleaseTextureView(view.color).HasValue());
    REQUIRE_FALSE(swapchain.PollRelease().Value());
    CHECK(xr.releases == 0);
    native.commandState.state.Complete();
    REQUIRE(frontend->ObserveSubmission({.completion = {{1}, 1}, .expectedDeviceIncarnation = 11}).Value());
    REQUIRE(swapchain.PollRelease().Value());
    CHECK(xr.releases == 1);
    CHECK(native.resourceState.state.destroyImageCount == 0);
    CHECK(native.commandState.state.waitCalls == 0);
    REQUIRE(swapchain.Acquire().HasValue());
    REQUIRE(swapchain.Wait(0).Value());
    REQUIRE(swapchain.Import().HasValue());
    auto replacementFrame = SubmissionTest::Take(frontend->BeginFrame({2, {640, 480}}));
    CHECK(prepared.Execute(replacementFrame, active, {{1}, {2}, {}, {{1}, 2}}).HasError());
    REQUIRE(swapchain.PollRelease().Value());
    REQUIRE(swapchain.Close().Value());
}
