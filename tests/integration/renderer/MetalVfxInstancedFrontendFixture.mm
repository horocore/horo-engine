#include "MetalVfxInstancedFrontendFixture.h"

#include "../../unit/runtime/renderer/RenderGraphTestUtils.h"
#include "Horo/Math/SceneMath.h"
#include "Horo/Runtime/Render/MaterialBinding.h"
#include "Horo/Runtime/Render/RenderFrontend.h"
#include "Horo/Runtime/Render/RenderGraphExecution.h"
#include "Horo/Runtime/Render/RenderGraphSynchronization.h"
#include "Horo/Runtime/Render/RenderPresentationOutput.h"
#include "Horo/Runtime/Render/ShaderArtifact.h"
#include "Horo/Runtime/Render/ShaderCompilerPipeline.h"
#include "Horo/Runtime/Render/VfxFrameUpload.h"
#include "Horo/Runtime/Render/VfxShaderParameters.h"
#include "Horo/Vfx/VfxWorld.h"
#include "editor/renderer/metal/SdlMetalPresentationPort.h"
#include "runtime/renderer/frontend/RenderFrontendResourceAccess.h"
#include "runtime/renderer/modules/metal/MetalResourceInstances.h"

#import <Metal/Metal.h>
#include <SDL3/SDL.h>
#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <optional>
#include <thread>

namespace Horo::Render::Test {
    namespace {
        using namespace std::chrono_literals;
        constexpr FramebufferExtent Extent{64, 32};
        constexpr Math::Vec3 Eye{.7F, .25F, .5F}, Left{-.6F, 0, -3}, Right{.6F, 0, -3};
        constexpr float AuthoredZ = .55F;

        /** @brief Preserves the exact native/frontend error at each genuine fixture operation. */
        template <class T> T Value(Result<T> result) {
            INFO((result.HasError() ? result.ErrorValue().message : "Actual operation accepted"));
            REQUIRE(result.HasValue());
            return std::move(result).Value();
        }

        /** @brief Preserves typed errors at actual void owner operations. */
        void Accepted(const Result<void> &result) {
            INFO((result.HasError() ? result.ErrorValue().message : "Actual operation accepted"));
            REQUIRE(result.HasValue());
        }

        /** @brief Waits only at this bounded qualification boundary, never within production frame admission. */
        bool Terminal(id<MTLCommandBuffer> commands) {
            if (!commands)
                return true;
            const auto deadline = std::chrono::steady_clock::now() + 5s;
            while (commands.status != MTLCommandBufferStatusCompleted && commands.status != MTLCommandBufferStatusError) {
                if (std::chrono::steady_clock::now() >= deadline)
                    return false;
                std::this_thread::sleep_for(1ms);
            }
            return commands.status == MTLCommandBufferStatusCompleted;
        }

        /** @brief Uses the existing frontend's actual terminal shutdown authority before releasing the surface. */
        bool Drain(RenderFrontend &frontend) {
            if (frontend.BeginShutdown().HasError())
                return false;
            const auto deadline = std::chrono::steady_clock::now() + 5s;
            while (std::chrono::steady_clock::now() < deadline) {
                const auto state = frontend.PollDrain();
                if (state.HasError())
                    return false;
                if (state.Value() == RenderBackendDrainProgress::Drained)
                    return true;
                std::this_thread::sleep_for(1ms);
            }
            return false;
        }

        /** @brief Reads only explicit genuine offline payload/reflection pairs, without fabricating opaque native stages. */
        std::vector<std::uint8_t> Read(const std::filesystem::path &path) {
            REQUIRE_FALSE(std::filesystem::is_symlink(path));
            const auto size = std::filesystem::file_size(path);
            REQUIRE(size > 0);
            REQUIRE(size <= 16U * 1024U * 1024U);
            std::vector<std::uint8_t> bytes(size);
            std::ifstream input(path, std::ios::binary);
            REQUIRE(input.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(bytes.size())));
            return bytes;
        }

        /** @brief Decodes actual same-artifact reflection against the canonical CPU instancing manifest. */
        CompiledShaderArtifact Load(const std::filesystem::path &root, bool masked) {
            const auto prefix = root / (masked ? "shaders/runtime/vfx-instanced-masked-metal" : "shaders/runtime/vfx-instanced-metal");
            auto payload = Read(prefix.string() + ".horoshdr");
            const auto metadata = Read(prefix.string() + ".reflection.json");
            const ShaderTargetRequirement target{ShaderTargetBackend::Metal, ShaderPayloadFormat::MetalLibrary24, 1, 1, 4, 0};
            auto reflection = Value(DecodeReflectedShaderArtifact(MakeVfxShaderManifest(target), target, payload, metadata));
            CompiledShaderArtifact artifact{.backend = target.backend,
                                            .payloadFormat = target.payloadFormat,
                                            .artifactKey = reflection.compiledArtifactKey,
                                            .payload = std::move(payload),
                                            .reflection = std::make_shared<const ReflectedShaderArtifact>(std::move(reflection))};
            Accepted(ValidateReflectedShaderArtifact(artifact, *artifact.reflection));
            return artifact;
        }

        /** @brief Owns the actual production frontend, device, presentation port and completion evidence. */
        struct NativeOwner final {
            SDL_Window *window{};
            std::unique_ptr<Editor::SdlMetalPresentationPort> presentation;
            MetalEditorGraphicsBridge bridge;
            RenderBackendRegistry registry;
            std::unique_ptr<RenderFrontend> frontend;
            std::vector<RenderQueueAssignment> queues;
            __strong id<MTLCommandBuffer> commands{nil}, copied{nil};
            __strong id<MTLSharedEvent> gate{nil};
            std::uint64_t frame{};

            /** @brief Releases a fixture-only gate and drains real native work before surface/resource owner destruction. */
            ~NativeOwner() {
                gate.signaledValue = 1;
                if (!Terminal(commands) || !Terminal(copied))
                    std::terminate();
                if (frontend && !Drain(*frontend))
                    std::terminate();
                frontend.reset();
                presentation.reset();
                if (window)
                    SDL_DestroyWindow(window);
                SDL_Quit();
            }

