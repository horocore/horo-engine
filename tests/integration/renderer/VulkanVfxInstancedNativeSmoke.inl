/** @file VulkanVfxInstancedNativeSmoke.inl @brief Genuine indexed instanced VFX pixels through the registered Vulkan backend. */
namespace VulkanVfxInstancedSmoke {
    using VulkanVfxGpuSmoke::Accepted;
    using VulkanVfxGpuSmoke::Read;
    using VulkanVfxGpuSmoke::Value;
    using namespace Horo::Render::Test::VfxInstanced;
    constexpr FramebufferExtent Extent{64, 32};
    constexpr Math::Vec3 Eye{.7F, .25F, .5F}, Left{-.6F, 0, -3}, Right{.6F, 0, -3};
    constexpr float AuthoredZ = .55F;

    /** @brief Uses only the genuine explicitly provided ordinary/masked Vulkan package. */
    std::filesystem::path Package() {
        const auto configured = std::getenv("HORO_VFX_INSTANCED_ASSET_ROOT");
        if (!configured || !*configured)
            SKIP("Supply the genuine Vulkan instanced ordinary/masked cooked family.");
        const std::filesystem::path root{configured};
        REQUIRE(root.is_absolute());
        return root;
    }

    /** @brief Keeps canonical manifest and real same-operation reflection/payload evidence together. */
    struct Program final {
        ShaderManifest manifest;
        ShaderTargetRequirement target;
        CompiledShaderArtifact artifact;
    };

    /** @brief Decodes the actual package; no target binding or reflection layout is authored by the fixture. */
    Program Load(const std::filesystem::path &root, bool masked) {
        const ShaderTargetRequirement target{ShaderTargetBackend::Vulkan, ShaderPayloadFormat::SpirV16, 1, 1, 4, 0, false, false};
        auto manifest = MakeVfxShaderManifest(target);
        const std::string prefix = masked ? "shaders/runtime/vfx-instanced-masked-vulkan" : "shaders/runtime/vfx-instanced-vulkan";
        auto payload = Read(root / (prefix + ".horoshdr"));
        const auto metadata = Read(root / (prefix + ".reflection.json"));
        auto bound = Value(DecodeReflectedShaderArtifact(manifest, target, payload, metadata));
        CompiledShaderArtifact artifact{.backend = target.backend,
                                        .payloadFormat = target.payloadFormat,
                                        .artifactKey = bound.compiledArtifactKey,
                                        .payload = std::move(payload),
                                        .reflection = std::make_shared<const ReflectedShaderArtifact>(std::move(bound))};
        return {std::move(manifest), target, std::move(artifact)};
    }

    /** @brief Keeps the real delegate/observer and registry alive through ordinary frontend drain. */
    struct Owner final {
        SystemLoader system;
        ObservedLoader loader{system};
        RenderBackendRegistry registry;
        std::unique_ptr<RenderFrontend> frontend;
        std::array<RenderQueueAssignment, 2> queues{{{RenderQueueRole::Graphics, {1}}, {RenderQueueRole::Transfer, {1}}}};

        /** @brief Initializes an actual headless backend, with explicitly aliased roles and truthful driver capabilities. */
        Owner() {
            try {
                Initialize();
            } catch (...) {
                Close();
                throw;
            }
        }

        /** @brief Completes actual backend creation while partial-construction cleanup remains live. */
        void Initialize() {
            Accepted(RegisterVulkanRenderBackend(registry, loader));
            Accepted(registry.Seal());
            RenderBackendConfig config{.requirePresentation = false, .enableValidation = true};
            config.queueAssignments.assign(queues.begin(), queues.end());
            frontend = Value(RenderFrontend::Create(registry, RenderBackendId{"vulkan"}, config));
            REQUIRE_FALSE(frontend->Capabilities().support.synthetic);
            REQUIRE(frontend->Capabilities().support.features.Supports(RenderCapability::VfxForwardInstancing));
        }

        /** @brief Retains loader/native parents through the existing bounded real backend drain, including assertion unwinding. */
        ~Owner() {
            Close();
        }

        /** @brief Completes bounded ordinary shutdown before any native loader parents can be destroyed. */
        void Close() noexcept {
            if (!frontend)
                return;
            if (frontend->BeginShutdown().HasError())
                std::terminate();
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
            while (std::chrono::steady_clock::now() < deadline) {
                const auto progress = frontend->PollDrain();
                if (progress.HasValue() && progress.Value() == RenderBackendDrainProgress::Drained) {
                    frontend.reset();
                    return;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds{1});
            }
            std::terminate();
        }
    };

