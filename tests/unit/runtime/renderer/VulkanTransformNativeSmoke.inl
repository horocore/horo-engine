namespace {
    /** @brief Real-device indexed draw fixture using the canonical typed matrix/array packer. */
    struct NativeTransformFixture : NativePreparationFixture {
        std::uint64_t vertices{}, indices{}, mesh{}, output{}, outputView{}, target{};

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
            const std::array<std::uint32_t, 1> viewIndex{1};
            const std::array<float, 32> transforms{1,    0, 0, 0,     0, 1,    0, 0,      0, 0, 1, 0, 0, 0, 0, 1,
                                                   0.5F, 0, 0, 0.25F, 0, 0.5F, 0, 0.125F, 0, 0, 1, 0, 0, 0, 0, 1};
            const std::array values{MaterialParameterValue{{1}, std::span<const float>{tint}},
                                    MaterialParameterValue{{2}, std::span<const std::uint32_t>{viewIndex}},
                                    MaterialParameterValue{{3}, std::span<const float>{transforms}}};
            auto packed = PackMaterialParameterBinding(layout.Value(), {1}, values);
            REQUIRE(packed.HasValue());
            REQUIRE(packed.Value().bytes.size() == 160);
            return std::move(packed).Value();
        }

        void PrepareDraw(const bool layered = false) {
            const std::array<MeshVertex, 3> vertexData{{{{-1, -1, 0}}, {{1, -1, 0}}, {{0, 1, 0}}}};
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
                                                .format = RenderTextureFormat::Rgba8Unorm,
                                                .layerCount = layered ? 2U : 1U,
                                                .usage = RenderTextureUsage::RenderAttachment};
            const auto cost = backend->QueryTextureMemoryCost(color);
            REQUIRE(cost.HasValue());
            auto createdColor = backend->CreateTexture(color, {}, TestSupport::PlacementFor(cost.Value(), 9));
            REQUIRE(createdColor.HasValue());
            output = createdColor.Value();
            auto createdView = backend->CreateTextureView({.texture = {{1}, 9, 1}}, output);
            REQUIRE(createdView.HasValue());
            outputView = createdView.Value();
            auto createdTarget = backend->CreateRenderTarget({.colorAttachment = {{1}, 10, 1}, .extent = {8, 8}}, outputView, 0);
            REQUIRE(createdTarget.HasValue());
            target = createdTarget.Value();
        }

        void Submit() {
            auto parameters = Parameters();
            buffer = Buffer(parameters.bytes, RenderBufferUsage::Uniform);
            auto material = metadata.Material();
            material.resources[0].value = std::move(parameters);
            const std::array resources{MaterialResidentResource{MaterialResidentResourceKind::Buffer, buffer, 0, 160}};
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
            const RenderMaterialExecutionRequest request{frame.Value(), target, {8, 8}, {}, {}, draws, &lease, &transfer, submission};
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

TEST_CASE("Vulkan native material draw consumes a view-indexed nonidentity transform array", "[vulkan][material][native-smoke]") {
    const auto vertexPath = std::getenv("HORO_VULKAN_TRANSFORM_VERTEX");
    const auto fragmentPath = std::getenv("HORO_VULKAN_TRANSFORM_FRAGMENT");
    if (!vertexPath || !fragmentPath)
        SKIP("Opt-in native transform qualification requires cooked shader paths.");
    NativeTransformFixture fixture;
    fixture.Initialize();
    fixture.metadata.UseMatrixParameters();
    fixture.PrepareShaders(vertexPath, fragmentPath, false);
    fixture.PreparePipeline();
    fixture.PrepareDraw();
    fixture.Submit();
}