            /** @brief Creates a separate unfocused fixture window and registers the real selected Metal implementation. */
            void Initialize() {
                REQUIRE(SDL_Init(SDL_INIT_VIDEO));
                REQUIRE(SDL_SetHint(SDL_HINT_VIDEO_METAL_AUTO_RESIZE_DRAWABLE, "0"));
                REQUIRE(SDL_SetHint(SDL_HINT_WINDOW_ACTIVATE_WHEN_SHOWN, "0"));
                window = SDL_CreateWindow("Horo indexed VFX qualification", 64, 32, SDL_WINDOW_METAL | SDL_WINDOW_HIDDEN);
                REQUIRE(window);
                REQUIRE(SDL_SetWindowFocusable(window, false));
                presentation = std::make_unique<Editor::SdlMetalPresentationPort>(*window);
                Accepted(RegisterMetalRenderBackend(registry, *presentation, bridge));
                Accepted(registry.Seal());
                frontend = Value(RenderFrontend::Create(registry, RenderBackendId{"metal"},
                                                        {.requirePresentation = true,
                                                         .enableValidation = true,
                                                         .maxFramesInFlight = 2,
                                                         .presentMode = PresentMode::Immediate}));
                REQUIRE(SDL_ShowWindow(window));
                const auto topology = Value(Detail::RenderFrontendResourceAccess::QueueAssignments(*frontend));
                queues.assign(topology.begin(), topology.end());
                REQUIRE(frontend->Capabilities().support.features.Supports(RenderCapability::VfxForwardInstancing));
            }

            /** @brief Retries only genuine pending drawable admission under one explicit fixture deadline. */
            RenderFrameScope Begin() {
                const auto deadline = std::chrono::steady_clock::now() + 5s;
                const auto number = ++frame;
                for (;;) {
                    SDL_PumpEvents();
                    const auto available = Value(frontend->PollPresentationAvailability(Extent));
                    if (available) {
                        auto result = frontend->BeginFrame({number, Extent});
                        if (result.HasValue())
                            return std::move(result).Value();
                        REQUIRE(result.ErrorValue().code.Value() == RenderPresentationErrors::AcquisitionPending.code.Value());
                    }
                    REQUIRE(std::chrono::steady_clock::now() < deadline);
                    std::this_thread::sleep_for(1ms);
                }
            }

            /** @brief Proves exact public resource readiness after cold processing. */
            template <class Handle> void Ready(Handle handle) {
                Value(frontend->ProcessResourceRequests());
                REQUIRE(Value(frontend->ResourceState(handle)) == RenderResourceState::Ready);
            }

            /** @brief Resolves the actual graphics queue identity for graph synchronization. */
            RenderQueueId Queue() const {
                const auto found = std::ranges::find(queues, RenderQueueRole::Graphics, &RenderQueueAssignment::role);
                REQUIRE(found != queues.end());
                return found->queue;
            }
        };

        struct Image final {
            RenderTextureHandle texture;
            RenderTextureViewHandle view;
        };

        /** @brief Allocates actual sampled/attachment storage and its typed view through the public frontend. */
        Image Texture(NativeOwner &owner, RenderTextureDescriptor descriptor, RenderTextureAspect aspect,
                      std::span<const std::byte> data = {}) {
            const auto texture = Value(owner.frontend->CreateTexture(descriptor, data)).handle;
            owner.Ready(texture);
            const auto view =
                Value(owner.frontend->CreateTextureView({.texture = texture, .format = descriptor.format, .aspect = aspect})).handle;
            owner.Ready(view);
            return {texture, view};
        }

        struct MaterialFixture final {
            MaterialRuntimeId id;
            RenderShaderModuleHandle vertex, fragment;
            RenderPipelineHandle pipeline;
            MaterialBindingLayout layout;
            RenderBufferHandle instances;
            Image texture;
            RenderSamplerHandle sampler;
            std::unique_ptr<MaterialBindingTable> table;
            std::shared_ptr<PreparedVfxFrameSlot> slot;
        };

        /** @brief Computes actual selected-device compatibility for an exact genuine compiled VFX pipeline. */
        PipelineCacheCompatibility Compatibility(NativeOwner &owner, const ShaderTargetRequirement &target,
                                                 const ReflectedShaderArtifact &artifact, const RenderGraphicsPipelineState &state) {
            const auto device = Value(owner.frontend->QueryPipelineDeviceIdentity());
            return {1,
                    device.backend,
                    device.backendModuleVersion,
                    device.adapter,
                    device.deviceCompatibilityDigest,
                    device.driverIdentity,
                    device.driverVersion,
                    target.backend,
                    target.payloadFormat,
                    artifact.cookedArtifactKey,
                    artifact.reflection.interfaceCompatibility,
                    Value(ComputeRenderGraphicsPipelineStateDigest(state))};
        }

        /** @brief Creates one real indexed VFX pipeline for its declared blend/depth class. */
        void Pipeline(NativeOwner &owner, MaterialFixture &fixture, const CompiledShaderArtifact &artifact, unsigned index) {
            const ShaderTargetRequirement target{ShaderTargetBackend::Metal, ShaderPayloadFormat::MetalLibrary24, 1, 1, 4, 0};
            REQUIRE(artifact.reflection);
            const auto &metadata = *artifact.reflection;
            fixture.vertex = Value(owner.frontend->CreateShaderModule({target, ShaderStage::Vertex, "VSMain", metadata.cookedArtifactKey,
                                                                       metadata.reflection.interfaceCompatibility},
                                                                      artifact.payload))
                                 .handle;
            fixture.fragment =
                Value(owner.frontend->CreateShaderModule({target, ShaderStage::Fragment, "PSMain", metadata.cookedArtifactKey,
                                                          metadata.reflection.interfaceCompatibility},
                                                         artifact.payload))
                    .handle;
            const RenderGraphicsPipelineState state{.cull = RenderCullMode::None,
                                                    .blend = index == 2   ? RenderBlendMode::Alpha
                                                             : index == 4 ? RenderBlendMode::Additive
                                                                          : RenderBlendMode::Disabled,
                                                    .depthTest = true,
                                                    .depthWrite = index != 2 && index != 4,
                                                    .colorFormat = RenderTextureFormat::Rgba16Float,
                                                    .depthFormat = RenderTextureFormat::Depth32Float};
            fixture.pipeline =
                Value(owner.frontend->CreateGraphicsPipeline({fixture.vertex, fixture.fragment, MakeVfxShaderManifest(target), target,
                                                              metadata.reflection, state, Compatibility(owner, target, metadata, state)}))
                    .handle;
            owner.Ready(fixture.pipeline);
            fixture.layout = Value(PrepareMaterialBindingLayout(metadata.reflection));
        }

