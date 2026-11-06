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
#include "RenderFrontendResourceAccess.h"
#include "editor/renderer/metal/SdlMetalPresentationPort.h"
#include "runtime/renderer/modules/metal/MetalResourceInstances.h"

#import <Metal/Metal.h>
#include <SDL3/SDL.h>
#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <optional>
#include <string_view>
#include <thread>

namespace Horo::Render::Test {
    namespace {
        using namespace std::chrono_literals;
        constexpr FramebufferExtent Extent{64, 32};
        constexpr FramebufferExtent BenchmarkExtent{1920, 1080};
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
            FramebufferExtent extent{Extent};

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
            void Initialize(FramebufferExtent requested = Extent, bool validation = true, PresentMode mode = PresentMode::Immediate) {
                extent = requested;
                REQUIRE(SDL_Init(SDL_INIT_VIDEO));
                REQUIRE(SDL_SetHint(SDL_HINT_VIDEO_METAL_AUTO_RESIZE_DRAWABLE, "0"));
                REQUIRE(SDL_SetHint(SDL_HINT_WINDOW_ACTIVATE_WHEN_SHOWN, "0"));
                window = SDL_CreateWindow("Horo indexed VFX qualification", static_cast<int>(extent.width), static_cast<int>(extent.height),
                                          SDL_WINDOW_METAL | SDL_WINDOW_HIDDEN);
                REQUIRE(window);
                REQUIRE(SDL_SetWindowFocusable(window, false));
                presentation = std::make_unique<Editor::SdlMetalPresentationPort>(*window);
                Accepted(RegisterMetalRenderBackend(registry, *presentation, bridge));
                Accepted(registry.Seal());
                frontend = Value(RenderFrontend::Create(registry, RenderBackendId{"metal"},
                                                        {.requirePresentation = true,
                                                         .enableValidation = validation,
                                                         .maxFramesInFlight = 2,
                                                         .presentMode = mode}));
                REQUIRE(SDL_ShowWindow(window));
                const auto topology = Value(Detail::RenderFrontendResourceAccess::QueueAssignments(*frontend));
                queues.assign(topology.begin(), topology.end());
                REQUIRE(frontend->Capabilities().support.features.Supports(RenderCapability::VfxForwardInstancing));
            }