    /** @brief Observes exact creation operations rather than treating a nonzero handle or process count as readiness. */
    template <class Handle> Handle Ready(RenderFrontend &frontend, Result<ResourceCreation<Handle>> result) {
        const auto creation = Value(std::move(result));
        Value(frontend.ProcessResourceRequests());
        Accepted(frontend.ResourceOperationResult(creation.operation));
        REQUIRE(Value(frontend.ResourceState(creation.handle)) == RenderResourceState::Ready);
        return creation.handle;
    }

    /** @brief Exact resident indexed geometry and its actual graph-importable buffer generations. */
    struct Mesh final {
        RenderMeshHandle mesh;
        RenderBufferHandle vertices, indices;
    };

    /** @brief Creates the unit indexed billboard source or authored-Z mesh through ordinary frontend resource operations. */
    Mesh Geometry(RenderFrontend &frontend, float z) {
        std::array<MeshVertex, 4> vertices{{{{-.5F, -.5F, z}, {0, 0, 1}, {0, 0}},
                                            {{.5F, -.5F, z}, {0, 0, 1}, {1, 0}},
                                            {{.5F, .5F, z}, {0, 0, 1}, {1, 1}},
                                            {{-.5F, .5F, z}, {0, 0, 1}, {0, 1}}}};
        if (z != 0)
            for (auto &vertex : vertices)
                vertex.uv = {.75F, .25F};
        constexpr std::array<std::uint32_t, 6> indices{0, 1, 2, 2, 3, 0};
        const auto vertex = Ready(frontend, frontend.CreateBuffer({.byteSize = sizeof(vertices),
                                                                   .usage = RenderBufferUsage::Vertex,
                                                                   .access = RenderBufferAccess::HostVisible},
                                                                  std::as_bytes(std::span{vertices})));
        const auto index = Ready(frontend, frontend.CreateBuffer({.byteSize = sizeof(indices),
                                                                  .usage = RenderBufferUsage::Index,
                                                                  .access = RenderBufferAccess::HostVisible},
                                                                 std::as_bytes(std::span{indices})));
        const auto mesh = Ready(frontend, frontend.CreateMesh({.vertexBuffer = vertex,
                                                               .indexBuffer = index,
                                                               .vertexStride = sizeof(MeshVertex),
                                                               .vertexCount = 4,
                                                               .indexCount = 6,
                                                               .localBounds = {{-.5F, -.5F, z}, {.5F, .5F, z}}}));
        return {mesh, vertex, index};
    }

    /** @brief Actual material, image-upload source and issued parameter slot; owns no native handles. */
    struct Material final {
        MaterialRuntimeId id;
        RenderBufferHandle parameters, upload;
        RenderTextureHandle texture;
        RenderTextureViewHandle view;
        FramebufferExtent extent;
        std::size_t bytes{};
        std::unique_ptr<MaterialBindingTable> table;
        std::shared_ptr<PreparedVfxFrameSlot> slot;
    };

    /** @brief Forms exact selected-device pipeline compatibility from the actual cooked artifact and declared draw state. */
    RenderGraphicsPipelineDescriptor PipelineDescriptor(RenderFrontend &frontend, const Program &program, RenderShaderModuleHandle vertex,
                                                        RenderShaderModuleHandle fragment, const RenderGraphicsPipelineState &state) {
        const auto device = Value(frontend.QueryPipelineDeviceIdentity());
        const auto digest = Value(ComputeRenderGraphicsPipelineStateDigest(state));
        const auto &reflection = program.artifact.reflection->reflection;
        return {vertex,
                fragment,
                program.manifest,
                program.target,
                reflection,
                state,
                {1, device.backend, device.backendModuleVersion, device.adapter, device.deviceCompatibilityDigest, device.driverIdentity,
                 device.driverVersion, program.target.backend, program.target.payloadFormat, program.artifact.reflection->cookedArtifactKey,
                 reflection.interfaceCompatibility, digest}};
    }

    /** @brief Creates ordinary or genuinely masked indexed pipelines with the exact required opaque/alpha/additive depth policy. */
    RenderPipelineHandle Pipeline(RenderFrontend &frontend, const Program &program, std::uint32_t role) {
        const auto &reflection = program.artifact.reflection->reflection;
        const auto vertex =
            Ready(frontend, frontend.CreateShaderModule({program.target, ShaderStage::Vertex, "VSMain",
                                                         program.artifact.reflection->cookedArtifactKey, reflection.interfaceCompatibility},
                                                        program.artifact.payload));
        const auto fragment =
            Ready(frontend, frontend.CreateShaderModule({program.target, ShaderStage::Fragment, "PSMain",
                                                         program.artifact.reflection->cookedArtifactKey, reflection.interfaceCompatibility},
                                                        program.artifact.payload));
        RenderGraphicsPipelineState state;
        state.colorFormat = RenderTextureFormat::Rgba16Float;
        state.depthFormat = RenderTextureFormat::Depth32Float;
        state.cull = RenderCullMode::None;
        state.depthTest = true;
        state.depthWrite = role != 2 && role != 4;
        state.blend = role == 2 ? RenderBlendMode::Alpha : role == 4 ? RenderBlendMode::Additive : RenderBlendMode::Disabled;
        return Ready(frontend, frontend.CreateGraphicsPipeline(PipelineDescriptor(frontend, program, vertex, fragment, state)));
    }