        /** @brief Uses a nonuniform texture to distinguish authored mesh UVs from normal fields and flipped coordinates. */
        Image MaterialTexture(NativeOwner &owner, unsigned index, bool cutout) {
            const std::array<std::byte, 4> texel{std::byte{255}, std::byte{255}, std::byte{255},
                                                 index == 3 ? std::byte{0} : std::byte{255}};
            const std::array<std::byte, 16> atlas{std::byte{32},  std::byte{32},  std::byte{32},  std::byte{255},
                                                  std::byte{128}, std::byte{128}, std::byte{128}, std::byte{255},
                                                  std::byte{64},  std::byte{64},  std::byte{64},  std::byte{255},
                                                  std::byte{255}, std::byte{255}, std::byte{255}, std::byte{255}};
            if (index == 3 && cutout) {
                const std::array<std::byte, 8> mask{std::byte{255}, std::byte{255}, std::byte{255}, std::byte{0},
                                                    std::byte{255}, std::byte{255}, std::byte{255}, std::byte{255}};
                return Texture(owner, {.extent = {2, 1}, .format = RenderTextureFormat::Rgba8Unorm, .usage = RenderTextureUsage::Sampled},
                               RenderTextureAspect::Color, mask);
            }
            const auto extent = index == 1 ? FramebufferExtent{2, 2} : FramebufferExtent{1, 1};
            return Texture(owner, {.extent = extent, .format = RenderTextureFormat::Rgba8Unorm, .usage = RenderTextureUsage::Sampled},
                           RenderTextureAspect::Color, index == 1 ? std::span<const std::byte>{atlas} : std::span<const std::byte>{texel});
        }

        /** @brief Packs immutable tint/threshold through canonical material layout, leaving native padding to the existing backend query.
         */
        MaterialParameterBinding Constants(const MaterialBindingLayout &layout) {
            const std::array<float, 4> tint{1, 1, 1, 1};
            const std::array<float, 1> cutoff{.5F};
            const std::array values{MaterialParameterValue{VfxShaderParameterIds::Tint, std::span<const float>{tint}},
                                    MaterialParameterValue{VfxShaderParameterIds::AlphaCutoff, std::span<const float>{cutoff}}};
            return Value(PackMaterialParameterBinding(layout, VfxShaderParameterIds::Material, values));
        }

        /** @brief Publishes the actual canonical material and obtains its single dedicated host-visible upload slot. */
        void PrepareSlot(NativeOwner &owner, MaterialFixture &fixture, std::size_t bytes) {
            fixture.table = Value(MaterialBindingTable::Create(fixture.pipeline.owner, *owner.frontend));
            const auto published = Value(fixture.table->Publish(
                {.material = fixture.id,
                 .revision = 1,
                 .pipeline = fixture.pipeline,
                 .layout = fixture.layout,
                 .resources = {{VfxShaderParameterIds::Instances, 0, MaterialBufferBinding{fixture.instances, 0, bytes}},
                               {VfxShaderParameterIds::Material, 0, Constants(fixture.layout)},
                               {VfxShaderParameterIds::BaseColor, 0, fixture.texture.view},
                               {VfxShaderParameterIds::BaseColorSampler, 0, fixture.sampler}}}));
            fixture.slot = Value(owner.frontend->PrepareVfxFrameSlot(Value(fixture.table->Acquire(published.binding))));
        }

        /** @brief Cold-prepares only real shader, texture, sampler, buffer and resident material generations. */
        MaterialFixture Material(NativeOwner &owner, unsigned index, const std::array<CompiledShaderArtifact, 2> &artifacts, bool cutout) {
            MaterialFixture fixture;
            fixture.id = {100 + index};
            Pipeline(owner, fixture, artifacts[index == 3 ? 1 : 0], index);
            const auto bytes = Value(VfxShaderParameterBytes(fixture.layout));
            REQUIRE(bytes == 10240);
            fixture.instances =
                Value(owner.frontend->CreateBuffer({bytes, RenderBufferUsage::Uniform, RenderBufferAccess::HostVisible}, {})).handle;
            fixture.texture = MaterialTexture(owner, index, cutout);
            fixture.sampler = Value(owner.frontend->CreateSampler({})).handle;
            owner.Ready(fixture.instances);
            owner.Ready(fixture.sampler);
            PrepareSlot(owner, fixture, bytes);
            return fixture;
        }

        struct MeshFixture final {
            RenderMeshHandle mesh;
            RenderBufferHandle vertices, indices;
        };

        /** @brief Issues canonical indexed quad geometry or a mesh with deliberately nonzero authored Z and sentinel UVs. */
        MeshFixture Mesh(NativeOwner &owner, float z) {
            std::array<MeshVertex, 4> vertices{{{{-.5F, -.5F, z}, {0, 0, 1}, {0, 0}},
                                                {{.5F, -.5F, z}, {0, 0, 1}, {1, 0}},
                                                {{.5F, .5F, z}, {0, 0, 1}, {1, 1}},
                                                {{-.5F, .5F, z}, {0, 0, 1}, {0, 1}}}};
            if (z != 0)
                for (auto &vertex : vertices)
                    vertex.uv = {.75F, .25F};
            constexpr std::array<std::uint32_t, 6> indices{0, 1, 2, 2, 3, 0};
            const auto vertex =
                Value(owner.frontend->CreateBuffer({sizeof(vertices), RenderBufferUsage::Vertex}, std::as_bytes(std::span{vertices})))
                    .handle;
            const auto index =
                Value(owner.frontend->CreateBuffer({sizeof(indices), RenderBufferUsage::Index}, std::as_bytes(std::span{indices}))).handle;
            owner.Ready(vertex);
            owner.Ready(index);
            const auto mesh = Value(owner.frontend->CreateMesh({.vertexBuffer = vertex,
                                                                .indexBuffer = index,
                                                                .vertexStride = sizeof(MeshVertex),
                                                                .vertexCount = 4,
                                                                .indexCount = 6,
                                                                .localBounds = {{-.5F, -.5F, z}, {.5F, .5F, z}}}))
                                  .handle;
            owner.Ready(mesh);
            return {mesh, vertex, index};
        }