            /** @brief Retries only genuine pending drawable admission under one explicit fixture deadline. */
            RenderFrameScope Begin(std::optional<std::chrono::steady_clock::time_point> limit = {}) {
                auto deadline = std::chrono::steady_clock::now() + 5s;
                if (limit)
                    deadline = std::min(deadline, *limit);
                const auto number = ++frame;
                for (;;) {
                    SDL_PumpEvents();
                    const auto available = Value(frontend->PollPresentationAvailability(extent));
                    if (available) {
                        auto result = frontend->BeginFrame({number, extent});
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
        VfxDrawView View(NativeOwner &owner, FramebufferExtent extent = Extent) {
            const auto view = Value(Math::TryLookAt(Eye, {0, 0, -3}, {0, 1, 0}));
            const auto projection =
                Value(Math::TryPerspective(1.1F, float(extent.width) / float(extent.height), .2F, 80, Math::ClipDepthRange::ZeroToOne));
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

        /** @brief Shares one genuine cold pipeline/texture family while giving every native chunk its own real uniform publication. */
        MaterialFixture ChunkMaterial(NativeOwner &owner, const MaterialFixture &family) {
            MaterialFixture chunk;
            chunk.id = family.id;
            chunk.pipeline = family.pipeline;
            chunk.layout = family.layout;
            chunk.texture = family.texture;
            chunk.sampler = family.sampler;
            const auto bytes = Value(VfxShaderParameterBytes(chunk.layout));
            chunk.instances =
                Value(owner.frontend->CreateBuffer({bytes, RenderBufferUsage::Uniform, RenderBufferAccess::HostVisible}, {})).handle;
            owner.Ready(chunk.instances);
            PrepareSlot(owner, chunk, bytes);
            return chunk;
        }

        /** @brief Owns actual canonical CPU simulation/extraction and actual frontend material/geometry generations. */
        struct Scene final {
            NativeOwner &owner;
            MeshFixture billboard, mesh;
            Image color, depth;
            RenderTargetHandle target;
            std::vector<MaterialFixture> materials, chunkMaterials;
            std::uint32_t benchmarkCount{};
            FramebufferExtent extent{Extent};
            std::uint32_t maximumInstances{64}, maximumDraws{16};
            std::unique_ptr<Vfx::VfxWorld> world;
            VfxDrawView view;
            std::array<Sha256Digest, 2> artifactKeys;

            /** @brief Creates real fixed scene resources outside frame encoding and measurement windows. */
            Scene(NativeOwner &native, const std::filesystem::path &root, bool cutout = false, std::uint32_t measuredCount = 0)
                : owner(native), benchmarkCount(measuredCount), extent(measuredCount ? BenchmarkExtent : Extent),
                  maximumInstances(measuredCount ? measuredCount : 64), maximumDraws(measuredCount ? 512 : 16), view(View(native, extent)) {
                const std::size_t bytes = measuredCount ? 32'000'000 : 1'000'000;
                Accepted(owner.frontend->PrepareVfxScheduling({maximumInstances, 1, bytes, maximumDraws}));
                billboard = Mesh(owner, 0);
                mesh = Mesh(owner, AuthoredZ);
                color = Texture(owner,
                                {.extent = extent,
                                 .format = RenderTextureFormat::Rgba16Float,
                                 .usage = RenderTextureUsage::RenderAttachment | RenderTextureUsage::CopySource},
                                RenderTextureAspect::Color);
                depth = Texture(owner,
                                {.extent = extent,
                                 .format = RenderTextureFormat::Depth32Float,
                                 .usage = RenderTextureUsage::RenderAttachment | RenderTextureUsage::CopySource},
                                RenderTextureAspect::Depth);
                target = Value(owner.frontend->CreateRenderTarget(
                                   {.colorAttachment = color.view, .depthAttachment = depth.view, .extent = extent}))
                             .handle;
                owner.Ready(target);
                const std::array artifacts{Load(root, false), Load(root, true)};
                artifactKeys = {artifacts[0].artifactKey, artifacts[1].artifactKey};
                for (unsigned i = 0; i < 5; ++i)
                    materials.push_back(Material(owner, i, artifacts, cutout));
                const auto scope = Value(Vfx::VfxIdentityScope::Create(17));
                const auto emitters = Emitters(scope);
                world = Value(Vfx::VfxWorld::Prepare(scope, emitters, {16, maximumInstances, ExtractionDrawCredits(), 2, bytes}));
                Populate(emitters);
            }

            /** @brief Closes only actual material/world owners while the selected frontend still exists. */
            ~Scene() {
                if (world)
                    static_cast<void>(world->Shutdown());
                CloseMaterials(chunkMaterials);
                CloseMaterials(materials);
            }

            /** @brief Tears down only fixture-owned immutable material references before the frontend. */
            static void CloseMaterials(std::vector<MaterialFixture> &values) {
                for (auto &material : values) {
                    material.slot.reset();
                    if (material.table && material.table->Shutdown().HasError())
                        std::terminate();
                }
            }

            /** @brief Distributes the exact total deterministically over five declared blend/topology families. */
            std::uint32_t FamilyCount(unsigned index) const noexcept {
                return benchmarkCount / 5 + std::uint32_t(index < benchmarkCount % 5);
            }

            /** @brief Reserves canonical conservative translucent-per-particle and solid depth/color extraction credits. */
            std::uint32_t ExtractionDrawCredits() const noexcept {
                return benchmarkCount ? FamilyCount(2) + 7 : 128;
            }

            /** @brief Prepares one unique upload/material generation per canonical 128-instance chunk, outside measurement. */
            void PrepareChunks() {
                for (unsigned family = 0; family < 5; ++family) {
                    const auto chunks = (FamilyCount(family) + MaximumVfxDrawInstances - 1) / MaximumVfxDrawInstances;
                    for (std::size_t chunk = 1; chunk < chunks; ++chunk)
                        chunkMaterials.push_back(ChunkMaterial(owner, materials[family]));
                }
            }

            /** @brief Transfers a distinct real publication for each canonical scheduled chunk. */
            MaterialFixture &NextMaterial(MaterialRuntimeId id) {
                auto family = std::ranges::find(materials, id, &MaterialFixture::id);
                REQUIRE(family != materials.end());
                if (family->slot)
                    return *family;
                auto chunk = std::ranges::find_if(chunkMaterials, [&](const auto &candidate) {
                    return candidate.id == id && bool(candidate.slot);
                });
                REQUIRE(chunk != chunkMaterials.end());
                return *chunk;
            }

            /** @brief Seeds a bounded on-screen grid whose footprint scales with count, keeping the five layer mix explicit. */
            void PopulateBenchmarkEmitter(const Vfx::VfxEmitterPreparation &emitter, unsigned index) {
                Accepted(world->Play(emitter.effect));
                const auto particles = Value(world->Particles(emitter.effect, emitter.emitter));
                const auto count = FamilyCount(index);
                for (std::uint32_t i = 0; i < count; ++i)
                    Value(particles->Spawn(Value(Vfx::ParticleSimulationId::Create(index * 16'384 + i + 1))));
                auto values = Value(particles->View());
                const auto columns = std::uint32_t(std::ceil(std::sqrt(float(count) * 2.F)));
                const auto rows = (count + columns - 1) / columns;
                const float size = .72F * std::min(4.F / float(columns), 2.F / float(rows));
                constexpr std::array<std::uint32_t, 5> colors{0xFF0000FF, 0x00FF00FF, 0x0000FF80, 0xFFFFFFFF, 0x00400000};
                constexpr std::array depths{-3.4F, -3.9F, -3.2F, -3.1F, -3.F};
                for (std::uint32_t i = 0; i < count; ++i) {
                    values.positionX[i] = -2.F + (float(i % columns) + .5F) * 4.F / float(columns);
                    values.positionY[i] = -1.F + (float(i / columns) + .5F) * 2.F / float(rows);
                    values.positionZ[i] = depths[index];
                    values.sizeX[i] = size;
                    values.sizeY[i] = size;
                    values.rotation[i] = .17F;
                    values.packedColor[i] = colors[index];
                }
            }

            /** @brief Preserves canonical solid-mesh admission; only translucent/additive emitters use camera-facing billboards. */
            bool UsesBillboard(unsigned index) const noexcept {
                return index == 2 || index == 4;
            }

            /** @brief Selects only actual mesh identities; camera-facing billboard emitters carry no mesh handle. */
            RenderMeshHandle EmitterMesh(unsigned index) const noexcept {
                if (UsesBillboard(index))
                    return {};
                return index == 1 ? mesh.mesh : billboard.mesh;
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
                                   .particles = {.buffer = {scope, i, 1}, .capacity = benchmarkCount ? FamilyCount(i) : 8},
                                   .material = materials[i].id,
                                   .materialBlend = blends[i],
                                   .mesh = EmitterMesh(i),
                                   .blend = passes[i],
                                   .topology = UsesBillboard(i) ? VfxPrimitiveTopology::CameraFacingBillboard
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
                if (benchmarkCount) {
                    PrepareChunks();
                    for (unsigned i = 0; i < 5; ++i)
                        PopulateBenchmarkEmitter(emitters[i], i);
                    return;
                }
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
            std::array<RenderGraphResourceId, 5> samples;
            for (std::size_t i = 0; i < samples.size(); ++i)
                samples[i] = Value(recipe.builder.ImportTexture(scene.materials[i].texture.texture, RenderGraphResourceClass::Persistent));
            for (const auto &record : recipe.schedule.Workloads()) {
                const auto &vfx = std::get<RenderGraphVfx>(record.workload);
                const auto batches = vfx.snapshot.Batches();
                for (std::size_t first = 0; first < vfx.order->instances.size();) {
                    const auto &batch = batches[vfx.order->instances[first].batch];
                    auto &material = scene.NextMaterial(batch.material);
                    const auto family = std::ranges::find(scene.materials, batch.material, &MaterialFixture::id);
                    REQUIRE(family != scene.materials.end());
                    const auto familyIndex = static_cast<std::size_t>(family - scene.materials.begin());
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
                         .parameters = Value(recipe.builder.ImportBuffer(material.instances, RenderGraphResourceClass::Persistent)),
                         .sampledTexture = samples[familyIndex],
                         .slot = std::move(material.slot)});
                    const auto batchIndex = vfx.order->instances[first].batch;
                    std::size_t count{};
                    while (first < vfx.order->instances.size() && vfx.order->instances[first].batch == batchIndex &&
                           count < MaximumVfxDrawInstances) {
                        ++first;
                        ++count;
                    }
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
                                          .maximumInstances = scene.maximumInstances,
                                          .remainingDrawCalls = scene.maximumDraws,
                                          .opaqueBegin = clearDepth,
                                          .opaqueComplete = opaque,
                                          .outputTransform = output,
                                          .sceneColor = color,
                                          .sceneDepth = depth};
            const auto extracted = scene.world->Extract(scene.view.frame, {}, 1);
            REQUIRE(extracted.report.status == Vfx::VfxExtractionStatus::Complete);
            REQUIRE(extracted.report.omittedInstances == 0);
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
            : builder(Value(RenderGraphBuilder::Create({.maxPasses = 16,
                                                        .maxResources = scene.benchmarkCount ? 1024U : 32U,
                                                        .maxUsages = scene.benchmarkCount ? 2048U : 128U,
                                                        .maxDependencies = 64}))) {
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
                              std::span<const RenderGraphPassWorkload> workloads, bool submit, bool gated = false,
                              std::optional<std::chrono::steady_clock::time_point> deadline = {}) {
            auto frame = owner.Begin(deadline);
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

        using BenchmarkClock = std::chrono::steady_clock;
        constexpr std::size_t BenchmarkMaximumFrames = 10'000;

        /** @brief Prints only JSON-safe real diagnostic text outside the measured window. */
        void JsonText(std::FILE *file, std::string_view value) {
            std::fputc('"', file);
            for (const unsigned char character : value) {
                if (character < 32)
                    std::fprintf(file, "\\u%04x", unsigned(character));
                else {
                    if (character == '"' || character == '\\')
                        std::fputc('\\', file);
                    std::fputc(character, file);
                }
            }
            std::fputc('"', file);
        }

        /** @brief Unknown/nonfinite timestamps are explicit null values, never fabricated zero durations. */
        void JsonNumber(std::FILE *file, double value) {
            if (std::isfinite(value))
                std::fprintf(file, "%.17g", value);
            else
                std::fputs("null", file);
        }

        struct BenchmarkSample final {
            EncodingTimes cpu;
            double gpuStart{}, gpuEnd{};
            unsigned nativeStatus{};
            bool terminal{};
        };

        /** @brief Classifies genuine native timestamp evidence without silently discarding any sample. */
        const char *GpuStatus(const BenchmarkSample &sample) {
            if (!std::isfinite(sample.gpuStart) || !std::isfinite(sample.gpuEnd) || sample.gpuStart < 0 || sample.gpuEnd < sample.gpuStart)
                return "invalid";
            return sample.gpuStart > 0 ? "available" : "unavailable";
        }

        /** @brief Retains bounded observations and writes on normal completion or assertion unwind, outside the steady window. */
        struct BenchmarkReceipt final {
            std::unique_ptr<std::FILE, decltype(&std::fclose)> file{nullptr, &std::fclose};
            std::vector<BenchmarkSample> samples;
            std::uint32_t instances{};
            std::size_t warmupAttempts{}, warmupCompleted{}, measuredAttempts{};
            std::optional<BenchmarkSample> lastWarmup;
            std::size_t lastWarmupOrdinal{};
            const char *phase{"initializing"};
            double measuredSeconds{};
            std::optional<double> coldSchedulePackMs;
            bool complete{}, written{};

            BenchmarkReceipt(const std::filesystem::path &path, std::uint32_t count) : instances(count) {
                REQUIRE(path.is_absolute());
                REQUIRE_FALSE(std::filesystem::exists(path));
                file.reset(std::fopen(path.c_str(), "wx"));
                REQUIRE(file);
                std::fprintf(file.get(),
                             "{\"type\":\"invocation\",\"schema\":1,\"profileRevision\":2,\"instances\":%u,"
                             "\"requestedExtent\":[1920,1080],\"presentModeRequested\":\"Fifo\","
                             "\"budgetStatus\":\"UNSET_NON_GATING\",\"deviceQualification\":false}\n",
                             count);
                REQUIRE(std::fflush(file.get()) == 0);
                samples.reserve(BenchmarkMaximumFrames);
            }

            /** @brief Preserves partial evidence before the native owner handles terminal cleanup. */
            ~BenchmarkReceipt() {
                Write();
            }

            /** @brief Records authentic selected native device and compiled artifact identity before warmup. */
            void Describe(NativeOwner &owner, const Scene &scene, const Recipe &recipe) {
                auto *out = file.get();
                id<MTLDevice> device = (__bridge id<MTLDevice>)owner.bridge.Device();
                std::fputs("{\"type\":\"cohort\",\"device\":", out);
                const auto *name = device.name.UTF8String;
                JsonText(out, name ? name : "UNAVAILABLE");
                std::fputs(",\"compiler\":", out);
                JsonText(out, __VERSION__);
#ifdef NDEBUG
                std::fputs(",\"ndebug\":true", out);
#else
                std::fputs(",\"ndebug\":false", out);
#endif
                std::
                    fprintf(out,
                            ",\"validationRequested\":false,\"presentModeRequested\":\"Fifo\","
                            "\"configuredMaxFramesInFlight\":2,\"measurementInflightDepth\":1,"
                            "\"serialCompletionFpsIsNotProductThroughput\":true,\"indexedDraws\":%u,\"maximumInstancesPerDraw\":128,"
                            "\"meshVertices\":4,\"meshIndices\":6,\"authoredMeshLocalZ\":%.9g,\"color\":\"RGBA16F\",\"depth\":\"Depth32F\","
                            "\"staticPreparedGraph\":true,\"catchAssertionInstrumentation\":true,"
                            "\"gridWorldSpan\":[4,2],\"gridColumns\":\"ceil(sqrt(familyCount*2))\","
                            "\"objectSizeWorld\":\"0.72*min(4/columns,2/rows)\","
                            "\"completionObservation\":\"serial_native_status_poll_1ms\","
                            "\"cpuScope\":\"encode_and_present_calls_after_drawable_acquisition\","
                            "\"gpuScope\":\"entire_native_command_buffer\",\"wholeFrameCpuMeasured\":false,"
                            "\"minimumWarmupFrames\":300,\"minimumMeasuredFrames\":1200,\"minimumMeasuredSeconds\":20,"
                            "\"maximumMeasuredFrames\":10000,\"maximumIterationSeconds\":300,\"families\":[",
                            recipe.schedule.drawCalls, double(AuthoredZ));
                Families(out, scene);
                std::fprintf(out, "],\"extractorDrawCredits\":%u,\"nativeDrawCapacity\":%u,\"compiledArtifactKeys\":[",
                             scene.ExtractionDrawCredits(), scene.maximumDraws);
                JsonText(out, FormatSha256(scene.artifactKeys[0]));
                std::fputc(',', out);
                JsonText(out, FormatSha256(scene.artifactKeys[1]));
                std::fputs("]}\n", out);
                REQUIRE(std::fflush(out) == 0);
            }

            /** @brief Exposes the deterministic submitted geometry/depth/blend mix, not an inferred production workload. */
            static void Families(std::FILE *out, const Scene &scene) {
                constexpr std::array names{"opaque_instanced_quad", "opaque_authored_z_mesh", "translucent_billboard",
                                           "masked_instanced_quad", "additive_billboard"};
                for (unsigned i = 0; i < names.size(); ++i) {
                    if (i != 0)
                        std::fputc(',', out);
                    const auto columns = std::uint32_t(std::ceil(std::sqrt(float(scene.FamilyCount(i)) * 2.F)));
                    const auto rows = (scene.FamilyCount(i) + columns - 1) / columns;
                    const float size = .72F * std::min(4.F / float(columns), 2.F / float(rows));
                    constexpr std::array depths{-3.4F, -3.9F, -3.2F, -3.1F, -3.F};
                    std::fprintf(out,
                                 "{\"name\":\"%s\",\"instances\":%u,\"originZ\":%.9g,\"columns\":%u,\"rows\":%u,"
                                 "\"sizeWorld\":%.9g,\"depthTest\":true,\"depthWrite\":%s,\"maskedDiscard\":%s}",
                                 names[i], scene.FamilyCount(i), double(depths[i]), columns, rows, double(size),
                                 i != 2 && i != 4 ? "true" : "false", i == 3 ? "true" : "false");
                }
            }

            /** @brief Writes one raw completed or failed native observation; no percentile/filter is applied here. */
            static void Sample(std::FILE *out, const BenchmarkSample &sample, std::size_t ordinal, const char *kind = "sample") {
                std::fprintf(out, "{\"type\":\"%s\",\"ordinal\":%zu,\"encodeMs\":", kind, ordinal);
                JsonNumber(out, sample.cpu.encode);
                std::fputs(",\"submitMs\":", out);
                JsonNumber(out, sample.cpu.submit);
                std::fprintf(out, ",\"gpuStatus\":\"%s\",\"gpuStartSeconds\":", GpuStatus(sample));
                JsonNumber(out, sample.gpuStart);
                std::fputs(",\"gpuEndSeconds\":", out);
                JsonNumber(out, sample.gpuEnd);
                std::fputs(",\"commandBufferGpuMs\":", out);
                if (std::strcmp(GpuStatus(sample), "available") == 0)
                    JsonNumber(out, (sample.gpuEnd - sample.gpuStart) * 1000);
                else
                    std::fputs("null", out);
                std::fprintf(out, ",\"nativeStatus\":%u,\"terminalCompleted\":%s}\n", sample.nativeStatus,
                             sample.terminal ? "true" : "false");
            }

            /** @brief Flushes bounded evidence exactly once, including honest partial state on assertion failure. */
            bool Write() noexcept {
                if (written || !file)
                    return true;
                written = true;
                if (lastWarmup)
                    Sample(file.get(), *lastWarmup, lastWarmupOrdinal, "warmup-last");
                for (std::size_t i = 0; i < samples.size(); ++i)
                    Sample(file.get(), samples[i], i);
                std::fprintf(file.get(),
                             "{\"type\":\"summary\",\"phase\":\"%s\",\"complete\":%s,\"warmupCompleted\":%zu,"
                             "\"warmupAttempts\":%zu,\"measuredAttempts\":%zu,\"rawSamples\":%zu,\"measuredSeconds\":%.17g,"
                             "\"coldSchedulePackMs\":",
                             phase, complete ? "true" : "false", warmupCompleted, warmupAttempts, measuredAttempts, samples.size(),
                             measuredSeconds);
                if (coldSchedulePackMs)
                    JsonNumber(file.get(), *coldSchedulePackMs);
                else
                    std::fputs("null", file.get());
                std::fputs(",\"sampleFiltering\":false,\"thresholdApplied\":false}\n", file.get());
                return std::fflush(file.get()) == 0 && std::ferror(file.get()) == 0;
            }
        };

        /** @brief Polls real native completion only within the one iteration's finite diagnostic deadline. */
        bool BenchmarkTerminal(id<MTLCommandBuffer> commands, BenchmarkClock::time_point deadline) {
            if (!commands)
                return false;
            while (commands.status != MTLCommandBufferStatusCompleted && commands.status != MTLCommandBufferStatusError) {
                if (BenchmarkClock::now() >= deadline)
                    return false;
                std::this_thread::sleep_for(1ms);
            }
            return commands.status == MTLCommandBufferStatusCompleted;
        }

        /** @brief Executes the unchanged actual indexed graph, bounds native temporaries and copies real terminal/timestamp evidence. */
        BenchmarkSample BenchmarkFrame(NativeOwner &owner, const CompiledRenderGraphExecution &graph,
                                       std::span<const RenderGraphPassWorkload> workloads, BenchmarkClock::time_point deadline) {
            @autoreleasepool {
                const auto cpu = Execute(owner, graph, workloads, true, false, deadline);
                const bool terminal = BenchmarkTerminal(owner.commands, std::min(deadline, BenchmarkClock::now() + 5s));
                return {cpu, owner.commands.GPUStartTime, owner.commands.GPUEndTime, unsigned(owner.commands.status), terminal};
            }
        }

        /** @brief Uses the declared count and duration minimums together; neither substitutes for the other. */
        bool NeedsBenchmarkSamples(std::size_t count, double seconds) {
            return count < 1200 || seconds < 20;
        }

        /** @brief Runs exactly 300 completed warmup frames, excluded from all raw measured samples. */
        void BenchmarkWarmup(NativeOwner &owner, const CompiledRenderGraphExecution &graph,
                             std::span<const RenderGraphPassWorkload> workloads, BenchmarkReceipt &receipt,
                             BenchmarkClock::time_point deadline) {
            receipt.phase = "warmup";
            for (unsigned i = 0; i < 300; ++i) {
                REQUIRE(BenchmarkClock::now() < deadline);
                ++receipt.warmupAttempts;
                const auto sample = BenchmarkFrame(owner, graph, workloads, deadline);
                receipt.lastWarmup = sample;
                receipt.lastWarmupOrdinal = i;
                REQUIRE(sample.terminal);
                ++receipt.warmupCompleted;
            }
        }

        /** @brief Retains every measured attempt; protocol failure exits with a partial receipt rather than silently filtering samples. */
        void BenchmarkWindow(NativeOwner &owner, const CompiledRenderGraphExecution &graph,
                             std::span<const RenderGraphPassWorkload> workloads, BenchmarkReceipt &receipt,
                             BenchmarkClock::time_point deadline) {
            receipt.phase = "measuring";
            const auto start = BenchmarkClock::now();
            while (NeedsBenchmarkSamples(receipt.samples.size(), receipt.measuredSeconds)) {
                REQUIRE(receipt.samples.size() < BenchmarkMaximumFrames);
                REQUIRE(BenchmarkClock::now() < deadline);
                ++receipt.measuredAttempts;
                receipt.samples.push_back(BenchmarkFrame(owner, graph, workloads, deadline));
                receipt.measuredSeconds = std::chrono::duration<double>(BenchmarkClock::now() - start).count();
                const auto &sample = receipt.samples.back();
                REQUIRE(sample.terminal);
                REQUIRE(std::isfinite(sample.cpu.encode));
                REQUIRE(std::isfinite(sample.cpu.submit));
            }
            receipt.complete = true;
            receipt.phase = "finished";
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

    /** @copydoc RunMetalVfxInstancedBenchmark */
    void RunMetalVfxInstancedBenchmark(const std::filesystem::path &root, std::uint32_t instances, const std::filesystem::path &path) {
        REQUIRE((instances == 1024 || instances == 4096 || instances == 16384));
        const auto deadline = BenchmarkClock::now() + 300s;
        NativeOwner owner;
        BenchmarkReceipt receipt{path, instances};
        owner.Initialize(BenchmarkExtent, false, PresentMode::Fifo);
        Scene scene(owner, root, true, instances);
        const auto cold = BenchmarkClock::now();
        Recipe recipe{scene};
        receipt.coldSchedulePackMs = std::chrono::duration<double, std::milli>(BenchmarkClock::now() - cold).count();
        const auto graph = Compile(owner, recipe);
        auto workloads = Workloads(recipe, graph);
        std::uint32_t expectedDraws{};
        for (unsigned family = 0; family < 5; ++family)
            expectedDraws += (scene.FamilyCount(family) + 127) / 128;
        VerifyCounts(recipe, instances, expectedDraws, expectedDraws);
        receipt.Describe(owner, scene, recipe);
        BenchmarkWarmup(owner, graph, workloads, receipt, deadline);
        BenchmarkWindow(owner, graph, workloads, receipt, deadline);
        REQUIRE(receipt.Write());
    }

}  // namespace Horo::Render::Test