    /** @brief Creates initialized host staging and empty canonical sampled textures for real in-graph upload. */
    void Sample(RenderFrontend &frontend, Material &material, std::uint32_t role) {
        const std::array<std::byte, 4> texel{std::byte{255}, std::byte{255}, std::byte{255}, role == 3 ? std::byte{0} : std::byte{255}};
        const std::array<std::byte, 16> atlas{std::byte{32},  std::byte{32},  std::byte{32},  std::byte{255},
                                              std::byte{128}, std::byte{128}, std::byte{128}, std::byte{255},
                                              std::byte{64},  std::byte{64},  std::byte{64},  std::byte{255},
                                              std::byte{255}, std::byte{255}, std::byte{255}, std::byte{255}};
        material.extent = role == 1 ? FramebufferExtent{2, 2} : FramebufferExtent{1, 1};
        const auto bytes = role == 1 ? std::span<const std::byte>{atlas} : std::span<const std::byte>{texel};
        material.bytes = bytes.size();
        material.upload = Ready(frontend, frontend.CreateBuffer({.byteSize = bytes.size(),
                                                                 .usage = RenderBufferUsage::CopySource,
                                                                 .access = RenderBufferAccess::HostVisible},
                                                                bytes));
        material.texture =
            Ready(frontend, frontend.CreateTexture({.extent = material.extent,
                                                    .format = RenderTextureFormat::Rgba8Unorm,
                                                    .usage = RenderTextureUsage::Sampled | RenderTextureUsage::CopyDestination}));
        material.view = Ready(frontend, frontend.CreateTextureView({.texture = material.texture}));
    }

    /** @brief Packs only immutable material tint/cutoff through real reflection, never fixture-authored offsets. */
    MaterialParameterBinding Constants(const MaterialBindingLayout &layout) {
        const std::array<float, 4> tint{1, 1, 1, 1};
        const std::array<float, 1> cutoff{.5F};
        const std::array parameters{MaterialParameterValue{VfxShaderParameterIds::Tint, std::span<const float>{tint}},
                                    MaterialParameterValue{VfxShaderParameterIds::AlphaCutoff, std::span<const float>{cutoff}}};
        return Value(PackMaterialParameterBinding(layout, VfxShaderParameterIds::Material, parameters));
    }

    /** @brief Acquires a unique real frontend parameter slot for each independent scene material. */
    Material MakeMaterial(RenderFrontend &frontend, const Program &program, std::uint32_t role) {
        Material material;
        material.id = {100 + role};
        const auto pipeline = Pipeline(frontend, program, role);
        const auto layout = Value(PrepareMaterialBindingLayout(program.artifact.reflection->reflection));
        const auto bytes = Value(VfxShaderParameterBytes(layout));
        material.parameters =
            Ready(frontend,
                  frontend.CreateBuffer({.byteSize = bytes, .usage = RenderBufferUsage::Uniform, .access = RenderBufferAccess::HostVisible},
                                        {}));
        Sample(frontend, material, role);
        const auto sampler = Ready(frontend, frontend.CreateSampler({}));
        material.table = Value(MaterialBindingTable::Create(pipeline.owner, frontend));
        const auto published = Value(material.table->Publish(
            {.material = material.id,
             .revision = 1,
             .pipeline = pipeline,
             .layout = layout,
             .resources = {{VfxShaderParameterIds::Instances, 0, MaterialBufferBinding{material.parameters, 0, bytes}},
                           {VfxShaderParameterIds::Material, 0, Constants(layout)},
                           {VfxShaderParameterIds::BaseColor, 0, material.view},
                           {VfxShaderParameterIds::BaseColorSampler, 0, sampler}}}));
        material.slot = Value(frontend.PrepareVfxFrameSlot(Value(material.table->Acquire(published.binding))));
        return material;
    }

    /** @brief Derives Vulkan clip-zero-to-one and its actual issued view without borrowing particle packer transforms. */
    VfxDrawView View(RenderFrontend &frontend) {
        const auto view = Value(Math::TryLookAt(Eye, {0, 0, -3}, {0, 1, 0}));
        const auto projection = Value(Math::TryPerspective(1.1F, 2, .2F, 80, Math::ClipDepthRange::ZeroToOne));
        const auto identity = Value(frontend.CreateRenderViewIdentity());
        return {.viewProjection = Math::Multiply(projection, view),
                .right = {view.At(0, 0), view.At(0, 1), view.At(0, 2)},
                .up = {view.At(1, 0), view.At(1, 1), view.At(1, 2)},
                .scene = 17,
                .frame = 1,
                .originRevision = 1,
                .view = identity.value,
                .viewGeneration = 1};
    }