        /** @brief Builds the independent off-axis camera with Metal zero-to-one clip depth and an actually issued logical view. */
        VfxDrawView View(NativeOwner &owner) {
            const auto view = Value(Math::TryLookAt(Eye, {0, 0, -3}, {0, 1, 0}));
            const auto projection = Value(Math::TryPerspective(1.1F, 2, .2F, 80, Math::ClipDepthRange::ZeroToOne));
            const auto identity = Value(owner.frontend->CreateRenderViewIdentity());
            return {.viewProjection = Math::Multiply(projection, view),
                    .right = {view.At(0, 0), view.At(0, 1), view.At(0, 2)},
                    .up = {view.At(1, 0), view.At(1, 1), view.At(1, 2)},
                    .scene = 17,
                    .frame = 1,
                    .originRevision = 1,
                    .view = identity.value,
                    .viewGeneration = 1};
        }

        /** @brief Moves a layer along the actual eye ray so distinct depth/blend layers overlap at the reference pixel. */
        Math::Vec3 AlongRay(Math::Vec3 point, float scale) {
            return Eye + (point - Eye) * scale;
        }

        /** @brief Derives only authored particle sizes; this helper contains no shader packing or expected-pixel computation. */
        float RayScale(unsigned index, unsigned particle) {
            if (index == 2)
                return particle == 1 ? 1.5F : .7F;
            if (index == 3)
                return .55F;
            if (index == 4)
                return .5F;
            return 1;
        }

        /** @brief Owns actual canonical CPU simulation/extraction and actual frontend material/geometry generations. */
        struct Scene final {
            NativeOwner &owner;
            MeshFixture billboard, mesh;
            Image color, depth;
            RenderTargetHandle target;
            std::vector<MaterialFixture> materials;
            std::unique_ptr<Vfx::VfxWorld> world;
            VfxDrawView view;

            /** @brief Creates real fixed scene resources outside frame encoding and measurement windows. */
            Scene(NativeOwner &native, const std::filesystem::path &root, bool cutout = false) : owner(native), view(View(native)) {
                Accepted(owner.frontend->PrepareVfxScheduling({64, 1, 1'000'000, 16}));
                billboard = Mesh(owner, 0);
                mesh = Mesh(owner, AuthoredZ);
                color = Texture(owner,
                                {.extent = Extent,
                                 .format = RenderTextureFormat::Rgba16Float,
                                 .usage = RenderTextureUsage::RenderAttachment | RenderTextureUsage::CopySource},
                                RenderTextureAspect::Color);
                depth = Texture(owner,
                                {.extent = Extent,
                                 .format = RenderTextureFormat::Depth32Float,
                                 .usage = RenderTextureUsage::RenderAttachment | RenderTextureUsage::CopySource},
                                RenderTextureAspect::Depth);
                target = Value(owner.frontend->CreateRenderTarget(
                                   {.colorAttachment = color.view, .depthAttachment = depth.view, .extent = Extent}))
                             .handle;
                owner.Ready(target);
                const std::array artifacts{Load(root, false), Load(root, true)};
                for (unsigned i = 0; i < 5; ++i)
                    materials.push_back(Material(owner, i, artifacts, cutout));
                const auto scope = Value(Vfx::VfxIdentityScope::Create(17));
                const auto emitters = Emitters(scope);
                world = Value(Vfx::VfxWorld::Prepare(scope, emitters, {16, 64, 128, 2, 1'000'000}));
                Populate(emitters);
            }

            /** @brief Closes only actual material/world owners while the selected frontend still exists. */
            ~Scene() {
                if (world)
                    static_cast<void>(world->Shutdown());
                for (auto &material : materials) {
                    material.slot.reset();
                    if (material.table && material.table->Shutdown().HasError())
                        std::terminate();
                }
            }

            /** @brief Creates stable logical emitter identities and exact material/topology bindings, never native identities. */
            std::array<Vfx::VfxEmitterPreparation, 5> Emitters(Vfx::VfxIdentityScope scope) const {
                constexpr std::array passes{VfxParticlePass::Opaque, VfxParticlePass::Opaque, VfxParticlePass::Translucent,
                                            VfxParticlePass::Masked, VfxParticlePass::Additive};
                constexpr std::array blends{MaterialAlphaMode::Opaque, MaterialAlphaMode::Opaque, MaterialAlphaMode::Translucent,
                                            MaterialAlphaMode::Masked, MaterialAlphaMode::Additive};
                std::array<Vfx::VfxEmitterPreparation, 5> emitters;
                for (unsigned i = 0; i < 5; ++i)
                    emitters[i] = {.effect = {scope, i, 1},
                                   .emitter = {scope, i, 1},
                                   .particles = {.buffer = {scope, i, 1}, .capacity = 8},
                                   .material = materials[i].id,
                                   .materialBlend = blends[i],
                                   .mesh = i == 1             ? mesh.mesh
                                           : i == 0 || i == 3 ? billboard.mesh
                                                              : RenderMeshHandle{},
                                   .blend = passes[i],
                                   .topology = i == 2 || i == 4 ? VfxPrimitiveTopology::CameraFacingBillboard
                                                                : VfxPrimitiveTopology::InstancedMesh};
                return emitters;
            }

            /** @brief Seeds one actual particle buffer with distinguishable positions, colors and authored mesh-local dimensions. */
            void PopulateEmitter(const Vfx::VfxEmitterPreparation &emitter, unsigned index, Math::Vec3 origin, std::uint32_t colorValue) {
                Accepted(world->Play(emitter.effect));
                const auto particles = Value(world->Particles(emitter.effect, emitter.emitter));
                const auto count = index == 0 || index == 2 ? 2U : 1U;
                for (unsigned i = 0; i < count; ++i)
                    Value(particles->Spawn(Value(Vfx::ParticleSimulationId::Create(index * 8 + i + 1))));
                auto attributes = Value(particles->View());
                for (unsigned i = 0; i < count; ++i) {
                    auto position = origin;
                    if (i == 1)
                        position = index == 0 ? Math::Vec3{-.6F, -1.1F, -3} : AlongRay(Right + Math::Vec3{0, 0, AuthoredZ}, 1.5F);
                    attributes.positionX[i] = position.x;
                    attributes.positionY[i] = position.y;
                    attributes.positionZ[i] = position.z;
                    attributes.sizeX[i] = .8F * RayScale(index, i);
                    attributes.sizeY[i] = .8F * RayScale(index, i);
                    attributes.rotation[i] = index == 1 ? .35F : .17F;
                    attributes.packedColor[i] = i == 1 && index == 0 ? 0xFFFF00FF : colorValue;
                }
            }

