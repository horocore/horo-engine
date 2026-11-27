namespace {
    /** @brief Real-device indexed draw fixture using the canonical typed matrix/array packer. */
    struct NativeTransformFixture : NativePreparationFixture {
        std::uint64_t vertices{}, indices{}, mesh{}, output{}, outputView{}, target{}, depthImage{}, depthAttachment{};

        std::uint64_t Buffer(const std::span<const std::byte> bytes, const RenderBufferUsage usage) {
            const RenderBufferDescriptor descriptor{bytes.size(), usage, RenderBufferAccess::HostVisible};
            const auto cost = backend->QueryBufferMemoryCost(descriptor);
            REQUIRE(cost.HasValue());
            auto created = backend->CreateBuffer(descriptor, bytes, TestSupport::PlacementFor(cost.Value(), 1));
            REQUIRE(created.HasValue());
            return created.Value();
        }

        MaterialParameterBinding Parameters() {
            auto layout = PrepareMaterialBindingLayout(metadata.reflection);
            REQUIRE(layout.HasValue());
            const std::array<float, 4> tint{0.25F, 0.5F, 0.75F, 1};
            const std::array<std::uint32_t, 1> viewIndex{31};
            std::array<float, 512> transforms{};
            for (std::size_t view = 0; view < 32; ++view) {
                transforms[view * 16] = 0.5F;
                transforms[view * 16 + 5] = 0.5F;
                transforms[view * 16 + 10] = 1;
                transforms[view * 16 + 15] = 1;
                transforms[view * 16 + 3] = static_cast<float>(view) / 64;
                transforms[view * 16 + 7] = static_cast<float>(view) / 128;
            }
            const std::array<float, 16> localToWorld{1, 0, 0, -0.25F, 0, 1, 0, -0.125F, 0, 0, 1, 0, 0, 0, 0, 1};
            const std::array values{MaterialParameterValue{{1}, std::span<const float>{tint}},
                                    MaterialParameterValue{{2}, std::span<const std::uint32_t>{viewIndex}},
                                    MaterialParameterValue{{3}, std::span<const float>{transforms}},
                                    MaterialParameterValue{{4}, std::span<const float>{localToWorld}}};
            auto packed = PackMaterialParameterBinding(layout.Value(), {1}, values);
            REQUIRE(packed.HasValue());
            REQUIRE(packed.Value().bytes.size() == 2144);
            return std::move(packed).Value();
        }

        void PrepareDraw(const bool layered = false, const Math::Vec3 normal = {}, const RenderGraphicsPipelineState *guide = nullptr) {
            const std::array<MeshVertex, 3> vertexData{{{{-1, -1, 0}, normal}, {{1, -1, 0}, normal}, {{0, 1, 0}, normal}}};
            const std::array<std::uint32_t, 3> indexData{0, 1, 2};
            vertices = Buffer(std::as_bytes(std::span{vertexData}), RenderBufferUsage::Vertex);
            indices = Buffer(std::as_bytes(std::span{indexData}), RenderBufferUsage::Index);
            RenderMeshDescriptor descriptor{.vertexBuffer = {{1}, 7, 1},
                                            .indexBuffer = {{1}, 8, 1},
                                            .vertexStride = sizeof(MeshVertex),
                                            .vertexCount = 3,
                                            .indexCount = 3,
                                            .localBounds = {{-1, -1, 0}, {1, 1, 0}}};
            auto createdMesh = backend->CreateMesh(descriptor, vertices, indices);
            REQUIRE(createdMesh.HasValue());
            mesh = createdMesh.Value();
            const RenderTextureDescriptor color{.extent = {8, 8},
                                                .format = guide ? guide->colorFormat : RenderTextureFormat::Rgba8Unorm,
                                                .layerCount = layered ? 2U : 1U,
                                                .usage = RenderTextureUsage::RenderAttachment};
            const auto cost = backend->QueryTextureMemoryCost(color);
            REQUIRE(cost.HasValue());
            auto createdColor = backend->CreateTexture(color, {}, TestSupport::PlacementFor(cost.Value(), 9));
            REQUIRE(createdColor.HasValue());
            output = createdColor.Value();
            auto createdView = backend->CreateTextureView({.texture = {{1}, 9, 1}, .format = color.format}, output);
            REQUIRE(createdView.HasValue());
            outputView = createdView.Value();
            RenderTargetDescriptor attachments{.colorAttachment = {{1}, 10, 1}, .extent = {8, 8}};
            if (guide && guide->depthFormat) {
                const RenderTextureDescriptor depth{.extent = {8, 8},
                                                    .format = *guide->depthFormat,
                                                    .usage = RenderTextureUsage::RenderAttachment};
                const auto depthCost = backend->QueryTextureMemoryCost(depth);
                REQUIRE(depthCost.HasValue());
                const auto image = backend->CreateTexture(depth, {}, TestSupport::PlacementFor(depthCost.Value(), 11));
                REQUIRE(image.HasValue());
                depthImage = image.Value();
                const auto view =
                    backend->CreateTextureView({.texture = {{1}, 11, 1}, .format = depth.format, .aspect = RenderTextureAspect::Depth},
                                               depthImage);
                REQUIRE(view.HasValue());
                depthAttachment = view.Value();
                attachments.depthAttachment = {{1}, 12, 1};
            }
            auto createdTarget = backend->CreateRenderTarget(attachments, outputView, depthAttachment);
            REQUIRE(createdTarget.HasValue());
            target = createdTarget.Value();
        }

        void Submit() {
            Submit(Parameters());
        }

        void Submit(MaterialParameterBinding parameters) {
            auto material = metadata.Material();
            const auto nativeSize = backend->QueryMaterialParameterBufferSize(native, material, {1});
            REQUIRE(nativeSize.HasValue());
            REQUIRE(nativeSize.Value() >= parameters.bytes.size());
            std::vector<std::byte> padded(nativeSize.Value());
            std::ranges::copy(parameters.bytes, padded.begin());
            buffer = Buffer(padded, RenderBufferUsage::Uniform);
            material.resources[0].value = std::move(parameters);
            const std::array resources{MaterialResidentResource{MaterialResidentResourceKind::Buffer, buffer, 0, padded.size()}};
            auto realized = backend->RealizeResolved(material, native, resources);
            INFO((realized.HasError() ? realized.ErrorValue().message : "transform material realized"));
            REQUIRE(realized.HasValue());
            auto table = std::move(realized).Value();

            struct Lease final : IRenderGraphResourceLease {
                std::size_t releases{};

                void Release() noexcept override {
                    ++releases;
                }
            } lease;

            struct DrainGuard {
                IRenderBackend &backend;

                ~DrainGuard() {
                    if (backend.BeginShutdown().HasError())
                        std::terminate();
                    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
                    for (;;) {
                        auto drained = backend.PollDrain();
                        if (drained.HasValue() && drained.Value() == RenderBackendDrainProgress::Drained)
                            break;
                        if (std::chrono::steady_clock::now() >= deadline)
                            std::terminate();
                        std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    }
                }
            } drain{*backend};

            const auto frame = backend->BeginFrame({.outputExtent = {8, 8}});
            REQUIRE(frame.HasValue());
            const std::array draws{RenderMaterialDrawInstance{mesh, table.get()}};
            RenderGraphLeaseTransfer transfer;
            const RenderQueueSubmission submission{{1}, {1}, {}, {{1}, 1}};
            const auto depthClear = depthAttachment ? std::optional{1.0F} : std::nullopt;
            const RenderMaterialExecutionRequest request{frame.Value(), target, {8, 8},    {},        depthClear,
                                                         draws,         &lease, &transfer, submission};
            auto consumed = backend->ExecuteMaterialPass(request);
            INFO((consumed.HasError() ? consumed.ErrorValue().message : "transform draw recorded"));
            REQUIRE(consumed.HasValue());
            REQUIRE(transfer.authority == RenderGraphLeaseAuthority::BackendCompletion);
            REQUIRE(lease.releases == 0);
            REQUIRE(backend->Present(frame.Value()).HasValue());
            REQUIRE(lease.releases == 0);
            // Registry discovery may end while the canonical submitted ticket still owns native backing.
            backend->DestroyRenderTarget(target);
            backend->DestroyTextureView(outputView);
            backend->DestroyTexture(output);
            if (depthAttachment) {
                backend->DestroyTextureView(depthAttachment);
                backend->DestroyTexture(depthImage);
            }
            backend->DestroyMesh(mesh);
            backend->DestroyBuffer(vertices);
            backend->DestroyBuffer(indices);
            backend->DestroyBuffer(buffer);
            backend->DestroyGraphicsPipeline(native);
            const RenderCpuWait wait{{{1}, 1},
                                     RenderCpuWaitPurpose::DeterministicTest,
                                     std::chrono::seconds(5),
                                     backend->Capabilities().support.deviceIncarnation};
            auto completed = backend->ObserveSubmission(wait);
            INFO((completed.HasError() ? completed.ErrorValue().message : "transform submission completed"));
            REQUIRE(completed.HasValue());
            REQUIRE(completed.Value());
            REQUIRE(lease.releases == 1);
            table.reset();
        }
    };
}  // namespace

TEST_CASE("Vulkan native material draw consumes view 31 from 32 transforms separately from an object transform",
          "[vulkan][material][native-smoke]") {
    const auto vertexPath = std::getenv("HORO_VULKAN_TRANSFORM_VERTEX");
    const auto fragmentPath = std::getenv("HORO_VULKAN_TRANSFORM_FRAGMENT");
    if (!vertexPath || !fragmentPath)
        SKIP("Opt-in native transform qualification requires cooked shader paths.");
    NativeTransformFixture fixture;
    fixture.Initialize();
    fixture.metadata.UseNativeViewTransforms();
    fixture.PrepareShaders(vertexPath, fragmentPath, false);
    fixture.PreparePipeline();
    fixture.PrepareDraw();
    fixture.Submit();
}