    /** @brief Positions the actual authored layers along the reference eye ray. */
    Math::Vec3 AlongRay(Math::Vec3 point, float scale) {
        return Eye + (point - Eye) * scale;
    }

    /** @brief Isolates authored per-particle positions from the actual renderer's billboard transform. */
    Math::Vec3 Position(std::uint32_t role, std::uint32_t particle) {
        const std::array positions{Left, Right, AlongRay(Left, .7F), AlongRay(Left, .55F), AlongRay(Left, .5F)};
        if (particle == 0)
            return positions[role];
        return role == 0 ? Math::Vec3{-.6F, -1.1F, -3} : AlongRay(Right + Math::Vec3{0, 0, AuthoredZ}, 1.5F);
    }

    /** @brief Preserves the GL oracle's authored ray-relative layer sizes. */
    float Size(std::uint32_t role, std::uint32_t particle) {
        if (role == 2)
            return .8F * (particle == 1 ? 1.5F : .7F);
        if (role == 3)
            return .8F * .55F;
        return .8F * (role == 4 ? .5F : 1);
    }

    /** @brief Owns the actual CPU world and canonical frontend geometry, attachments and material slots. */
    struct Scene final {
        Mesh billboard, mesh;
        RenderTextureHandle color, depth;
        RenderTargetHandle target;
        std::vector<Material> materials;
        std::unique_ptr<Horo::Vfx::VfxWorld> world;
        VfxDrawView view;

        /** @brief Creates actual copy-readable color/depth backing through the renderer resource registry. */
        void Target(RenderFrontend &frontend) {
            color =
                Ready(frontend, frontend.CreateTexture({.extent = Extent,
                                                        .format = RenderTextureFormat::Rgba16Float,
                                                        .usage = RenderTextureUsage::RenderAttachment | RenderTextureUsage::CopySource}));
            depth =
                Ready(frontend, frontend.CreateTexture({.extent = Extent,
                                                        .format = RenderTextureFormat::Depth32Float,
                                                        .usage = RenderTextureUsage::RenderAttachment | RenderTextureUsage::CopySource}));
            const auto colorView =
                Ready(frontend, frontend.CreateTextureView({.texture = color, .format = RenderTextureFormat::Rgba16Float}));
            const auto depthView =
                Ready(frontend, frontend.CreateTextureView(
                                    {.texture = depth, .format = RenderTextureFormat::Depth32Float, .aspect = RenderTextureAspect::Depth}));
            target = Ready(frontend, frontend.CreateRenderTarget({colorView, depthView, Extent}));
        }

        /** @brief Captures exact authored topology/material routing into genuine emitter preparations. */
        std::array<Horo::Vfx::VfxEmitterPreparation, 5> Emitters(Horo::Vfx::VfxIdentityScope scope) const {
            constexpr std::array passes{VfxParticlePass::Opaque, VfxParticlePass::Opaque, VfxParticlePass::Translucent,
                                        VfxParticlePass::Masked, VfxParticlePass::Additive};
            constexpr std::array blends{MaterialAlphaMode::Opaque, MaterialAlphaMode::Opaque, MaterialAlphaMode::Translucent,
                                        MaterialAlphaMode::Masked, MaterialAlphaMode::Additive};
            std::array<Horo::Vfx::VfxEmitterPreparation, 5> emitters;
            for (std::uint32_t i = 0; i < emitters.size(); ++i)
                emitters[i] = {.effect = {scope, i, 1},
                               .emitter = {scope, i, 1},
                               .particles = {.buffer = {scope, i, 1}, .capacity = 8},
                               .material = materials[i].id,
                               .materialBlend = blends[i],
                               .mesh = i == 1             ? mesh.mesh
                                       : i == 0 || i == 3 ? billboard.mesh
                                                          : RenderMeshHandle{},
                               .blend = passes[i],
                               .topology =
                                   i == 2 || i == 4 ? VfxPrimitiveTopology::CameraFacingBillboard : VfxPrimitiveTopology::InstancedMesh};
            return emitters;
        }