            /** @brief Retains the existing seven-particle independent GL semantic scene without copying a native shader oracle. */
            void Populate(const std::array<Vfx::VfxEmitterPreparation, 5> &emitters) {
                const std::array positions{Left, Right, AlongRay(Left, .7F), AlongRay(Left, .55F), AlongRay(Left, .5F)};
                constexpr std::array<std::uint32_t, 5> colors{0xFF0000FF, 0x00FF00FF, 0x0000FF80, 0xFFFFFFFF, 0x00400000};
                for (unsigned i = 0; i < 5; ++i)
                    PopulateEmitter(emitters[i], i, positions[i], colors[i]);
            }
        };

        struct Recipe final {
            RenderGraphBuilder builder;
            VfxGraphSchedule schedule;
            std::vector<VfxGraphDrawBinding> bindings;
            RenderGraphPassRef clearColor, clearDepth;
            RenderGraphResourceId color, depth;
            /** @brief Constructs and fills one stable graph owner in place; its issued references never cross a builder move. */
            explicit Recipe(Scene &scene);
            Recipe(const Recipe &) = delete;
            Recipe(Recipe &&) = delete;
        };

        /** @brief Binds each actual contiguous scheduled run to exact frontend-issued geometry, parameters, samples and unique upload
         * slots. */
        void BindDraws(Scene &scene, Recipe &recipe) {
            const std::array billboard{Value(recipe.builder.ImportBuffer(scene.billboard.vertices, RenderGraphResourceClass::Persistent)),
                                       Value(recipe.builder.ImportBuffer(scene.billboard.indices, RenderGraphResourceClass::Persistent))};
            const std::array mesh{Value(recipe.builder.ImportBuffer(scene.mesh.vertices, RenderGraphResourceClass::Persistent)),
                                  Value(recipe.builder.ImportBuffer(scene.mesh.indices, RenderGraphResourceClass::Persistent))};
            for (const auto &record : recipe.schedule.Workloads()) {
                const auto &vfx = std::get<RenderGraphVfx>(record.workload);
                const auto batches = vfx.snapshot.Batches();
                for (std::size_t first = 0; first < vfx.order->instances.size();) {
                    const auto &batch = batches[vfx.order->instances[first].batch];
                    auto material = std::ranges::find(scene.materials, batch.material, &MaterialFixture::id);
                    REQUIRE(material != scene.materials.end());
                    const bool authored = batch.mesh == scene.mesh.mesh;
                    const auto &geometry = authored ? scene.mesh : scene.billboard;
                    const auto &buffers = authored ? mesh : billboard;
                    recipe.bindings.push_back(
                        {.stage = vfx.stage,
                         .firstOrder = static_cast<std::uint32_t>(first),
                         .mesh = geometry.mesh,
                         .target = scene.target,
                         .vertices = buffers[0],
                         .indices = buffers[1],
                         .parameters = Value(recipe.builder.ImportBuffer(material->instances, RenderGraphResourceClass::Persistent)),
                         .sampledTexture =
                             Value(recipe.builder.ImportTexture(material->texture.texture, RenderGraphResourceClass::Persistent)),
                         .slot = std::move(material->slot)});
                    const auto batchIndex = vfx.order->instances[first].batch;
                    while (first < vfx.order->instances.size() && vfx.order->instances[first].batch == batchIndex)
                        ++first;
                }
            }
        }

        /** @brief Uses the canonical extractor/scheduler with the real scene, view and graph resource identities. */
        void Schedule(Scene &scene, Recipe &recipe) {
            auto &builder = recipe.builder;
            const auto color = Value(builder.ImportTexture(scene.color.texture, RenderGraphResourceClass::Persistent));
            const auto depth = Value(builder.ImportTexture(scene.depth.texture, RenderGraphResourceClass::Persistent));
            const auto clearColor = RequirePass(builder, RenderPassKind::Graphics, RenderQueueRole::Graphics);
            const auto clearDepth = RequirePass(builder, RenderPassKind::Graphics, RenderQueueRole::Graphics);
            const auto opaque = RequirePass(builder, RenderPassKind::Graphics, RenderQueueRole::Graphics);
            const auto output = RequirePass(builder, RenderPassKind::Graphics, RenderQueueRole::Graphics);
            RequireUsage(builder, {clearColor, color, RenderGraphAccess::Write, RenderGraphUsageKind::ColorAttachment});
            RequireUsage(builder, {clearDepth, depth, RenderGraphAccess::Write, RenderGraphUsageKind::DepthStencilAttachment});
            RequireDependency(builder, {clearColor, clearDepth});
            RequireDependency(builder, {clearDepth, opaque});
            RequireDependency(builder, {opaque, output});
            const auto forward = Math::Normalize(Math::Cross(scene.view.up, scene.view.right));
            const VfxGraphRequest request{.scene = scene.view.scene,
                                          .frame = scene.view.frame,
                                          .originRevision = scene.view.originRevision,
                                          .view = scene.view.view,
                                          .viewGeneration = scene.view.viewGeneration,
                                          .camera = Eye,
                                          .forward = forward,
                                          .maximumInstances = 64,
                                          .remainingDrawCalls = 16,
                                          .opaqueBegin = clearDepth,
                                          .opaqueComplete = opaque,
                                          .outputTransform = output,
                                          .sceneColor = color,
                                          .sceneDepth = depth};
            const auto extracted = scene.world->Extract(scene.view.frame, {}, 1);
            auto scheduled = scene.owner.frontend->ScheduleVfx(builder, extracted.snapshot, request);
            INFO((scheduled.HasError() ? scheduled.ErrorValue().message : "Actual VFX snapshot scheduled"));
            REQUIRE(scheduled.HasValue());
            recipe.schedule = std::move(scheduled.Value());
            recipe.clearColor = clearColor;
            recipe.clearDepth = clearDepth;
            recipe.color = color;
            recipe.depth = depth;
            BindDraws(scene, recipe);
            Accepted(scene.owner.frontend->PrepareVfxDraws(recipe.builder, recipe.schedule, scene.view, recipe.bindings));
        }

        /** @copydoc Recipe::Recipe */
        Recipe::Recipe(Scene &scene)
            : builder(Value(RenderGraphBuilder::Create({.maxPasses = 16, .maxResources = 32, .maxUsages = 128, .maxDependencies = 64}))) {
            Schedule(scene, *this);
        }

        /** @brief Derives synchronization from actual imported resources and the selected graphics queue, not a handwritten execution plan.
         */
        CompiledRenderGraphExecution Compile(NativeOwner &owner, Recipe &recipe) {
            const auto graph = RequireGraph(recipe.builder);
            const auto schedule = RequireSchedule(graph);
            std::vector<RenderGraphImportedState> initial;
            for (const auto &resource : graph.Resources()) {
                RenderGraphLogicalState state{RenderGraphSynchronizationAccess::Read,
                                              RenderGraphSynchronizationOperation::GraphicsInputBuffer, RenderGraphPipelineScope::Graphics,
                                              RenderGraphTextureLayout::NotApplicable, owner.Queue()};
                if (resource.kind == RenderGraphResourceKind::Texture) {
                    state.operation = RenderGraphSynchronizationOperation::Sampled;
                    state.layout = RenderGraphTextureLayout::ShaderReadOnly;
                }
                if (resource.id == recipe.color) {
                    state.access = RenderGraphSynchronizationAccess::ReadWrite;
                    state.operation = RenderGraphSynchronizationOperation::ColorAttachment;
                    state.layout = RenderGraphTextureLayout::ColorAttachment;
                }
                if (resource.id == recipe.depth) {
                    state.access = RenderGraphSynchronizationAccess::ReadWrite;
                    state.operation = RenderGraphSynchronizationOperation::DepthStencilAttachment;
                    state.layout = RenderGraphTextureLayout::DepthStencilAttachment;
                }
                initial.push_back({resource.id, state});
            }
            const auto synchronization = Value(SynthesizeRenderGraphSynchronization(graph, schedule, owner.queues, initial));
            return Value(CompileRenderGraphExecution(graph, schedule, synchronization, owner.queues));
        }

        /** @brief Combines actual retained passes with canonical attachment clears and the exact scheduled VFX operations. */
        std::vector<RenderGraphPassWorkload> Workloads(Recipe &recipe, const CompiledRenderGraphExecution &graph) {
            std::vector<RenderGraphPassWorkload> workloads;
            for (const auto &pass : graph.Passes()) {
                const auto found = std::ranges::find(recipe.schedule.Workloads(), pass.pass, &RenderGraphPassWorkload::pass);
                if (found != recipe.schedule.Workloads().end())
                    workloads.push_back(*found);
                else if (pass.pass == recipe.clearColor)
                    workloads.push_back({pass.pass, RenderGraphColorAttachment{recipe.color, {}}});
                else if (pass.pass == recipe.clearDepth)
                    workloads.push_back({pass.pass, RenderGraphDepthAttachment{recipe.depth, 1}});
                else
                    workloads.push_back({pass.pass, {}});
            }
            return workloads;
        }

        /** @brief Resolves only a real frontend-issued texture instance for fixture-only terminal diagnostic readback. */
        id<MTLTexture> NativeTexture(NativeOwner &owner, const Image &image) {
            // MetalResourceRuntime::CreateTextureView returns this actual retained view object, not a raw texture ID.
            const auto native = Value(Detail::RenderFrontendResourceAccess::BackendInstance(*owner.frontend, image.view));
            auto *instance = reinterpret_cast<Detail::MetalTextureViewInstance *>(static_cast<std::uintptr_t>(native));
            REQUIRE(instance);
            REQUIRE(instance->descriptor.texture == image.texture);
            REQUIRE(instance->descriptor.baseMip == 0);
            REQUIRE(instance->descriptor.baseLayer == 0);
            REQUIRE(HasTextureUsage(instance->usage, RenderTextureUsage::CopySource));
            REQUIRE(instance->texture);
            return instance->texture;
        }

        /** @brief Copies one completed actual texture pixel with native row alignment; this is not a production frame readback. */
        void CopyPixel(id<MTLBlitCommandEncoder> blit, id<MTLTexture> texture, id<MTLBuffer> bytes, unsigned x, unsigned y,
                       NSUInteger offset) {
            [blit copyFromTexture:texture
                             sourceSlice:0
                             sourceLevel:0
                            sourceOrigin:MTLOriginMake(x, y, 0)
                              sourceSize:MTLSizeMake(1, 1, 1)
                                toBuffer:bytes
                       destinationOffset:offset
                  destinationBytesPerRow:256
                destinationBytesPerImage:256];
        }

        struct PixelValue final {
            std::array<float, 4> color;
            float depth;
        };

        /** @brief Observes actual RGBA16F and Depth32 bytes only after native render completion. */
        PixelValue ReadPixel(NativeOwner &owner, const Scene &scene, unsigned x, unsigned y) {
            REQUIRE(Terminal(owner.commands));
            id<MTLDevice> device = (__bridge id<MTLDevice>)owner.bridge.Device();
            id<MTLCommandQueue> queue = (__bridge id<MTLCommandQueue>)owner.bridge.CommandQueue();
            id<MTLBuffer> bytes = [device newBufferWithLength:512 options:MTLResourceStorageModeShared];
            REQUIRE(bytes);
            owner.copied = [queue commandBuffer];
            id<MTLBlitCommandEncoder> blit = [owner.copied blitCommandEncoder];
            REQUIRE(blit);
            CopyPixel(blit, NativeTexture(owner, scene.color), bytes, x, y, 0);
            CopyPixel(blit, NativeTexture(owner, scene.depth), bytes, x, y, 256);
            [blit endEncoding];
            [owner.copied commit];
            REQUIRE(Terminal(owner.copied));
            std::array<std::uint16_t, 4> half;
            std::memcpy(half.data(), bytes.contents, sizeof(half));
            PixelValue value;
            for (std::size_t i = 0; i < 4; ++i)
                value.color[i] = static_cast<float>(std::bit_cast<_Float16>(half[i]));
            std::memcpy(&value.depth, static_cast<const std::byte *>(bytes.contents) + 256, sizeof(value.depth));
            return value;
        }