        /** @brief Seeds real CPU particles with independently authored sentinel attributes for native rendering. */
        void Populate(const Horo::Vfx::VfxEmitterPreparation &emitter, std::uint32_t role) {
            Accepted(world->Play(emitter.effect));
            auto *buffer = Value(world->Particles(emitter.effect, emitter.emitter));
            const auto count = role == 0 || role == 2 ? 2U : 1U;
            for (std::uint32_t i = 0; i < count; ++i)
                Value(buffer->Spawn(Value(Horo::Vfx::ParticleSimulationId::Create(role * 8 + i + 1))));
            auto particles = Value(buffer->View());
            constexpr std::array<std::uint32_t, 5> colors{0xFF0000FF, 0x00FF00FF, 0x0000FF80, 0xFFFFFFFF, 0x00400000};
            for (std::uint32_t i = 0; i < count; ++i) {
                const auto position = Position(role, i);
                particles.positionX[i] = position.x;
                particles.positionY[i] = position.y;
                particles.positionZ[i] = position.z;
                particles.sizeX[i] = Size(role, i);
                particles.sizeY[i] = Size(role, i);
                particles.rotation[i] = role == 1 ? .35F : .17F;
                particles.packedColor[i] = i == 1 && role == 0 ? 0xFFFF00FF : colors[role];
            }
        }

        /** @brief Realizes the exact seven-particle, five-material source scene with ordinary registered owners. */
        Scene(RenderFrontend &frontend, const std::filesystem::path &root) : view(View(frontend)) {
            Accepted(frontend.PrepareVfxScheduling({64, 1, 1'000'000, 16}));
            billboard = Geometry(frontend, 0);
            mesh = Geometry(frontend, AuthoredZ);
            Target(frontend);
            const auto ordinary = Load(root, false), masked = Load(root, true);
            for (std::uint32_t i = 0; i < 5; ++i)
                materials.push_back(MakeMaterial(frontend, i == 3 ? masked : ordinary, i));
            const auto scope = Value(Horo::Vfx::VfxIdentityScope::Create(17));
            const auto emitters = Emitters(scope);
            world = Value(Horo::Vfx::VfxWorld::Prepare(scope, emitters, {16, 64, 128, 2, 1'000'000}));
            for (std::uint32_t i = 0; i < emitters.size(); ++i)
                Populate(emitters[i], i);
        }
    };

    /** @brief Actual graph recipe and ordinary host bindings; no execution plan or publication receipt is fabricated. */
    struct Recipe final {
        RenderGraphBuilder builder;
        VfxGraphSchedule schedule;
        std::vector<VfxGraphDrawBinding> bindings;
        std::vector<RenderGraphPassWorkload> uploads;
        RenderGraphPassRef clearColor, clearDepth;
        RenderGraphResourceId color, depth;
    };

    /** @brief Appends canonical bounded staging-to-image transfers before any actual sampled VFX draw. */
    void Uploads(Scene &scene, Recipe &recipe) {
        for (const auto &material : scene.materials) {
            const auto source = Value(recipe.builder.ImportBuffer(material.upload, RenderGraphResourceClass::Persistent));
            const auto destination = Value(recipe.builder.ImportTexture(material.texture, RenderGraphResourceClass::Persistent));
            const auto pass = Value(recipe.builder.AddPass(RenderPassKind::Copy, RenderQueueRole::Graphics));
            Accepted(recipe.builder.AddUsage({pass, source, RenderGraphAccess::Read, RenderGraphUsageKind::CopySource}));
            Accepted(recipe.builder.AddUsage({pass, destination, RenderGraphAccess::Write, RenderGraphUsageKind::CopyDestination}));
            Accepted(recipe.builder.AddDependency({pass, recipe.clearColor, RenderGraphDependencyKind::ExecutionOrder}));
            recipe.uploads.push_back({pass, RenderGraphTextureUpload{source, destination, material.extent, material.bytes}});
        }
    }

    /** @brief Finds the one existing uploaded graph generation for the actual sampled texture; importing again would fork its state. */
    const RenderGraphTextureUpload &TextureUpload(const Scene &scene, const Recipe &recipe, RenderTextureHandle texture) {
        const auto material = std::ranges::find(scene.materials, texture, &Material::texture);
        REQUIRE(material != scene.materials.end());
        REQUIRE(recipe.uploads.size() == scene.materials.size());
        const auto index = static_cast<std::size_t>(material - scene.materials.begin());
        return std::get<RenderGraphTextureUpload>(recipe.uploads[index].workload);
    }