        /** @brief Computes an independent pixel-center ray/plane intersection in Metal zero-to-one depth and top-left coordinates. */
        float PlaneDepth(const Math::Mat4 &projection, unsigned x, unsigned y, Math::Vec3 center, Math::Vec3 normal) {
            const auto inverse = Value(Math::TryInverse(projection));
            const float nx = (float(x) + .5F) * 2 / Extent.width - 1, ny = 1 - (float(y) + .5F) * 2 / Extent.height;
            const auto near = Math::TransformPoint(inverse, {nx, ny, 0}), far = Math::TransformPoint(inverse, {nx, ny, 1});
            const auto ray = far - near;
            const auto denominator = Math::Dot(normal, ray);
            REQUIRE(std::abs(denominator) > .0001F);
            const auto intersection = near + ray * (Math::Dot(normal, center - near) / denominator);
            return Math::TransformPoint(projection, intersection).z;
        }

        /** @brief Checks independently expected scene-linear color and ray/plane depth at an actual interior native pixel. */
        void Pixel(NativeOwner &owner, const Scene &scene, Math::Vec3 center, const std::array<float, 4> &expected, bool authoredZ = false,
                   std::optional<Math::Vec3> depthPlane = std::nullopt) {
            const auto ndc = Math::TransformPoint(scene.view.viewProjection, center);
            const auto x = static_cast<unsigned>(std::floor((ndc.x + 1) * Extent.width * .5F));
            const auto y = static_cast<unsigned>(std::floor((1 - ndc.y) * Extent.height * .5F));
            REQUIRE(x > 0);
            REQUIRE(x < Extent.width - 1);
            REQUIRE(y > 0);
            REQUIRE(y < Extent.height - 1);
            const auto actual = ReadPixel(owner, scene, x, y);
            INFO("Actual Metal VFX pixel " << x << ',' << y);
            for (std::size_t i = 0; i < 4; ++i)
                CHECK(std::abs(actual.color[i] - expected[i]) < .02F);
            CHECK(std::abs(actual.depth - PlaneDepth(scene.view.viewProjection, x, y, depthPlane.value_or(center), {0, 0, 1})) < .0001F);
            if (authoredZ)
                CHECK(std::abs(actual.depth - PlaneDepth(scene.view.viewProjection, x, y, Right, {0, 0, 1})) > .001F);
        }

        /** @brief Requires actual indexed workload instances and runs from the canonical scheduler. */
        void VerifyCounts(const Recipe &recipe, unsigned expectedInstances, unsigned expectedDraws, unsigned expectedInstanced) {
            unsigned instances{}, draws{}, instanced{};
            for (const auto &record : recipe.schedule.Workloads())
                for (const auto &draw : std::get<RenderGraphVfx>(record.workload).order->draws) {
                    instances += draw.instanceCount;
                    ++draws;
                    if (draw.instanceCount > 1)
                        ++instanced;
                }
            CHECK(instances == expectedInstances);
            CHECK(draws == expectedDraws);
            CHECK(instanced == expectedInstanced);
        }

        struct EncodingTimes final {
            double encode{}, submit{};
        };

        /** @brief Executes the real graph through its owning frame, measuring only encoding/submission after drawable acquisition. */
        EncodingTimes Execute(NativeOwner &owner, const CompiledRenderGraphExecution &graph,
                              std::span<const RenderGraphPassWorkload> workloads, bool submit, bool gated = false) {
            auto frame = owner.Begin();
            owner.commands = (__bridge id<MTLCommandBuffer>)owner.bridge.CurrentCommandBuffer();
            REQUIRE(owner.commands);
            if (gated) {
                id<MTLDevice> device = (__bridge id<MTLDevice>)owner.bridge.Device();
                owner.gate = [device newSharedEvent];
                REQUIRE(owner.gate);
                [owner.commands encodeWaitForEvent:owner.gate value:1];
            }
            const auto start = std::chrono::steady_clock::now();
            Accepted(frame.ExecuteGraph(graph, workloads));
            const auto encoded = std::chrono::steady_clock::now();
            if (submit)
                Accepted(frame.Present());
            else
                owner.commands = nil;  // Scope destruction aborts this unsent command; it must never be waited as a committed command.
            const auto presented = std::chrono::steady_clock::now();
            return {std::chrono::duration<double, std::milli>(encoded - start).count(),
                    std::chrono::duration<double, std::milli>(presented - encoded).count()};
        }

        /** @brief Verifies source pixels distinguish masked discard, alpha depth rejection, additive color and authored mesh UV/Z. */
        void CheckPixels(NativeOwner &owner, const Scene &scene) {
            constexpr float alpha = 128.0F / 255;
            Pixel(owner, scene, Left, {1 - alpha, 64.0F / 255, alpha, 1});
            Pixel(owner, scene, {-.6F, -1.1F, -3}, {1, 1, 0, 1});
            Pixel(owner, scene, Right + Math::Vec3{0, 0, AuthoredZ}, {0, 128.0F / 255, 0, 1}, true);
        }

        /** @brief Reclaims the exact host upload handle only after all fixture graph CPU borrows are deliberately dropped. */
        std::shared_ptr<PreparedVfxFrameSlot> ReleaseBorrowers(Recipe &recipe, std::vector<RenderGraphPassWorkload> &workloads) {
            REQUIRE_FALSE(recipe.bindings.empty());
            auto slot = std::move(recipe.bindings[0].slot);
            workloads.clear();
            recipe.schedule = {};
            recipe.bindings.clear();
            return slot;
        }

        /** @brief Proves retained graph aliases block overwrite and actual terminal retirement permits a newer canonical publication. */
        void CheckLifetime(NativeOwner &owner, Recipe &recipe, std::vector<RenderGraphPassWorkload> &workloads) {
            REQUIRE_FALSE(recipe.bindings.empty());
            REQUIRE(Terminal(owner.commands));
            {
                auto frame = owner.Begin();
            }  // Pump real completed native leases while deliberately preserving every client alias.
            const std::array<VfxPackedDrawInstance, 1> replacement{};
            const auto revision = recipe.bindings[0].slot->Revision() + 1;
            const auto pending = owner.frontend->UpdateVfxFrame(recipe.bindings[0].slot, revision, replacement);
            REQUIRE(pending.HasError());
            CHECK(pending.ErrorValue().code.Value() == VfxFrameErrors::Pending.code.Value());
            auto slot = ReleaseBorrowers(recipe, workloads);
            REQUIRE(Terminal(owner.commands));
            {
                auto frame = owner.Begin();
            }  // Ordinary next-frame admission pumps actual completed command uses before its unsent abort.
            Accepted(owner.frontend->UpdateVfxFrame(slot, revision, replacement));
            CHECK(slot->Revision() == revision);
            slot.reset();
        }

        /** @brief Drops every client graph borrower while real native flight retains publication until actual completion and owner pump.
         * @details The native completion lease may retain the same slot state; Pending does not identify a particular backend guard.
         */
        void CheckInFlight(NativeOwner &owner, Recipe &recipe, std::vector<RenderGraphPassWorkload> &workloads) {
            REQUIRE(owner.commands.status != MTLCommandBufferStatusCompleted);
            auto slot = ReleaseBorrowers(recipe, workloads);
            REQUIRE(workloads.empty());
            REQUIRE(recipe.bindings.empty());
            REQUIRE(recipe.schedule.Workloads().empty());
            const std::array<VfxPackedDrawInstance, 1> replacement{};
            const auto revision = slot->Revision() + 1;
            const auto pending = owner.frontend->UpdateVfxFrame(slot, revision, replacement);
            REQUIRE(pending.HasError());
            CHECK(pending.ErrorValue().code.Value() == VfxFrameErrors::Pending.code.Value());
            owner.gate.signaledValue = 1;
            REQUIRE(Terminal(owner.commands));
            {
                auto frame = owner.Begin();
            }  // Actual completion is consumed by the existing native/frontend owner.
            Accepted(owner.frontend->UpdateVfxFrame(slot, revision, replacement));
            CHECK(slot->Revision() == revision);
        }

        /** @brief Reports only actual driver timing availability; a zero/missing clock is never invented as a successful GPU duration. */
        std::optional<double> GpuMilliseconds(id<MTLCommandBuffer> commands) {
            const double start = commands.GPUStartTime, end = commands.GPUEndTime;
            if (start <= 0 || end < start)
                return std::nullopt;
            return (end - start) * 1000;
        }
    }  // namespace

    /** @copydoc RunMetalVfxInstancedPixels */
    void RunMetalVfxInstancedPixels(const std::filesystem::path &root) {
        NativeOwner owner;
        owner.Initialize();
        Scene scene(owner, root);
        Recipe recipe{scene};
        const auto graph = Compile(owner, recipe);
        auto workloads = Workloads(recipe, graph);
        VerifyCounts(recipe, 7, 5, 2);
        Execute(owner, graph, workloads, true, true);
        CheckInFlight(owner, recipe, workloads);
        CheckPixels(owner, scene);
    }

    /** @copydoc RunMetalVfxInstancedMaskedDepth */
    void RunMetalVfxInstancedMaskedDepth(const std::filesystem::path &root) {
        NativeOwner owner;
        owner.Initialize();
        Scene scene(owner, root, true);
        Recipe recipe{scene};
        const auto graph = Compile(owner, recipe);
        auto workloads = Workloads(recipe, graph);
        VerifyCounts(recipe, 7, 5, 2);
        Execute(owner, graph, workloads, true);
        REQUIRE(Terminal(owner.commands));
        const auto center = AlongRay(Left, .55F);
        const float size = .8F * .55F;
        const Math::Vec3 offset{std::cos(.17F) * size * .25F, std::sin(.17F) * size * .25F, 0};
        constexpr float alpha = 128.0F / 255;
        Pixel(owner, scene, center - offset, {1 - alpha, 64.0F / 255, alpha, 1}, false, Left);
        Pixel(owner, scene, center + offset, {1, 1 + 64.0F / 255, 1, 1});
    }

    /** @copydoc RunMetalVfxInstancedAbort */
    void RunMetalVfxInstancedAbort(const std::filesystem::path &root) {
        NativeOwner owner;
        owner.Initialize();
        Scene scene(owner, root);
        Recipe recipe{scene};
        const auto graph = Compile(owner, recipe);
        auto workloads = Workloads(recipe, graph);
        VerifyCounts(recipe, 7, 5, 2);
        Execute(owner, graph, workloads, false);
        Execute(owner, graph, workloads, true);
        REQUIRE(Terminal(owner.commands));
        CheckPixels(owner, scene);
        CheckLifetime(owner, recipe, workloads);
    }

    /** @copydoc RunMetalVfxInstancedReference */
    void RunMetalVfxInstancedReference(const std::filesystem::path &root) {
        NativeOwner owner;
        owner.Initialize();
        Scene scene(owner, root);
        const auto start = std::chrono::steady_clock::now();
        Recipe recipe{scene};
        const double packing = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        const auto graph = Compile(owner, recipe);
        auto workloads = Workloads(recipe, graph);
        VerifyCounts(recipe, 7, 5, 2);
        Execute(owner, graph, workloads, true);
        REQUIRE(Terminal(owner.commands));  // One actual cache/device warmup, excluded below.
        id<MTLDevice> device = (__bridge id<MTLDevice>)owner.bridge.Device();
        std::cout << "VFX_INSTANCED_REFERENCE device=" << std::quoted(device.name.UTF8String)
                  << " pixels=64x32 format=RGBA16F depth=Depth32F"
                  << " particles=7 indexedDraws=5 geometryVertices=4 geometryIndices=6 warmupFrames=1 coldSchedulePackMs=" << packing
                  << " timingBudget=UNSPECIFIED_MEASUREMENT_ONLY" << std::endl;
        CHECK(std::isfinite(packing));
        for (unsigned sample = 0; sample < 8; ++sample) {
            const auto cpu = Execute(owner, graph, workloads, true);
            REQUIRE(Terminal(owner.commands));
            const auto gpu = GpuMilliseconds(owner.commands);
            std::cout << "VFX_INSTANCED_SAMPLE ordinal=" << sample << " encodeMs=" << cpu.encode << " submitMs=" << cpu.submit << " gpuMs=";
            if (gpu)
                std::cout << *gpu;
            else
                std::cout << "UNAVAILABLE";
            std::cout << std::endl;
            CHECK(std::isfinite(cpu.encode));
            CHECK(std::isfinite(cpu.submit));
        }
        CheckPixels(owner, scene);
    }
}  // namespace Horo::Render::Test