    /** @brief Binds an exact real material/mesh run while preserving the renderer's actual extracted order. */
    void Bind(Scene &scene, Recipe &recipe) {
        const std::array billboard{Value(recipe.builder.ImportBuffer(scene.billboard.vertices, RenderGraphResourceClass::Persistent)),
                                   Value(recipe.builder.ImportBuffer(scene.billboard.indices, RenderGraphResourceClass::Persistent))};
        const std::array authored{Value(recipe.builder.ImportBuffer(scene.mesh.vertices, RenderGraphResourceClass::Persistent)),
                                  Value(recipe.builder.ImportBuffer(scene.mesh.indices, RenderGraphResourceClass::Persistent))};
        for (const auto &record : recipe.schedule.Workloads()) {
            const auto &work = std::get<RenderGraphVfx>(record.workload);
            for (std::size_t first = 0; first < work.order->instances.size();) {
                const auto batchIndex = work.order->instances[first].batch;
                const auto &batch = work.snapshot.Batches()[batchIndex];
                auto material = std::ranges::find(scene.materials, batch.material, &Material::id);
                REQUIRE(material != scene.materials.end());
                const bool mesh = batch.mesh == scene.mesh.mesh;
                const auto &geometry = mesh ? scene.mesh : scene.billboard;
                const auto &buffers = mesh ? authored : billboard;
                const auto &upload = TextureUpload(scene, recipe, material->texture);
                recipe.bindings.push_back(
                    {.stage = work.stage,
                     .firstOrder = static_cast<std::uint32_t>(first),
                     .mesh = geometry.mesh,
                     .target = scene.target,
                     .vertices = buffers[0],
                     .indices = buffers[1],
                     .parameters = Value(recipe.builder.ImportBuffer(material->parameters, RenderGraphResourceClass::Persistent)),
                     .sampledTexture = upload.destination,
                     .slot = std::move(material->slot)});
                CHECK(recipe.bindings.back().sampledTexture == upload.destination);
                while (first < work.order->instances.size() && work.order->instances[first].batch == batchIndex)
                    ++first;
            }
        }
    }

    /** @brief Schedules canonical extraction through the same production scene/order owner used by ordinary hosts. */
    Recipe Schedule(RenderFrontend &frontend, Scene &scene, std::uint64_t frame, bool bind) {
        auto builder = Value(RenderGraphBuilder::Create({.maxPasses = 32, .maxResources = 32, .maxUsages = 128, .maxDependencies = 64}));
        const auto color = Value(builder.ImportTexture(scene.color, RenderGraphResourceClass::Persistent));
        const auto depth = Value(builder.ImportTexture(scene.depth, RenderGraphResourceClass::Persistent));
        const auto clearColor = Value(builder.AddPass(RenderPassKind::Graphics, RenderQueueRole::Graphics));
        const auto clearDepth = Value(builder.AddPass(RenderPassKind::Graphics, RenderQueueRole::Graphics));
        const auto opaque = Value(builder.AddPass(RenderPassKind::Graphics, RenderQueueRole::Graphics));
        const auto output = Value(builder.AddPass(RenderPassKind::Graphics, RenderQueueRole::Graphics));
        Accepted(builder.AddUsage({clearColor, color, RenderGraphAccess::Write, RenderGraphUsageKind::ColorAttachment}));
        Accepted(builder.AddUsage({clearDepth, depth, RenderGraphAccess::Write, RenderGraphUsageKind::DepthStencilAttachment}));
        Accepted(builder.AddDependency({clearColor, clearDepth}));
        Accepted(builder.AddDependency({clearDepth, opaque}));
        Accepted(builder.AddDependency({opaque, output}));
        const VfxGraphRequest request{.scene = 17,
                                      .frame = frame,
                                      .originRevision = 1,
                                      .view = scene.view.view,
                                      .viewGeneration = scene.view.viewGeneration,
                                      .camera = Eye,
                                      .forward = Math::Normalize(Math::Cross(scene.view.up, scene.view.right)),
                                      .maximumInstances = 64,
                                      .remainingDrawCalls = 16,
                                      .opaqueBegin = clearDepth,
                                      .opaqueComplete = opaque,
                                      .outputTransform = output,
                                      .sceneColor = color,
                                      .sceneDepth = depth};
        const auto extracted = scene.world->Extract(frame, {}, 1);
        auto scheduled = frontend.ScheduleVfx(builder, extracted.snapshot, request);
        INFO((scheduled.HasError() ? scheduled.ErrorValue().message : "Actual indexed VFX scene scheduled."));
        REQUIRE(scheduled.HasValue());
        Recipe recipe{std::move(builder), scheduled.Value(), {}, {}, clearColor, clearDepth, color, depth};
        if (bind) {
            Uploads(scene, recipe);
            Bind(scene, recipe);
        }
        return recipe;
    }

    /** @brief Compiles real resource definitions: undefined empty images and already initialized/actually published host buffers. */
    CompiledRenderGraphExecution Compile(Recipe &recipe, const Owner &owner) {
        const auto graph = Value(recipe.builder.Finalize());
        const auto order = Value(CompileRenderGraph(graph));
        std::vector<RenderGraphImportedState> initial;
        for (const auto &resource : graph.Resources()) {
            if (resource.kind == RenderGraphResourceKind::Texture) {
                initial.push_back({resource.id, {}, RenderGraphInitialContents::Discarded});
                continue;
            }
            RenderGraphLogicalState state{RenderGraphSynchronizationAccess::Read,
                                          RenderGraphSynchronizationOperation::GraphicsInputBuffer,
                                          RenderGraphPipelineScope::Graphics,
                                          RenderGraphTextureLayout::NotApplicable,
                                          {1}};
            const auto uploaded = std::ranges::find_if(recipe.uploads, [&](const auto &entry) {
                return std::get<RenderGraphTextureUpload>(entry.workload).source == resource.id;
            });
            if (uploaded != recipe.uploads.end()) {
                state.operation = RenderGraphSynchronizationOperation::CopySource;
                state.scope = RenderGraphPipelineScope::Transfer;
            }
            initial.push_back({resource.id, state, RenderGraphInitialContents::Preserved});
        }
        const auto sync = Value(SynthesizeRenderGraphSynchronization(graph, order, owner.queues, initial));
        return Value(CompileRenderGraphExecution(graph, order, sync, owner.queues));
    }

    /** @brief Copies only existing canonical workload records in the compiler's actual execution order. */
    std::vector<RenderGraphPassWorkload> Workloads(const Recipe &recipe, const CompiledRenderGraphExecution &graph) {
        std::vector<RenderGraphPassWorkload> result;
        for (const auto &pass : graph.Passes()) {
            const auto upload = std::ranges::find(recipe.uploads, pass.pass, &RenderGraphPassWorkload::pass);
            const auto draw = std::ranges::find(recipe.schedule.Workloads(), pass.pass, &RenderGraphPassWorkload::pass);
            if (upload != recipe.uploads.end())
                result.push_back(*upload);
            else if (draw != recipe.schedule.Workloads().end())
                result.push_back(*draw);
            else if (pass.pass == recipe.clearColor)
                result.push_back({pass.pass, RenderGraphColorAttachment{recipe.color, {}}});
            else if (pass.pass == recipe.clearDepth)
                result.push_back({pass.pass, RenderGraphDepthAttachment{recipe.depth, 1}});
            else
                result.push_back({pass.pass, std::monostate{}});
        }
        return result;
    }

    /** @brief Encodes actual indexed work; cancellation never manufactures a committed native result or accepted timeline point. */
    RenderTimelinePoint Execute(Owner &owner, const CompiledRenderGraphExecution &graph, std::span<const RenderGraphPassWorkload> workloads,
                                std::uint64_t frameNumber, bool cancel) {
        owner.loader.ResetDraws();
        auto frame = Value(owner.frontend->BeginFrame({frameNumber, Extent}));
        const auto issued = Value(frame.ReserveSubmission(RenderQueueRole::Graphics));
        const std::array submissions{RenderGraphQueueSubmission{issued, {0, graph.Passes().size()}}};
        Accepted(frame.ExecuteSubmittedGraph(graph, workloads, submissions));
        if (cancel)
            frame.Cancel();
        else {
            Accepted(frame.Present());
            RequireCompletion(*owner.frontend, issued.signal);
        }
        return issued.signal;
    }

    /** @brief Checks actual forwarded native indexed calls and the renderer's separately prepared batch counts. */
    void DrawCounts(const Owner &owner, const Recipe &recipe) {
        std::array<std::uint32_t, 3> count{};
        for (const auto &record : recipe.schedule.Workloads())
            for (const auto &draw : std::get<RenderGraphVfx>(record.workload).order->draws) {
                count[0] += draw.instanceCount;
                ++count[1];
                if (draw.instanceCount > 1)
                    ++count[2];
            }
        constexpr std::array<std::uint32_t, 3> expected{7, 5, 2};
        CHECK(count == expected);
        CHECK(owner.loader.DrawCounts() == expected);
    }

    /** @brief Computes the actual projected pixel-center ray/plane depth without using packed instance or shader outputs. */
    float PlaneDepth(const Math::Mat4 &projection, int x, int y, Math::Vec3 center) {
        const auto inverse = Value(Math::TryInverse(projection));
        const float nx = (float(x) + .5F) / 32 - 1, ny = (float(y) + .5F) / 16 - 1;
        const auto near = Math::TransformPoint(inverse, {nx, ny, 0});
        const auto far = Math::TransformPoint(inverse, {nx, ny, 1});
        const auto ray = far - near;
        REQUIRE(std::abs(ray.z) > .0001F);
        const auto point = near + ray * ((center.z - near.z) / ray.z);
        return Math::TransformPoint(projection, point).z;
    }

    /** @brief Verifies real copied FP16/depth values under Vulkan positive-height viewport and zero-to-one clip conventions. */
    void Pixel(const Scene &scene, const Pixels &pixels, Math::Vec3 center, std::array<float, 4> expected, bool authored = false) {
        const auto ndc = Math::TransformPoint(scene.view.viewProjection, center);
        const auto x = int(std::floor((ndc.x + 1) * 32)), y = int(std::floor((ndc.y + 1) * 16));
        REQUIRE(x > 0);
        REQUIRE(x < 63);
        REQUIRE(y > 0);
        REQUIRE(y < 31);
        const auto offset = std::size_t(y * 64 + x);
        INFO("Actual Vulkan pixel " << x << ',' << y);
        for (std::size_t i = 0; i < expected.size(); ++i)
            CHECK(float(std::bit_cast<_Float16>(pixels.color[offset * 4 + i])) == Catch::Approx(expected[i]).margin(.02F).epsilon(0));
        CHECK(pixels.depth[offset] == Catch::Approx(PlaneDepth(scene.view.viewProjection, x, y, center)).margin(.0001F).epsilon(0));
        if (authored)
            CHECK(std::abs(pixels.depth[offset] - PlaneDepth(scene.view.viewProjection, x, y, Right)) > .001F);
    }

    /** @brief Checks the independent seven-particle GL oracle, including actual masking and rear translucent depth rejection. */
    void CheckPixels(const Scene &scene, const Pixels &pixels) {
        constexpr float alpha = 128.0F / 255;
        Pixel(scene, pixels, Left, {1 - alpha, 64.0F / 255, alpha, 1});
        Pixel(scene, pixels, {-.6F, -1.1F, -3}, {1, 1, 0, 1});
        Pixel(scene, pixels, Right + Math::Vec3{0, 0, AuthoredZ}, {0, 128.0F / 255, 0, 1}, true);
    }

    /** @brief Requires retained CPU graph borrows and actual native completion before accepting a new instance revision. */
    void Reuse(Owner &owner, Scene &scene, Recipe &recipe, std::vector<RenderGraphPassWorkload> &workloads) {
        const std::array<VfxPackedDrawInstance, 1> replacement{};
        const auto revision = recipe.bindings[0].slot->Revision() + 1;
        const auto held = owner.frontend->UpdateVfxFrame(recipe.bindings[0].slot, revision, replacement);
        REQUIRE(held.HasError());
        CHECK(held.ErrorValue().code.Value() == VfxFrameErrors::Pending.code.Value());
        workloads.clear();
        recipe.schedule = {};
        auto retirement = Value(owner.frontend->BeginFrame({2, Extent}));
        retirement.Cancel();
        auto recycled = Schedule(*owner.frontend, scene, 2, false);
        CHECK(recycled.schedule.instances == 7);
        auto slot = std::move(recipe.bindings[0].slot);
        recipe.bindings.clear();
        Accepted(owner.frontend->UpdateVfxFrame(slot, revision, replacement));
        CHECK(slot->Revision() == revision);
    }

    TEST_CASE("Real Vulkan indexed VFX billboards and authored Z mesh preserve pixels depth blend and publication lifetime",
              "[integration][renderer][vulkan][vfx][instanced][software-coverage]") {
        const auto package = Package();
        Owner owner;
        Scene scene{*owner.frontend, package};
        auto recipe = Schedule(*owner.frontend, scene, 1, true);
        REQUIRE(recipe.bindings.size() == 5);
        Accepted(owner.frontend->PrepareVfxDraws(recipe.builder, recipe.schedule, scene.view, recipe.bindings));
        const auto graph = Compile(recipe, owner);
        auto workloads = Workloads(recipe, graph);
        Execute(owner, graph, workloads, 1, false);
        DrawCounts(owner, recipe);
        NativeReadback readback{owner.loader};
        readback.Prepare();
        CheckPixels(scene, readback.Read());
        Reuse(owner, scene, recipe, workloads);
    }

    TEST_CASE("Real Vulkan instanced draw abandonment preserves initialized resources for a later actual committed retry",
              "[integration][renderer][vulkan][vfx][instanced][abort][software-coverage]") {
        const auto package = Package();
        Owner owner;
        Scene scene{*owner.frontend, package};
        auto recipe = Schedule(*owner.frontend, scene, 1, true);
        Accepted(owner.frontend->PrepareVfxDraws(recipe.builder, recipe.schedule, scene.view, recipe.bindings));
        const auto graph = Compile(recipe, owner);
        const auto workloads = Workloads(recipe, graph);
        const auto abandoned = Execute(owner, graph, workloads, 1, true);
        const auto committed = Execute(owner, graph, workloads, 2, false);
        CHECK(committed.value > abandoned.value);
        DrawCounts(owner, recipe);
        NativeReadback readback{owner.loader};
        readback.Prepare();
        CheckPixels(scene, readback.Read());
    }
}  // namespace VulkanVfxInstancedSmoke
