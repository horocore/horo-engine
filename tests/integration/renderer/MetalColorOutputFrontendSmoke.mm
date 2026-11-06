#include "ColorOutputCook.h"
#include "ColorOutputShaderRecipe.h"
#include "ExposureGpuCook.h"
#include "Horo/Runtime/Render/MaterialBinding.h"
#include "Horo/Runtime/Render/MaterialBindingErrors.h"
#include "Horo/Runtime/Render/RenderFrontend.h"
#include "Horo/Runtime/Render/RenderGraphWorkload.h"
#include "MetalColorOutputLutReference.h"
#include "editor/renderer/metal/SdlMetalPresentationPort.h"
#include "runtime/renderer/frontend/RenderFrontendResourceAccess.h"
#include "runtime/renderer/modules/metal/MetalRenderBackendErrors.h"
#include "runtime/renderer/modules/metal/MetalResourceInstances.h"

#import <Metal/Metal.h>
#include <SDL3/SDL.h>
#include <algorithm>
#include <array>
#include <bit>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <map>
#include <nlohmann/json.hpp>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

namespace Horo::Render::Test {
    namespace {
        using namespace std::chrono_literals;
        constexpr FramebufferExtent InitialExtent{16, 16};

        /** @brief Preserves actual cook/frontend/native failure identities at every admission boundary. */
        template <class T> T Take(Result<T> result) {
            INFO((result.HasError() ? result.ErrorValue().code.Value() + ": " + result.ErrorValue().message : "accepted"));
            REQUIRE(result.HasValue());
            return std::move(result).Value();
        }

        /** @brief Requires actual success without hiding the original typed failure. */
        void Accepted(const Result<void> &result) {
            INFO((result.HasError() ? result.ErrorValue().code.Value() + ": " + result.ErrorValue().message : "accepted"));
            REQUIRE(result.HasValue());
        }

        /** @brief Records a bounded original error/cause chain without inventing missing lower-level provenance. */
        std::string ErrorTrace(const Error &error) {
            std::string trace;
            const Error *current = &error;
            for (std::size_t depth = 0; current && depth < 8; ++depth) {
                trace += current->domain.Value().substr(0, 128) + "/" + current->code.Value().substr(0, 128) + ": " +
                         current->message.substr(0, 512) + "\n";
                current = current->cause.Get();
            }
            return trace;
        }

        /** @brief Matches an exact original typed failure within at most eight actual owned cause nodes. */
        bool HasCause(const Error &error, const ErrorCodeDescriptor &expected) noexcept {
            const Error *current = error.cause.Get();
            for (std::size_t depth = 0; current && depth < 8; ++depth) {
                if (current->domain.Value() == expected.domain.Value() && current->code.Value() == expected.code.Value())
                    return true;
                current = current->cause.Get();
            }
            return false;
        }

        /** @brief Resolves an explicitly supplied package root, never discovering or generating runtime assets. */
        std::filesystem::path PackageRoot(const char *variable) {
            const char *value = std::getenv(variable);
            if (!value)
                SKIP("Supply genuine HORO_COLOR_OUTPUT_PACKAGE and HORO_EXPOSURE_GPU_PACKAGE for native qualification.");
            const std::filesystem::path root{value};
            REQUIRE(root.is_absolute());
            return root;
        }

        /** @brief Reconstructs only a genuine normalized compiler record, preserving every native binding. */
        ShaderReflectionCandidate Candidate(const NormalizedShaderReflection &r) {
            return {r.backend, r.interfaceSchemaVersion, r.bindings, r.parameters, r.stageInterface, r.targetBindings, r.sourceMap};
        }

        /** @brief Loads the actual independently cooked exposure family through canonical reflected-artifact decoding. */
        CookedExposureGpuKernel ExposureFamily() {
            const auto root = PackageRoot("HORO_EXPOSURE_GPU_PACKAGE");
            const auto prefixes = Editor::Offline::ExposureGpuCookPrefixes();
            const auto target = Editor::Offline::ExposureGpuMetalCookTarget();
            CookedExposureGpuKernel family;
            for (std::size_t i = 0; i < family.stages.size(); ++i) {
                auto &stage = family.stages[i];
                stage.target = target;
                stage.manifest = MakeExposureGpuShaderManifest(static_cast<ExposureGpuStage>(i), target);
                auto payload =
                    Take(Application::Internal::ReadShaderCookInput(root / (std::string{prefixes[i]} + ".horoshdr"), 16U << 20U));
                const auto metadata =
                    Take(Application::Internal::ReadShaderCookInput(root / (std::string{prefixes[i]} + ".reflection.json"), 16U << 20U));
                auto reflected = Take(DecodeReflectedShaderArtifact(stage.manifest, target, payload, metadata));
                stage.reflection = Candidate(reflected.reflection);
                stage.artifact = {.backend = target.backend,
                                  .payloadFormat = target.payloadFormat,
                                  .artifactKey = reflected.compiledArtifactKey,
                                  .payload = std::move(payload),
                                  .reflection = std::make_shared<const ReflectedShaderArtifact>(std::move(reflected))};
            }
            Take(ValidateCookedExposureGpuKernel(family, ShaderTargetBackend::Metal));
            return family;
        }

        /** @brief Contains all native waits inside this explicit five-second test-only deadline. */
        bool Terminal(id<MTLCommandBuffer> command) {
            const auto deadline = std::chrono::steady_clock::now() + 5s;
            while (command && command.status != MTLCommandBufferStatusCompleted && command.status != MTLCommandBufferStatusError) {
                if (std::chrono::steady_clock::now() >= deadline)
                    return false;
                std::this_thread::sleep_for(1ms);
            }
            return !command || command.status == MTLCommandBufferStatusCompleted;
        }

        /** @brief Retains the entire selected owner until real native shutdown retirement is complete. */
        bool Drain(RenderFrontend &frontend) {
            if (frontend.BeginShutdown().HasError())
                return false;
            const auto deadline = std::chrono::steady_clock::now() + 5s;
            while (std::chrono::steady_clock::now() < deadline) {
                const auto progress = frontend.PollDrain();
                if (progress.HasError())
                    return false;
                if (progress.Value() == RenderBackendDrainProgress::Drained)
                    return true;
                std::this_thread::sleep_for(1ms);
            }
            return false;
        }

        /** @brief Hashes the exact authored red-fastest RGB as portable little-endian float32 words. */
        Sha256Digest LookPayloadDigest(std::span<const float> rgb) {
            Sha256Builder digest;
            for (const auto value : rgb) {
                const auto bits = std::bit_cast<std::uint32_t>(value);
                const std::array bytes{std::byte(bits & 255U), std::byte((bits >> 8U) & 255U), std::byte((bits >> 16U) & 255U),
                                       std::byte(bits >> 24U)};
                REQUIRE(digest.Update(bytes));
            }
            return digest.Finalize();
        }

        /** @brief Admits the authored nonidentity cube through the real immutable creative-look factory. */
        std::shared_ptr<const CookedColorTransform> MakeLook(const CookedColorOutputProgram &output, ColorLutInterpolation interpolation) {
            ColorTransformDescriptor descriptor;
            descriptor.configurationRelease = output.Descriptor().configurationRelease;
            descriptor.configurationDigest = output.Descriptor().configurationDigest;
            descriptor.referenceDigest = Take(ParseSha256(LutOracle::ReferenceDigest));
            descriptor.payloadDigest = LookPayloadDigest(LutOracle::CubeRgb);
            descriptor.interpolation = interpolation;
            descriptor.dimension = 2;
            descriptor.domainMinimum = LutOracle::DomainMinimum;
            descriptor.domainMaximum = LutOracle::DomainMaximum;
            return Take(CookedColorTransform::Create(descriptor, LutOracle::CubeRgb));
        }

        /** @brief Owns a genuinely foreign Metal frontend while borrowing the outer fixture's live SDL session. */
        class ForeignTextureOwner final {
        public:
            SDL_Window *window{};
            RenderBackendRegistry registry;
            MetalEditorGraphicsBridge bridge;
            std::unique_ptr<Editor::SdlMetalPresentationPort> presentation;
            std::unique_ptr<RenderFrontend> frontend;
            RenderTextureViewHandle view;

            /** @brief Creates an actual independent device/resource owner without reinitializing or quitting shared SDL. */
            explicit ForeignTextureOwner(const CookedColorTransform &look) {
                try {
                    window = SDL_CreateWindow("Horo foreign creative resource qualification", 64, 64, SDL_WINDOW_METAL | SDL_WINDOW_HIDDEN);
                    REQUIRE(window);
                    REQUIRE(SDL_SetWindowFocusable(window, false));
                    presentation = std::make_unique<Editor::SdlMetalPresentationPort>(*window);
                    Accepted(RegisterMetalRenderBackend(registry, *presentation, bridge));
                    Accepted(registry.Seal());
                    frontend = Take(RenderFrontend::Create(registry, RenderBackendId{"metal"},
                                                           {.requirePresentation = true,
                                                            .enableValidation = true,
                                                            .maxFramesInFlight = 2,
                                                            .presentMode = PresentMode::Immediate}));
                    Prepare(look);
                } catch (...) {
                    if (!Close())
                        std::terminate();
                    throw;
                }
            }

            /** @brief Proves foreign native retirement before the outer fixture can destroy its SDL session. */
            ~ForeignTextureOwner() {
                if (!Close())
                    std::terminate();
            }

            /** @brief Uploads the same genuine factory atlas through the foreign public frontend. */
            void Prepare(const CookedColorTransform &look) {
                const auto descriptor = look.AtlasDescriptor();
                const auto texture = Take(frontend->CreateTexture(descriptor, look.AtlasUpload())).handle;
                Take(frontend->ProcessResourceRequests());
                REQUIRE(Take(frontend->ResourceState(texture)) == RenderResourceState::Ready);
                view = Take(frontend->CreateTextureView({.texture = texture, .format = descriptor.format})).handle;
                Take(frontend->ProcessResourceRequests());
                REQUIRE(Take(frontend->ResourceState(view)) == RenderResourceState::Ready);
            }

            /** @brief Contains bounded real native cleanup while deliberately leaving borrowed SDL globally untouched. */
            bool Close() noexcept {
                try {
                    if (frontend && !Drain(*frontend))
                        return false;
                    frontend.reset();
                    presentation.reset();
                    if (window)
                        SDL_DestroyWindow(window);
                    window = nullptr;
                    return true;
                } catch (...) {
                    return false;
                }
            }
        };

        /** @brief Actual issued texture/view pair; no native token or imported identity is manufactured. */
        struct Texture final {
            RenderTextureHandle handle;
            RenderTextureViewHandle view;
        };

        /** @brief Actual cooked frontend modules/pipeline and its unchanged normalized material layout. */
        struct Program final {
            RenderShaderModuleHandle vertex, fragment;
            RenderPipelineHandle pipeline;
            MaterialBindingLayout layout;
        };

        /** @brief Owns synchronous graph borrows and exact imported initial-content evidence. */
        struct Graph final {
            RenderGraphBuilder builder{
                Take(RenderGraphBuilder::Create({.maxPasses = 8, .maxResources = 24, .maxUsages = 48, .maxDependencies = 16}))};
            std::vector<RenderGraphImportedState> initial;
            std::vector<std::pair<RenderBufferHandle, RenderGraphResourceId>> buffers;
            std::vector<std::pair<RenderTextureHandle, RenderGraphResourceId>> textures;
            std::vector<RenderGraphPassWorkload> workloads;
            RenderQueueId queue;
            std::map<RenderBufferHandle, RenderGraphLogicalState> &bufferStates;
            std::map<RenderTextureHandle, RenderGraphLogicalState> &textureStates;
            std::map<RenderGraphResourceId, RenderGraphLogicalState> last;

            /** @brief Borrows only the fixture's actual completed per-resource state evidence. */
            Graph(RenderQueueId queue, std::map<RenderBufferHandle, RenderGraphLogicalState> &buffers,
                  std::map<RenderTextureHandle, RenderGraphLogicalState> &textures)
                : queue(queue), bufferStates(buffers), textureStates(textures) {}

            /** @brief Imports one actual initialized buffer generation once, preserving its real prior contents. */
            RenderGraphResourceId Buffer(RenderBufferHandle handle) {
                const auto found = std::ranges::find_if(buffers, [handle](const auto &entry) {
                    return entry.first == handle;
                });
                if (found != buffers.end())
                    return found->second;
                const auto id = Take(builder.ImportBuffer(handle, RenderGraphResourceClass::Persistent));
                buffers.emplace_back(handle, id);
                initial.push_back({id, bufferStates.at(handle), RenderGraphInitialContents::Preserved});
                return id;
            }

            /** @brief Imports an exact frontend texture, declaring discard only for a dominating attachment write. */
            RenderGraphResourceId Image(RenderTextureHandle handle, bool discarded = false) {
                const auto found = std::ranges::find_if(textures, [handle](const auto &entry) {
                    return entry.first == handle;
                });
                if (found != textures.end())
                    return found->second;
                const auto id = Take(builder.ImportTexture(handle, RenderGraphResourceClass::Persistent));
                textures.emplace_back(handle, id);
                initial.push_back({id, discarded ? RenderGraphLogicalState{} : textureStates.at(handle),
                                   discarded ? RenderGraphInitialContents::Discarded : RenderGraphInitialContents::Preserved});
                return id;
            }

            /** @brief Declares each real workload resource use before synchronization compilation. */
            void Use(RenderGraphPassRef pass, RenderGraphResourceId resource, RenderGraphUsageKind kind, RenderGraphAccess access,
                     RenderGraphPipelineScope scope = RenderGraphPipelineScope::Graphics) {
                Accepted(builder.AddUsage({pass, resource, access, kind}));
                auto state = State(kind, scope);
                state.access = static_cast<RenderGraphSynchronizationAccess>(static_cast<unsigned>(access) + 1);
                last[resource] = state;
            }

            /** @brief Names only the concrete resource operations used by this strictly ordered test graph. */
            RenderGraphLogicalState State(RenderGraphUsageKind kind, RenderGraphPipelineScope scope) const {
                using enum RenderGraphSynchronizationOperation;
                RenderGraphLogicalState state{.scope = scope, .queue = queue};
                switch (kind) {
                    case RenderGraphUsageKind::Sampled:
                        state.operation = Sampled;
                        state.layout = RenderGraphTextureLayout::ShaderReadOnly;
                        break;
                    case RenderGraphUsageKind::Storage:
                        state.operation = Storage;
                        break;
                    case RenderGraphUsageKind::ColorAttachment:
                        state.operation = ColorAttachment;
                        state.layout = RenderGraphTextureLayout::ColorAttachment;
                        break;
                    default:
                        state.operation = GraphicsInputBuffer;
                        break;
                }
                return state;
            }

            /** @brief Publishes tracked final states only after the real submitted graph completed; cancellation never calls this. */
            void Commit() {
                for (const auto &[handle, id] : buffers)
                    bufferStates[handle] = last.at(id);
                for (const auto &[handle, id] : textures)
                    textureStates[handle] = last.at(id);
            }

            /** @brief Retains explicit stage order independently of inferred resource hazards. */
            void After(RenderGraphPassRef before, RenderGraphPassRef after) {
                Accepted(builder.AddDependency({before, after, RenderGraphDependencyKind::ExecutionOrder}));
            }
        };

        /** @brief Keeps the exact material uniform resource alongside its immutable backend generation. */
        struct Material final {
            RenderBufferHandle uniform;
            std::shared_ptr<const ResidentMaterialBinding> binding;
        };

        /** @brief Owns the arrays borrowed synchronously by one material-raster workload. */
        struct Raster final {
            std::array<RenderGraphMaterialDraw, 1> draws;
            std::vector<RenderGraphMaterialRead> reads;
        };

        /** @brief Copies only actual completed output, never computing a replacement exposure or color transform. */
        struct Observation final {
            ExposureGpuState exposure;
            std::array<float, 4> displayLinear{};
            std::array<std::uint8_t, 4> bgra{};
        };

        /** @brief Public-frontend producer/consumer qualification; private native access is restricted to cold fences and observation. */
        class Fixture final {
        public:
            Editor::Offline::ColorOutputCookPackage package;
            RenderBackendRegistry registry;
            MetalEditorGraphicsBridge bridge;
            SDL_Window *window{};
            std::unique_ptr<Editor::SdlMetalPresentationPort> presentation;
            std::unique_ptr<RenderFrontend> frontend;
            std::unique_ptr<MaterialBindingTable> materials;
            std::vector<RenderQueueAssignment> queues;
            std::map<RenderBufferHandle, RenderGraphLogicalState> bufferStates;
            std::map<RenderTextureHandle, RenderGraphLogicalState> textureStates;
            std::shared_ptr<IResidentExposureGpuKernel> kernel;
            std::array<Program, 2> programs;
            Program lookProgram;
            std::array<Texture, 2> tables;
            Texture lookTexture;
            std::shared_ptr<const CookedColorTransform> look;
            ColorLookMode lookMode{ColorLookMode::Disabled};
            std::array<RenderSamplerHandle, 2> samplers;
            Texture scene, display, output;
            RenderBufferHandle vertices, indices, histogram, seed;
            RenderMeshHandle mesh;
            RenderViewId view;
            FramebufferExtent extent{InitialExtent};
            RenderTextureFormat displayFormat{RenderTextureFormat::Rgba16Float};
            ExposureState previous;
            std::uint64_t frameNumber{}, generation{}, materialRevision{}, acceptedOrdinal{};
            __strong id<MTLCommandBuffer> copiedCommands;

            /** @brief Loads actual cross-bound offline generations before constructing any renderer ownership. */
            explicit Fixture(RenderTextureFormat precision = RenderTextureFormat::Rgba16Float)
                : package(Take(Editor::Offline::LoadColorOutputCookPackage(PackageRoot("HORO_COLOR_OUTPUT_PACKAGE")))),
                  displayFormat(precision) {
                try {
                    REQUIRE((displayFormat == RenderTextureFormat::Rgba16Float || displayFormat == RenderTextureFormat::Rgba32Float));
                    Initialize();
                    kernel = Take(frontend->RealizeExposureGpuKernel(ExposureFamily()));
                    PrepareGeometry();
                    materials = Take(MaterialBindingTable::Create(vertices.owner, *frontend));
                    PreparePrograms();
                    PrepareTables();
                    Resize(InitialExtent);
                } catch (...) {
                    // This body-level catch runs before member destruction, unlike a constructor function-try handler.
                    if (!Close())
                        std::terminate();
                    throw;
                }
            }

            /** @brief Releases material pins before draining the actual frontend/backend and borrowed native parents. */
            ~Fixture() {
                if (!Close())
                    std::terminate();
            }

            /** @brief Performs the same bounded real shutdown for complete or partially constructed native ownership. */
            bool Close() noexcept {
                try {
                    if (!Terminal(copiedCommands))
                        return false;
                    if (materials && materials->Shutdown().HasError())
                        return false;
                    materials.reset();
                    kernel.reset();
                    if (frontend && !Drain(*frontend))
                        return false;
                    frontend.reset();
                    presentation.reset();
                    if (window)
                        SDL_DestroyWindow(window);
                    window = nullptr;
                    SDL_Quit();
                    return true;
                } catch (...) {
                    return false;
                }
            }

            /** @brief Uses an isolated unfocused SDL surface and the actual registered production Metal backend. */
            void Initialize() {
                REQUIRE(SDL_Init(SDL_INIT_VIDEO));
                REQUIRE(SDL_SetHint(SDL_HINT_VIDEO_METAL_AUTO_RESIZE_DRAWABLE, "0"));
                REQUIRE(SDL_SetHint(SDL_HINT_WINDOW_ACTIVATE_WHEN_SHOWN, "0"));
                window = SDL_CreateWindow("Horo public color/exposure qualification", 64, 64, SDL_WINDOW_METAL | SDL_WINDOW_HIDDEN);
                REQUIRE(window);
                REQUIRE(SDL_SetWindowFocusable(window, false));
                presentation = std::make_unique<Editor::SdlMetalPresentationPort>(*window);
                Accepted(RegisterMetalRenderBackend(registry, *presentation, bridge));
                Accepted(registry.Seal());
                frontend = Take(RenderFrontend::Create(registry, RenderBackendId{"metal"},
                                                       {.requirePresentation = true,
                                                        .enableValidation = true,
                                                        .maxFramesInFlight = 2,
                                                        .presentMode = PresentMode::Immediate}));
                REQUIRE(SDL_ShowWindow(window));
                const auto topology = Take(Detail::RenderFrontendResourceAccess::QueueAssignments(*frontend));
                queues.assign(topology.begin(), topology.end());
                view = Take(frontend->CreateRenderViewIdentity());
            }

            /** @brief Requires frontend readiness after actual bounded cold upload processing. */
            template <class Handle> void Ready(Handle handle) {
                Take(frontend->ProcessResourceRequests());
                REQUIRE(Take(frontend->ResourceState(handle)) == RenderResourceState::Ready);
            }

            /** @brief Borrows the actual frontend-issued graphics queue rather than guessing a queue ordinal. */
            RenderQueueId GraphicsQueue() const {
                const auto found = std::ranges::find(queues, RenderQueueRole::Graphics, &RenderQueueAssignment::role);
                REQUIRE(found != queues.end());
                return found->queue;
            }

            /** @brief Records a real completed staging/observation operation on the selected same native queue. */
            RenderGraphLogicalState Transfer(bool write, bool texture) const {
                return {write ? RenderGraphSynchronizationAccess::Write : RenderGraphSynchronizationAccess::Read,
                        write ? RenderGraphSynchronizationOperation::CopyDestination : RenderGraphSynchronizationOperation::CopySource,
                        RenderGraphPipelineScope::Transfer,
                        texture ? (write ? RenderGraphTextureLayout::CopyDestination : RenderGraphTextureLayout::CopySource)
                                : RenderGraphTextureLayout::NotApplicable,
                        GraphicsQueue()};
            }

            /** @brief Allocates initialized real device-local storage, so graph Preserved means actual existing bytes. */
            RenderBufferHandle Buffer(std::size_t size, RenderBufferUsage usage, std::span<const std::byte> data = {}) {
                std::vector<std::byte> zeros;
                if (data.empty()) {
                    zeros.resize(size);
                    data = zeros;
                }
                const auto handle =
                    Take(frontend->CreateBuffer({size, usage | RenderBufferUsage::CopyDestination, RenderBufferAccess::DeviceLocal}, data))
                        .handle;
                Ready(handle);
                bufferStates[handle] = Transfer(true, false);
                return handle;
            }

            /** @brief Creates one exact frontend texture and view, optionally copying a genuine cooked auxiliary payload. */
            Texture Image(FramebufferExtent size, RenderTextureFormat format, RenderTextureUsage usage,
                          std::span<const std::byte> data = {}) {
                RenderTextureDescriptor descriptor{.extent = size, .format = format, .usage = usage};
                if (!data.empty()) {
                    const auto bytes = RenderTextureBaseLevelByteSize(descriptor);
                    REQUIRE(bytes.has_value());
                    REQUIRE(data.size() == *bytes);
                }
                Texture texture;
                texture.handle = Take(frontend->CreateTexture(descriptor, data)).handle;
                Ready(texture.handle);
                texture.view = Take(frontend->CreateTextureView({.texture = texture.handle, .format = format})).handle;
                Ready(texture.view);
                textureStates[texture.handle] = data.empty() ? RenderGraphLogicalState{} : Transfer(true, true);
                return texture;
            }

            /** @brief Submits a real same-queue cold fence after uploads, avoiding fabricated ready/completion evidence. */
            void FinishUploads() {
                id<MTLCommandQueue> queue = (__bridge id<MTLCommandQueue>)bridge.CommandQueue();
                id<MTLCommandBuffer> fence = [queue commandBuffer];
                REQUIRE(fence);
                [fence commit];
                REQUIRE(Terminal(fence));
            }

            /** @brief Resets only newly allocated scene/history resources while preserving the monotonic view and generation issuer. */
            void Resize(FramebufferExtent size) {
                RetireImages();
                RetireHistory();
                extent = size;
                const auto colorUsage = RenderTextureUsage::Sampled | RenderTextureUsage::RenderAttachment;
                scene = Image(size, RenderTextureFormat::Rgba16Float, colorUsage);
                display = Image(size, displayFormat, colorUsage | RenderTextureUsage::CopySource);
                output =
                    Image(size, RenderTextureFormat::Bgra8Unorm, RenderTextureUsage::RenderAttachment | RenderTextureUsage::CopySource);
                histogram =
                    Buffer(ExposureHistogramBins * sizeof(std::uint32_t), RenderBufferUsage::Storage | RenderBufferUsage::CopySource);
                seed = Buffer(sizeof(ExposureGpuState), RenderBufferUsage::Storage | RenderBufferUsage::CopyDestination);
                previous = {};
                FinishUploads();
            }

            /** @brief Retires exact old views before their texture parents after all fixture submissions completed. */
            void RetireImages() {
                for (const auto image : std::array{scene, display, output}) {
                    if (image.view.IsValid())
                        Accepted(frontend->ReleaseTextureView(image.view));
                    if (image.handle.IsValid())
                        Accepted(frontend->ReleaseTexture(image.handle));
                }
            }

            /** @brief Retires only this completed view's old history generations; new reset storage never aliases them. */
            void RetireHistory() {
                for (const auto buffer : std::array{histogram, seed, previous.buffer})
                    if (buffer.IsValid())
                        Accepted(frontend->ReleaseBuffer(buffer));
            }

            /** @brief Provides genuine indexed mesh handles even though the product vertex shader selects the fullscreen triangle by vertex
             * ID. */
            void PrepareGeometry() {
                const std::array<MeshVertex, 3> points{};
                const std::array<std::uint32_t, 3> order{0, 1, 2};
                vertices = Buffer(sizeof(points), RenderBufferUsage::Vertex, std::as_bytes(std::span{points}));
                indices = Buffer(sizeof(order), RenderBufferUsage::Index, std::as_bytes(std::span{order}));
                mesh = Take(frontend->CreateMesh({.vertexBuffer = vertices,
                                                  .indexBuffer = indices,
                                                  .vertexStride = sizeof(MeshVertex),
                                                  .vertexCount = 3,
                                                  .indexCount = 3,
                                                  .localBounds = {{-1, -1, 0}, {1, 1, 0}}}))
                           .handle;
                Ready(mesh);
            }

            /** @brief Realizes a genuine cooked module through the public frontend; no native artifact identities are invented. */
            RenderShaderModuleHandle Module(const CompiledShaderArtifact &artifact, ShaderStage stage, const char *entry) {
                REQUIRE(artifact.reflection);
                const auto &reflection = *artifact.reflection;
                return Take(frontend->CreateShaderModule({Editor::Offline::ColorOutputMetalCookTarget(), stage, entry,
                                                          reflection.cookedArtifactKey, reflection.reflection.interfaceCompatibility},
                                                         artifact.payload))
                    .handle;
            }

            /** @brief Prepares both real product graphics pipelines using the selected device's actual cache identity. */
            void PreparePrograms() {
                for (std::size_t i = 0; i < programs.size(); ++i)
                    PrepareProgram(programs[i], package.programs[i], static_cast<ColorOutputShaderStage>(i));
            }

            /** @brief Realizes one authentic product variant with the selected native device and exact final-artifact reflection. */
            void PrepareProgram(Program &program, const CompiledShaderArtifact &artifact, ColorOutputShaderStage stage) {
                const auto target = Editor::Offline::ColorOutputMetalCookTarget();
                const auto device = Take(frontend->QueryPipelineDeviceIdentity());
                const auto manifest = Take(MakeColorOutputShaderManifest(stage, target));
                REQUIRE(artifact.reflection);
                const auto &metadata = *artifact.reflection;
                program.vertex = Module(artifact, ShaderStage::Vertex, "VSMain");
                program.fragment = Module(artifact, ShaderStage::Fragment, "PSMain");
                RenderGraphicsPipelineState state{.cull = RenderCullMode::None,
                                                  .depthTest = false,
                                                  .depthWrite = false,
                                                  .colorFormat = stage == ColorOutputShaderStage::EncodeSdr
                                                                     ? RenderTextureFormat::Bgra8Unorm
                                                                     : displayFormat,
                                                  .depthFormat = std::nullopt};
                const PipelineCacheCompatibility compatibility{1,
                                                               device.backend,
                                                               device.backendModuleVersion,
                                                               device.adapter,
                                                               device.deviceCompatibilityDigest,
                                                               device.driverIdentity,
                                                               device.driverVersion,
                                                               target.backend,
                                                               target.payloadFormat,
                                                               metadata.cookedArtifactKey,
                                                               metadata.reflection.interfaceCompatibility,
                                                               Take(ComputeRenderGraphicsPipelineStateDigest(state))};
                program.pipeline = Take(frontend->CreateGraphicsPipeline({program.vertex, program.fragment, manifest, target,
                                                                          metadata.reflection, state, compatibility}))
                                       .handle;
                program.layout = Take(PrepareMaterialBindingLayout(metadata.reflection));
            }

            /** @brief Uploads a real immutable nonidentity creative atlas and selects the genuinely cooked look shader variant. */
            void ConfigureLook(ColorLutInterpolation interpolation) {
                const auto candidate = MakeLook(*package.analytic.program, interpolation);
                const auto *artifact = package.LookProgram();
                REQUIRE(artifact);
                if (!lookProgram.pipeline.IsValid())
                    PrepareProgram(lookProgram, *artifact, ColorOutputShaderStage::SceneToDisplayLinearWithLook);
                const auto descriptor = candidate->AtlasDescriptor();
                const auto atlas = Image(descriptor.extent, descriptor.format, descriptor.usage, candidate->AtlasUpload());
                FinishUploads();
                look = candidate;
                lookTexture = atlas;
                lookMode = ColorLookMode::Enabled;
            }

            /** @brief Obtains actual accepted scale-one GPU history before a signed/domain case without supplying a CPU EV. */
            void BootstrapLookExposure() {
                const auto selected = lookMode;
                lookMode = ColorLookMode::Disabled;
                const auto state = Step(16);
                lookMode = selected;
                const auto observed = ReadExposure(state);
                REQUIRE(observed.reason == static_cast<std::uint32_t>(ExposureSampleReason::Valid));
                REQUIRE(observed.exposureEv == Catch::Approx(0));
                REQUIRE(observed.exposureScale == Catch::Approx(1));
            }

            /** @brief Resolves only actual prepared product variants; no missing shader is replaced by a fabricated program. */
            const Program &ProgramAt(std::size_t index) const {
                return index == 2 ? lookProgram : programs.at(index);
            }

            /** @brief Uploads only the immutable original program's exact lossless auxiliary layouts and declared samplers. */
            void PrepareTables() {
                const auto &analytic = *package.analytic.program;
                for (std::size_t i = 0; i < tables.size(); ++i) {
                    const auto *descriptor = analytic.Table(i);
                    REQUIRE(descriptor);
                    tables[i] = Image(descriptor->extent, descriptor->format,
                                      RenderTextureUsage::Sampled | RenderTextureUsage::CopyDestination, analytic.TableUpload(i));
                    samplers[i] = Take(frontend->CreateSampler(descriptor->sampler)).handle;
                }
            }

            /** @brief Packs complete reflected uniforms into an explicit frontend buffer instead of an implicit material allocation. */
            Material Bind(std::size_t programIndex, std::span<const MaterialParameterValue> values,
                          std::vector<MaterialResourceBinding> resources) {
                const auto &program = ProgramAt(programIndex);
                auto packed = Take(PackMaterialParameterBinding(program.layout, {1}, values));
                MaterialBindingDescriptor descriptor{.material = {++materialRevision},
                                                     .revision = 1,
                                                     .pipeline = program.pipeline,
                                                     .layout = program.layout};
                const auto bytes = Take(Detail::RenderFrontendResourceAccess::MaterialParameterBufferBytes(*frontend, descriptor, {1}));
                REQUIRE(bytes >= packed.bytes.size());
                packed.bytes.resize(bytes);
                Material material;
                material.uniform = Buffer(bytes, RenderBufferUsage::Uniform, packed.bytes);
                resources.push_back({{1}, 0, MaterialBufferBinding{material.uniform, 0, bytes}});
                descriptor.resources = std::move(resources);
                const auto selection = Take(materials->Publish(std::move(descriptor)));
                material.binding = Take(materials->Acquire(selection.binding));
                return material;
            }

            /** @brief Selects GPU-written exposure by exact packed generation/submission bits, never a CPU-calculated EV. */
            Material SceneMaterial(const ExposureState &state, bool staleIdentity) {
                const std::array<float, 4> size{static_cast<float>(extent.width), static_cast<float>(extent.height), 0, 0};
                const std::array<float, 4> selection{1, 0, 0, 0};
                const auto gen = state.generation + static_cast<std::uint64_t>(staleIdentity);
                const std::array<float, 4> identity{std::bit_cast<float>(static_cast<std::uint32_t>(gen)),
                                                    std::bit_cast<float>(static_cast<std::uint32_t>(gen >> 32U)),
                                                    std::bit_cast<float>(static_cast<std::uint32_t>(state.sourceSubmission)),
                                                    std::bit_cast<float>(static_cast<std::uint32_t>(state.sourceSubmission >> 32U))};
                std::vector values{MaterialParameterValue{{1}, std::span<const float>{size}},
                                   MaterialParameterValue{{2}, std::span<const float>{selection}},
                                   MaterialParameterValue{{3}, std::span<const float>{identity}}};
                std::vector<MaterialResourceBinding> resources{{{2}, 0, scene.view},
                                                               {{3}, 0, tables[0].view},
                                                               {{4}, 0, tables[1].view},
                                                               {{5}, 0, samplers[0]},
                                                               {{6}, 0, samplers[1]},
                                                               {{7}, 0, MaterialBufferBinding{state.buffer, 0, sizeof(ExposureGpuState)}}};
                std::array<float, 4> minimum{}, maximum{}, control{};
                if (lookMode == ColorLookMode::Enabled) {
                    REQUIRE(look);
                    const auto &descriptor = look->Descriptor();
                    minimum = {descriptor.domainMinimum.x, descriptor.domainMinimum.y, descriptor.domainMinimum.z, 0};
                    maximum = {descriptor.domainMaximum.x, descriptor.domainMaximum.y, descriptor.domainMaximum.z, 0};
                    control = {static_cast<float>(descriptor.dimension), static_cast<float>(descriptor.interpolation),
                               static_cast<float>(descriptor.domainPolicy), 0};
                    values.push_back({{4}, std::span<const float>{minimum}});
                    values.push_back({{5}, std::span<const float>{maximum}});
                    values.push_back({{6}, std::span<const float>{control}});
                    resources.push_back({{8}, 0, lookTexture.view});
                }
                return Bind(lookMode == ColorLookMode::Enabled ? 2 : 0, values, std::move(resources));
            }

            /** @brief Checks real material admission for missing atlas or an actual foreign-owner view, preserving the valid generation. */
            void RejectLookBinding(std::optional<RenderTextureViewHandle> foreign = std::nullopt) {
                const auto valid = SceneMaterial(previous, false);
                auto descriptor = valid.binding->Descriptor();
                descriptor.material = {++materialRevision};
                const auto atlas = std::ranges::find_if(descriptor.resources, [](const auto &resource) {
                    return resource.binding.value == 8;
                });
                REQUIRE(atlas != descriptor.resources.end());
                if (foreign)
                    atlas->value = *foreign;
                else
                    descriptor.resources.erase(atlas);
                const auto before = materials->Snapshot();
                const auto rejected = materials->Publish(std::move(descriptor));
                INFO((rejected.HasError() ? ErrorTrace(rejected.ErrorValue()) : "Unexpected invalid creative material admission."));
                REQUIRE(rejected.HasError());
                CHECK(rejected.ErrorValue().domain.Value() == MaterialBindingErrors::InvalidDescriptor.domain.Value());
                CHECK(rejected.ErrorValue().code.Value() == MaterialBindingErrors::InvalidDescriptor.code.Value());
                CHECK(materials->Snapshot().retainedGenerations == before.retainedGenerations);
                CHECK(Take(materials->Acquire(valid.binding->Id())) == valid.binding);
                Accepted(materials->Release(valid.binding->Id()));
            }

            /** @brief Selects exact SDR quantization and optional bounded post-transfer Bayer dither. */
            Material EncodeMaterial(bool dither) {
                const std::array<float, 4> control{static_cast<float>(extent.width), static_cast<float>(extent.height),
                                                   dither ? 1.0F : 0.0F, 255};
                const std::array values{MaterialParameterValue{{1}, std::span<const float>{control}}};
                return Bind(1, values, {{{2}, 0, display.view}});
            }

            /** @brief Produces a fresh actual output generation with host metadata distinct from native completion authority. */
            ExposureState Next() {
                const auto ordinal = acceptedOrdinal + 1;
                return {.mode = ExposureMode::AutomaticHistogram,
                        .view = view,
                        .generation = ++generation,
                        .settingsGeneration = 1,
                        .deviceGeneration = frontend->Capabilities().support.deviceIncarnation,
                        .sourceSubmission = ordinal,
                        .consumeSubmission = ordinal + 1,
                        .buffer = Buffer(sizeof(ExposureGpuState), RenderBufferUsage::Storage | RenderBufferUsage::CopySource)};
            }

            /** @brief Uses the canonical dispatch preparation with a clamped oracle that gives exact GPU scale two then one. */
            RenderGraphExposureGpu Meter(Graph &graph, ExposureState produced) {
                ExposureSettings settings;
                settings.mode = ExposureMode::AutomaticHistogram;
                settings.minimumEv = 0;
                settings.maximumEv = 1;
                settings.brightenEvPerSecond = 10;
                settings.darkenEvPerSecond = 10;
                const auto pass = Take(graph.builder.AddPass(RenderPassKind::Compute, RenderQueueRole::Graphics));
                RenderGraphExposureGpu work{graph.Image(scene.handle),
                                            graph.Buffer(histogram),
                                            graph.Buffer(previous.buffer.IsValid() ? previous.buffer : seed),
                                            graph.Buffer(produced.buffer),
                                            extent,
                                            {Take(PrepareExposureDispatch(settings, extent, {0, 0, extent.width, extent.height}, 1)),
                                             {static_cast<std::uint32_t>(produced.generation),
                                              static_cast<std::uint32_t>(produced.generation >> 32U),
                                              static_cast<std::uint32_t>(produced.sourceSubmission),
                                              static_cast<std::uint32_t>(produced.sourceSubmission >> 32U)}},
                                            previous,
                                            produced,
                                            !previous.buffer.IsValid(),
                                            kernel};
                graph.Use(pass, work.sceneColor, RenderGraphUsageKind::Sampled, RenderGraphAccess::Read, RenderGraphPipelineScope::Compute);
                graph.Use(pass, work.histogram, RenderGraphUsageKind::Storage, RenderGraphAccess::ReadWrite,
                          RenderGraphPipelineScope::Compute);
                graph.Use(pass, work.previousExposure, RenderGraphUsageKind::Storage,
                          work.resetPrevious ? RenderGraphAccess::ReadWrite : RenderGraphAccess::Read, RenderGraphPipelineScope::Compute);
                graph.Use(pass, work.outputExposure, RenderGraphUsageKind::Storage, RenderGraphAccess::Write,
                          RenderGraphPipelineScope::Compute);
                graph.workloads.push_back({pass, work});
                return work;
            }

            /** @brief Declares the exact geometry and every immutable material resource read against real graph imports. */
            void RasterInputs(Graph &graph, RenderGraphPassRef pass, Raster &raster, const Material &material) {
                const auto vertex = graph.Buffer(vertices), index = graph.Buffer(indices);
                graph.Use(pass, vertex, RenderGraphUsageKind::VertexBuffer, RenderGraphAccess::Read);
                graph.Use(pass, index, RenderGraphUsageKind::IndexBuffer, RenderGraphAccess::Read);
                raster.draws[0] = {mesh, vertex, index, material.binding};
                for (const auto &resource : material.binding->Descriptor().resources)
                    RasterRead(graph, pass, raster, resource);
            }

            /** @brief Resolves exactly the buffer or sampled texture carried by one real material binding. */
            void RasterRead(Graph &graph, RenderGraphPassRef pass, Raster &raster, const MaterialResourceBinding &resource) {
                if (const auto *buffer = std::get_if<MaterialBufferBinding>(&resource.value)) {
                    const auto id = graph.Buffer(buffer->buffer);
                    graph.Use(pass, id,
                              buffer->buffer == previous.buffer ? RenderGraphUsageKind::Storage : RenderGraphUsageKind::UniformBuffer,
                              RenderGraphAccess::Read);
                    raster.reads.push_back({id, buffer->buffer, {}});
                }
                if (const auto *texture = std::get_if<RenderTextureViewHandle>(&resource.value)) {
                    const auto id = graph.Image(Parent(*texture));
                    graph.Use(pass, id, RenderGraphUsageKind::Sampled, RenderGraphAccess::Read);
                    raster.reads.push_back({id, {}, *texture});
                }
            }

            /** @brief Maps only retained frontend-issued views owned by this fixture, never inferring native or forged parents. */
            RenderTextureHandle Parent(RenderTextureViewHandle texture) const {
                const std::array known{scene, display, output, tables[0], tables[1], lookTexture};
                const auto found = std::ranges::find(known, texture, &Texture::view);
                REQUIRE(found != known.end());
                return found->handle;
            }

            /** @brief Adds one real product raster with retained inputs and exact attachment ordering. */
            RenderGraphPassRef AddRaster(Graph &graph, Raster &raster, const Material &material, Texture target) {
                const auto pass = Take(graph.builder.AddPass(RenderPassKind::Graphics, RenderQueueRole::Graphics));
                const auto color = graph.Image(target.handle, true);
                graph.Use(pass, color, RenderGraphUsageKind::ColorAttachment, RenderGraphAccess::Write);
                RasterInputs(graph, pass, raster, material);
                graph.workloads.push_back(
                    {pass, RenderGraphMaterialRaster{.color = color,
                                                     .colorView = target.view,
                                                     .extent = extent,
                                                     .colorOperations = {.loadOperation = AttachmentLoadOperation::Clear,
                                                                         .clearColor = {0, 1, 0, 1}},
                                                     .depthClear = std::nullopt,
                                                     .draws = raster.draws,
                                                     .reads = raster.reads}});
                return pass;
            }

            /** @brief Acquires a real frame with finite presentation polling and no invented backend frame token. */
            RenderFrameScope Begin() {
                const auto deadline = std::chrono::steady_clock::now() + 5s;
                const auto number = ++frameNumber;
                while (std::chrono::steady_clock::now() < deadline) {
                    SDL_PumpEvents();
                    const auto available = frontend->PollPresentationAvailability({64, 64});
                    if (!available.HasValue() || !available.Value()) {
                        std::this_thread::sleep_for(1ms);
                        continue;
                    }
                    auto frame = frontend->BeginFrame({.frameNumber = number, .outputExtent = {64, 64}});
                    if (frame.HasValue())
                        return std::move(frame).Value();
                    REQUIRE(frame.ErrorValue().code.Value() == RenderPresentationErrors::AcquisitionPending.code.Value());
                }
                FAIL("Actual public Metal frame acquisition exceeded five seconds.");
                std::terminate();
            }

            /** @brief Compiles and executes using only actual queue assignments and frontend-reserved submission points. */
            RenderTimelinePoint Execute(Graph &graph, RenderFrameScope &frame) {
                const auto finalized = Take(graph.builder.Finalize());
                const auto schedule = Take(CompileRenderGraph(finalized));
                const auto sync = Take(SynthesizeRenderGraphSynchronization(finalized, schedule, queues, graph.initial));
                const auto compiled = Take(CompileRenderGraphExecution(finalized, schedule, sync, queues));
                const auto issued = Take(frame.ReserveSubmission(RenderQueueRole::Graphics));
                const std::array submissions{RenderGraphQueueSubmission{issued, {0, compiled.Passes().size()}}};
                Accepted(frame.ExecuteSubmittedGraph(compiled, graph.workloads, submissions));
                return issued.signal;
            }

            /** @brief Exercises actual public graph admission using wrong bounds or an unrelated real output buffer. */
            void RejectMeter(bool bounds) {
                const auto produced = Next();
                FinishUploads();
                Graph graph{GraphicsQueue(), bufferStates, textureStates};
                const auto clear = Take(graph.builder.AddPass(RenderPassKind::Graphics, RenderQueueRole::Graphics));
                const auto source = graph.Image(scene.handle, true);
                graph.Use(clear, source, RenderGraphUsageKind::ColorAttachment, RenderGraphAccess::Write);
                graph.workloads.push_back({clear, RenderGraphColorAttachment{source, {.clearColor = {0.5F, 0.5F, 0.5F, 1}}}});
                Meter(graph, produced);
                auto &work = std::get<RenderGraphExposureGpu>(graph.workloads.back().workload);
                if (bounds)
                    ++work.dispatch.exposure.region[2];
                else
                    work.produced.buffer = seed;
                graph.After(clear, graph.workloads.back().pass);
                RejectGraph(graph, bounds ? ExposureErrors::InvalidDispatch : ExposureGpuErrors::InvalidInput);
            }

            /** @brief Preserves the typed rejection and leaves prior resource states unpublished after actual frame cancellation. */
            void RejectGraph(Graph &graph, const ErrorCodeDescriptor &expected) {
                const auto finalized = Take(graph.builder.Finalize());
                const auto schedule = Take(CompileRenderGraph(finalized));
                const auto sync = Take(SynthesizeRenderGraphSynchronization(finalized, schedule, queues, graph.initial));
                const auto compiled = Take(CompileRenderGraphExecution(finalized, schedule, sync, queues));
                auto frame = Begin();
                const auto issued = Take(frame.ReserveSubmission(RenderQueueRole::Graphics));
                const std::array submissions{RenderGraphQueueSubmission{issued, {0, compiled.Passes().size()}}};
                const auto result = frame.ExecuteSubmittedGraph(compiled, graph.workloads, submissions);
                INFO((result.HasError() ? ErrorTrace(result.ErrorValue()) : "Unexpected successful malformed graph admission."));
                REQUIRE(result.HasError());
                CHECK(result.ErrorValue().domain.Value() == MetalBackendErrors::InvalidExecutionPlan.domain.Value());
                CHECK(result.ErrorValue().code.Value() == MetalBackendErrors::InvalidExecutionPlan.code.Value());
                CHECK(HasCause(result.ErrorValue(), expected));
                frame.Cancel();
            }

            /** @brief Polls the actual reserved signal after Present; CPU metadata never substitutes for acceptance or completion. */
            void Observe(RenderTimelinePoint signal) {
                const auto deadline = std::chrono::steady_clock::now() + 5s;
                while (std::chrono::steady_clock::now() < deadline) {
                    if (Take(frontend->ObserveSubmission(
                            {signal, RenderCpuWaitPurpose::Poll, 0ns, frontend->Capabilities().support.deviceIncarnation})))
                        return;
                    std::this_thread::sleep_for(1ms);
                }
                FAIL("Actual frontend-issued GPU completion exceeded five seconds.");
            }

            /** @brief Removes completed/abandoned material generations from discovery while synchronous graph borrows finish unwinding. */
            void ReleaseMaterials(const std::optional<Material> &color, const std::optional<Material> &encode) {
                if (color)
                    Accepted(materials->Release(color->binding->Id()));
                if (encode)
                    Accepted(materials->Release(encode->binding->Id()));
            }

            /** @brief Runs GPU scene clear and metering, then consumes only the prior accepted state through both product rasters. */
            ExposureState Step(Math::Vec3 sourceColor, bool staleIdentity = false, bool cancel = false, bool dither = false) {
                const auto produced = Next();
                std::optional<Material> color, encode;
                if (previous.buffer.IsValid()) {
                    color = SceneMaterial(previous, staleIdentity);
                    encode = EncodeMaterial(dither);
                }
                FinishUploads();
                Graph graph{GraphicsQueue(), bufferStates, textureStates};
                const auto clear = Take(graph.builder.AddPass(RenderPassKind::Graphics, RenderQueueRole::Graphics));
                const auto source = graph.Image(scene.handle, true);
                graph.Use(clear, source, RenderGraphUsageKind::ColorAttachment, RenderGraphAccess::Write);
                graph.workloads.push_back(
                    {clear, RenderGraphColorAttachment{source, {.clearColor = {sourceColor.x, sourceColor.y, sourceColor.z, 1}}}});
                Meter(graph, produced);
                const auto meter = graph.workloads.back().pass;
                graph.After(clear, meter);
                std::array<Raster, 2> rasters;
                if (color) {
                    const auto linear = AddRaster(graph, rasters[0], *color, display);
                    const auto encoded = AddRaster(graph, rasters[1], *encode, output);
                    graph.After(meter, linear);
                    graph.After(linear, encoded);
                }
                auto frame = Begin();
                const auto signal = Execute(graph, frame);
                if (cancel) {
                    frame.Cancel();
                    ReleaseMaterials(color, encode);
                    return produced;
                }
                Accepted(frame.Present());
                Observe(signal);
                graph.Commit();
                previous = produced;
                acceptedOrdinal = produced.sourceSubmission;
                ReleaseMaterials(color, encode);
                return produced;
            }

            /** @brief Keeps existing neutral cases on the identical real GPU scene-write and submission route. */
            ExposureState Step(float gray, bool staleIdentity = false, bool cancel = false, bool dither = false) {
                return Step(Math::Vec3{gray, gray, gray}, staleIdentity, cancel, dither);
            }

            /** @brief Reads the independent pinned CPU corpus retained by the real package, not a production CPU color evaluator. */
            std::array<float, 3> Reference(std::string_view name, Math::Vec3 expectedInput) const {
                const auto &bytes = package.analytic.files[2];
                REQUIRE(bytes.size() < 64U * 1024U);
                const auto document = nlohmann::json::parse(bytes);
                REQUIRE(document.at("cases").size() < 128);
                for (const auto &entry : document.at("cases")) {
                    if (entry.at("name") != name)
                        continue;
                    const std::array<float, 3> expected{expectedInput.x, expectedInput.y, expectedInput.z};
                    const auto input = entry.at("input").get<std::array<float, 3>>();
                    REQUIRE(input == expected);
                    return entry.at("displayLinearRgb").get<std::array<float, 3>>();
                }
                FAIL("Pinned independent CPU reference case is absent.");
                return {};
            }

            /** @brief Selects the exact independent neutral corpus entry without changing source interpretation. */
            std::array<float, 3> Reference(std::string_view name, float expectedInput) const {
                return Reference(name, Math::Vec3{expectedInput, expectedInput, expectedInput});
            }

            /** @brief Uses private native lookup only to observe frontend-issued resources after their genuine submission completed. */
            Observation Read(ExposureState state) {
                auto *linear = reinterpret_cast<Detail::MetalTextureViewInstance *>(
                    static_cast<std::uintptr_t>(Take(Detail::RenderFrontendResourceAccess::BackendInstance(*frontend, display.view))));
                auto *encoded = reinterpret_cast<Detail::MetalTextureViewInstance *>(
                    static_cast<std::uintptr_t>(Take(Detail::RenderFrontendResourceAccess::BackendInstance(*frontend, output.view))));
                auto *exposure = reinterpret_cast<Detail::MetalBufferInstance *>(
                    static_cast<std::uintptr_t>(Take(Detail::RenderFrontendResourceAccess::BackendInstance(*frontend, state.buffer))));
                id<MTLDevice> device = (__bridge id<MTLDevice>)bridge.Device();
                id<MTLCommandQueue> queue = (__bridge id<MTLCommandQueue>)bridge.CommandQueue();
                id<MTLBuffer> bytes = [device newBufferWithLength:768 options:MTLResourceStorageModeShared];
                REQUIRE(bytes);
                copiedCommands = [queue commandBuffer];
                id<MTLBlitCommandEncoder> blit = [copiedCommands blitCommandEncoder];
                REQUIRE(blit);
                CopyPixel(blit, linear->texture, bytes, 0);
                CopyPixel(blit, encoded->texture, bytes, 256);
                [blit copyFromBuffer:exposure->buffer sourceOffset:0 toBuffer:bytes destinationOffset:512 size:sizeof(ExposureGpuState)];
                [blit endEncoding];
                [copiedCommands commit];
                REQUIRE(Terminal(copiedCommands));
                textureStates[display.handle] = Transfer(false, true);
                textureStates[output.handle] = Transfer(false, true);
                bufferStates[state.buffer] = Transfer(false, false);
                Observation result;
                DecodeDisplay(bytes.contents, result);
                std::memcpy(result.bgra.data(), static_cast<const std::byte *>(bytes.contents) + 256, 4);
                std::memcpy(&result.exposure, static_cast<const std::byte *>(bytes.contents) + 512, sizeof(result.exposure));
                return result;
            }

            /** @brief Decodes only the actual matched display-attachment format, preserving the same GPU shader and readback operation. */
            void DecodeDisplay(const void *bytes, Observation &result) const {
                if (displayFormat == RenderTextureFormat::Rgba32Float) {
                    std::memcpy(result.displayLinear.data(), bytes, sizeof(result.displayLinear));
                    return;
                }
                std::array<std::uint16_t, 4> half;
                std::memcpy(half.data(), bytes, sizeof(half));
                for (std::size_t i = 0; i < half.size(); ++i)
                    result.displayLinear[i] = static_cast<float>(std::bit_cast<_Float16>(half[i]));
            }

            /** @brief Copies one actual center pixel with native row alignment, leaving production resource state untouched. */
            void CopyPixel(id<MTLBlitCommandEncoder> blit, id<MTLTexture> source, id<MTLBuffer> destination, NSUInteger offset) const {
                [blit copyFromTexture:source
                                 sourceSlice:0
                                 sourceLevel:0
                                sourceOrigin:MTLOriginMake(extent.width / 2, extent.height / 2, 0)
                                  sourceSize:MTLSizeMake(1, 1, 1)
                                    toBuffer:destination
                           destinationOffset:offset
                      destinationBytesPerRow:256
                    destinationBytesPerImage:256];
            }

            /** @brief Copies only actual GPU exposure bytes, including bootstrap before any color attachment has been rendered. */
            ExposureGpuState ReadExposure(ExposureState state) {
                id<MTLDevice> device = (__bridge id<MTLDevice>)bridge.Device();
                id<MTLCommandQueue> queue = (__bridge id<MTLCommandQueue>)bridge.CommandQueue();
                auto *buffer = reinterpret_cast<Detail::MetalBufferInstance *>(
                    static_cast<std::uintptr_t>(Take(Detail::RenderFrontendResourceAccess::BackendInstance(*frontend, state.buffer))));
                id<MTLBuffer> bytes = [device newBufferWithLength:sizeof(ExposureGpuState) options:MTLResourceStorageModeShared];
                copiedCommands = [queue commandBuffer];
                id<MTLBlitCommandEncoder> blit = [copiedCommands blitCommandEncoder];
                REQUIRE(bytes);
                REQUIRE(blit);
                [blit copyFromBuffer:buffer->buffer sourceOffset:0 toBuffer:bytes destinationOffset:0 size:sizeof(ExposureGpuState)];
                [blit endEncoding];
                [copiedCommands commit];
                REQUIRE(Terminal(copiedCommands));
                bufferStates[state.buffer] = Transfer(false, false);
                ExposureGpuState result;
                std::memcpy(&result, bytes.contents, sizeof(result));
                return result;
            }

            /** @brief TEST-ONLY private fault injection: reads real GPU state, changes its reason on CPU and blits the altered bytes back.
             * This does not represent a public producer path or a native-computed initial invalid verdict. */
            void InvalidateGpuState() {
                auto read = ReadExposure(previous);
                read.reason = static_cast<std::uint32_t>(ExposureSampleReason::Invalid);
                id<MTLDevice> device = (__bridge id<MTLDevice>)bridge.Device();
                id<MTLCommandQueue> queue = (__bridge id<MTLCommandQueue>)bridge.CommandQueue();
                auto *buffer = reinterpret_cast<Detail::MetalBufferInstance *>(
                    static_cast<std::uintptr_t>(Take(Detail::RenderFrontendResourceAccess::BackendInstance(*frontend, previous.buffer))));
                id<MTLBuffer> staging = [device newBufferWithBytes:&read length:sizeof(read) options:MTLResourceStorageModeShared];
                copiedCommands = [queue commandBuffer];
                id<MTLBlitCommandEncoder> blit = [copiedCommands blitCommandEncoder];
                REQUIRE(staging);
                REQUIRE(blit);
                [blit copyFromBuffer:staging sourceOffset:0 toBuffer:buffer->buffer destinationOffset:0 size:sizeof(read)];
                [blit endEncoding];
                [copiedCommands commit];
                REQUIRE(Terminal(copiedCommands));
                bufferStates[previous.buffer] = Transfer(true, false);
            }
        };

        /** @brief Compares GPU display-linear output against actual pinned CPU data with per-value FP16 quantization bounds. */
        void CheckColor(const Observation &read, std::array<float, 3> reference, std::array<double, 3> storeAllowance) {
            for (std::size_t i = 0; i < reference.size(); ++i) {
                // Default gray references lie in [0.25,0.5), whose FP16 half-ULP is1/8192.
                // HDR F16 callers supply one ULP1/2048 for [0.5,1); F32 callers supply zero storage allowance.
                // The extra2e-5 budgets official analytic GPU/CPU float evaluation, separately from final UNORM quantization.
                CHECK(read.displayLinear[i] == Catch::Approx(reference[i]).epsilon(0).margin(storeAllowance[i] + 2e-5));
                const float display = std::clamp(reference[i], 0.0F, 1.0F);
                const float encoded = display <= 0.0031308F ? 12.92F * display : 1.055F * std::pow(display, 1.0F / 2.4F) - 0.055F;
                const auto channel = 2 - i;
                CHECK(static_cast<float>(read.bgra[channel]) / 255 == Catch::Approx(encoded).epsilon(0).margin(1.0 / 255));
            }
            CHECK(read.bgra[3] == 255);
        }

        /** @brief Preserves the exact existing scalar precision budgets for all original neutral cases. */
        void CheckColor(const Observation &read, std::array<float, 3> reference, double storeAllowance = 1.0 / 8192.0) {
            CheckColor(read, reference, std::array{storeAllowance, storeAllowance, storeAllowance});
        }

        /** @brief Names one representable FP16 spacing at each independent mixed reference without a global loose color bound. */
        std::array<double, 3> MixedStoreAllowance(std::array<float, 3> reference, RenderTextureFormat format) {
            std::array<double, 3> allowance{};
            if (format == RenderTextureFormat::Rgba32Float)
                return allowance;
            for (std::size_t i = 0; i < reference.size(); ++i) {
                const auto magnitude = std::abs(reference[i]);
                int exponent{};
                std::frexp(magnitude, &exponent);
                allowance[i] = magnitude < std::ldexp(1.0F, -14) ? std::ldexp(1.0, -24) : std::ldexp(1.0, exponent - 11);
            }
            return allowance;
        }

        /** @brief Requires explicit nonfinite intermediate and reserved final diagnostic, never a successful color fallback. */
        void CheckDiagnostic(const Observation &read) {
            CHECK(!std::isfinite(read.displayLinear[0]));
            const std::array<std::uint8_t, 4> diagnostic{255, 0, 255, 255};
            CHECK(read.bgra == diagnostic);
        }
    }  // namespace

    TEST_CASE("Metal public frontend meters the actual scene and product output consumes exactly prior GPU exposure",
              "[renderer][metal][color-output-frontend]") {
        Fixture fixture;
        const auto bootstrap = fixture.Step(1.0F / 128);
        CHECK(fixture.ReadExposure(bootstrap).exposureScale == Catch::Approx(2));
        const auto second = fixture.Step(0.5F);
        const auto firstRead = fixture.Read(second);
        CHECK(firstRead.exposure.exposureEv == Catch::Approx(0));
        CHECK(firstRead.exposure.exposureScale == Catch::Approx(1));
        CheckColor(firstRead, fixture.Reference("gray-8", 1));
        const auto third = fixture.Step(0.5F);
        CheckColor(fixture.Read(third), fixture.Reference("gray-7", 0.5F));
    }

    TEST_CASE("Metal product color rejects stale shader identity through explicit diagnostic output",
              "[renderer][metal][color-output-frontend]") {
        Fixture fixture;
        fixture.Step(1.0F / 128);
        CheckDiagnostic(fixture.Read(fixture.Step(0.5F, true)));
    }

    TEST_CASE("Metal TEST-ONLY private GPU fault injection checks invalid exposure propagation and reset recovery",
              "[renderer][metal][color-output-fault]") {
        Fixture fixture;
        fixture.Step(1.0F / 128);
        fixture.InvalidateGpuState();
        const auto invalid = fixture.Read(fixture.Step(0.5F));
        CheckDiagnostic(invalid);
        CHECK(invalid.exposure.reason == static_cast<std::uint32_t>(ExposureSampleReason::Invalid));
        fixture.Resize(InitialExtent);
        fixture.Step(1.0F / 128);
        CheckColor(fixture.Read(fixture.Step(0.5F)), fixture.Reference("gray-8", 1));
    }

    TEST_CASE("Metal cancelled public frame cannot replace accepted exposure and resize uses fresh history resources",
              "[renderer][metal][color-output-frontend]") {
        Fixture fixture;
        fixture.Step(1.0F / 128);
        const auto accepted = fixture.previous;
        const auto abandoned = fixture.Step(0.5F, false, true);
        CHECK(fixture.previous.buffer == accepted.buffer);
        CHECK(abandoned.buffer != accepted.buffer);
        CheckColor(fixture.Read(fixture.Step(0.5F)), fixture.Reference("gray-8", 1));
        const auto old = fixture.previous;
        const auto oldScene = fixture.scene;
        fixture.Resize({8, 8});
        CHECK(fixture.frontend->CreateTextureView({.texture = oldScene.handle, .format = RenderTextureFormat::Rgba16Float}).HasError());
        fixture.Step(1.0F / 128);
        CHECK(fixture.previous.buffer != old.buffer);
        CHECK(fixture.previous.view == old.view);
        CHECK(fixture.previous.generation > old.generation);
        CheckColor(fixture.Read(fixture.Step(0.5F)), fixture.Reference("gray-8", 1));
    }

    TEST_CASE("Metal public graph rejects invalid exposure bounds and mismatched real buffer associations",
              "[renderer][metal][color-output-frontend]") {
        Fixture fixture;
        fixture.Step(1.0F / 128);
        const auto accepted = fixture.previous;
        SECTION("out of bounds") {
            fixture.RejectMeter(true);
        }
        SECTION("unrelated real output buffer") {
            fixture.RejectMeter(false);
        }
        CHECK(fixture.previous.buffer == accepted.buffer);
        CheckColor(fixture.Read(fixture.Step(0.5F)), fixture.Reference("gray-8", 1));
    }

    TEST_CASE("Metal public frontend product output accepts finite FP16 HDR scenes against the pinned CPU corpus",
              "[renderer][metal][color-output-frontend]") {
        Fixture fixture;
        const auto bootstrap = fixture.Step(16);
        CHECK(fixture.ReadExposure(bootstrap).exposureScale == Catch::Approx(1));
        const std::array sources{16.0F, 128.0F, 1024.0F, 65504.0F};
        const std::array names{"gray-9", "gray-10", "gray-11", "gray-12"};
        for (std::size_t i = 0; i < sources.size(); ++i) {
            const auto produced = fixture.Step(sources[i]);
            const auto observation = fixture.Read(produced);
            INFO("Actual FP16 source gray " << sources[i]);
            CHECK(observation.exposure.exposureEv == Catch::Approx(0));
            CHECK(observation.exposure.exposureScale == Catch::Approx(1));
            CHECK(observation.exposure.reason == static_cast<std::uint32_t>(ExposureSampleReason::Valid));
            CHECK(observation.exposure.validSamples == InitialExtent.width * InitialExtent.height);
            CheckColor(observation, fixture.Reference(names[i], sources[i]), 1.0 / 2048.0);
        }
    }

    TEST_CASE("Metal product output meets independent HDR reference bounds across F32 and measured F16 storage",
              "[renderer][metal][color-output-precision]") {
        // Paired actual c28 evidence: 394-tests-c28/HoroMetalGraphExecutionSmoke.xml system-out.
        // Maximum F32 CPU residual across these four values was2.384e-7. Gray128 green wasF32 .99999988079
        // versusF16 .99951171875, with finalBGRA255. One F16 ULP bounds that measured target path;
        // this is not a universal claim about hardware rounding modes or other processor inputs.
        // Each complete fixture is destroyed before the next SDL/native owner is constructed.
        for (const auto format : std::array{RenderTextureFormat::Rgba32Float, RenderTextureFormat::Rgba16Float}) {
            Fixture fixture{format};
            fixture.Step(16);
            const std::array sources{16.0F, 128.0F, 1024.0F, 65504.0F};
            const std::array names{"gray-9", "gray-10", "gray-11", "gray-12"};
            for (std::size_t i = 0; i < sources.size(); ++i) {
                const auto observation = fixture.Read(fixture.Step(sources[i]));
                REQUIRE(std::ranges::all_of(observation.displayLinear, [](float value) {
                    return std::isfinite(value);
                }));
                REQUIRE(observation.exposure.reason == static_cast<std::uint32_t>(ExposureSampleReason::Valid));
                REQUIRE(observation.exposure.exposureEv == Catch::Approx(0));
                REQUIRE(observation.exposure.exposureScale == Catch::Approx(1));
                const auto reference = fixture.Reference(names[i], sources[i]);
                const double storeAllowance = format == RenderTextureFormat::Rgba32Float ? 0 : 1.0 / 2048.0;
                CheckColor(observation, reference, storeAllowance);
                const nlohmann::json record{{"qualification", "bounded-reference-and-format-precision"},
                                            {"format", format == RenderTextureFormat::Rgba32Float ? "Rgba32Float" : "Rgba16Float"},
                                            {"sourceFp16Gray", sources[i]},
                                            {"displayLinearGpu", observation.displayLinear},
                                            {"displayLinearCpuReference", reference},
                                            {"encodedBgra", observation.bgra},
                                            {"gpuExposureEv", observation.exposure.exposureEv},
                                            {"gpuExposureScale", observation.exposure.exposureScale},
                                            {"analyticAbsoluteBudget", 2e-5},
                                            {"storeAbsoluteBudget", storeAllowance}};
                std::cout << "HORO_COLOR_OUTPUT_PRECISION " << record.dump() << '\n';
            }
        }
    }

    TEST_CASE("Metal public color output processes exact FP16 nonneutral HDR primaries against independent references",
              "[renderer][metal][color-output-frontend]") {
        // These are the existing corpus's exact-FP16 nonneutral vectors. All saturate to white in this profile;
        // this case establishes actual colored HDR ingress, not broad non-clipped hue/conformance coverage.
        for (const auto format : std::array{RenderTextureFormat::Rgba32Float, RenderTextureFormat::Rgba16Float}) {
            Fixture fixture{format};
            fixture.Step(16);
            const std::array sources{Math::Vec3{65504, 0, 0}, Math::Vec3{0, 65504, 0}, Math::Vec3{0, 0, 65504}};
            const std::array names{"mixed-3", "mixed-4", "mixed-5"};
            for (std::size_t i = 0; i < sources.size(); ++i) {
                const auto observation = fixture.Read(fixture.Step(sources[i]));
                INFO("Actual FP16 HDR source " << sources[i].x << "," << sources[i].y << "," << sources[i].z);
                CHECK(observation.exposure.reason == static_cast<std::uint32_t>(ExposureSampleReason::Valid));
                CHECK(observation.exposure.exposureEv == Catch::Approx(0));
                CHECK(observation.exposure.exposureScale == Catch::Approx(1));
                const double storeAllowance = format == RenderTextureFormat::Rgba32Float ? 0 : 1.0 / 2048.0;
                CheckColor(observation, fixture.Reference(names[i], sources[i]), storeAllowance);
            }
        }
    }

    TEST_CASE("Metal public color output preserves nonneutral signed FP16 HDR inputs against independent CPU references",
              "[renderer][metal][color-output-frontend]") {
        for (const auto format : std::array{RenderTextureFormat::Rgba32Float, RenderTextureFormat::Rgba16Float}) {
            Fixture fixture{format};
            fixture.Step(16);
            const std::array sources{Math::Vec3{4, 0.0078125F, -0.125F}, Math::Vec3{-0.25F, 12, 0.00390625F},
                                     Math::Vec3{0.03125F, 0.125F, 2}, Math::Vec3{1, 0.25F, 0.0625F}};
            const std::array names{"half-mixed-0", "half-mixed-1", "half-mixed-2", "half-mixed-3"};
            for (std::size_t i = 0; i < sources.size(); ++i) {
                const auto observation = fixture.Read(fixture.Step(sources[i]));
                INFO("Actual signed FP16 HDR source " << sources[i].x << "," << sources[i].y << "," << sources[i].z);
                CHECK(observation.exposure.reason == static_cast<std::uint32_t>(ExposureSampleReason::Valid));
                CHECK(observation.exposure.exposureEv == Catch::Approx(0));
                CHECK(observation.exposure.exposureScale == Catch::Approx(1));
                const auto reference = fixture.Reference(names[i], sources[i]);
                CheckColor(observation, reference, MixedStoreAllowance(reference, format));
            }
        }
    }

    TEST_CASE("Metal creative LUT interpolation matches independent OCIO output at corners interiors and tetrahedral ties",
              "[renderer][metal][color-output-lut]") {
        for (const auto format : std::array{RenderTextureFormat::Rgba32Float, RenderTextureFormat::Rgba16Float}) {
            for (const auto mode : std::array{ColorLutInterpolation::Trilinear, ColorLutInterpolation::Tetrahedral}) {
                Fixture fixture{format};
                fixture.ConfigureLook(mode);
                for (const auto &reference : LutOracle::Cases) {
                    if (reference.interpolation != mode)
                        continue;
                    fixture.BootstrapLookExposure();
                    const auto observation = fixture.Read(fixture.Step(reference.input));
                    INFO("Independent OCIO LUT case " << reference.name << " interpolation " << static_cast<unsigned>(mode));
                    CheckColor(observation, reference.displayLinear, MixedStoreAllowance(reference.displayLinear, format));
                }
            }
        }
    }

    TEST_CASE("Metal creative LUT diagnoses adjacent FP16 values outside its declared domain for both interpolation modes",
              "[renderer][metal][color-output-lut]") {
        for (const auto mode : std::array{ColorLutInterpolation::Trilinear, ColorLutInterpolation::Tetrahedral}) {
            Fixture fixture{RenderTextureFormat::Rgba32Float};
            fixture.ConfigureLook(mode);
            for (const auto input : LutOracle::Outside) {
                fixture.BootstrapLookExposure();
                CheckDiagnostic(fixture.Read(fixture.Step(input)));
            }
        }
    }

    TEST_CASE("Metal explicit disabled creative LUT bypasses the atlas without replacing the analytic output program",
              "[renderer][metal][color-output-lut]") {
        Fixture fixture{RenderTextureFormat::Rgba32Float};
        fixture.ConfigureLook(ColorLutInterpolation::Tetrahedral);
        fixture.lookMode = ColorLookMode::Disabled;
        fixture.Step(16);
        const Math::Vec3 input{4, 0.0078125F, -0.125F};
        const auto observation = fixture.Read(fixture.Step(input));
        // Red4 is outside the configured creative domain. Explicit Disabled must still use the original analytic route.
        CheckColor(observation, fixture.Reference("half-mixed-0", input), 0);
    }

    TEST_CASE("Metal actual creative material rejects missing atlas and foreign frontend resources without losing its valid generation",
              "[renderer][metal][color-output-lut]") {
        Fixture fixture{RenderTextureFormat::Rgba32Float};
        fixture.ConfigureLook(ColorLutInterpolation::Trilinear);
        fixture.BootstrapLookExposure();
        SECTION("missing binding8") {
            fixture.RejectLookBinding();
        }
        SECTION("actual foreign-owner atlas") {
            ForeignTextureOwner foreign{*fixture.look};
            CHECK(foreign.view.owner != fixture.lookTexture.view.owner);
            fixture.RejectLookBinding(foreign.view);
        }
        const auto &reference = LutOracle::Cases.front();
        CheckColor(fixture.Read(fixture.Step(reference.input)), reference.displayLinear, 0);
    }
}  // namespace Horo::Render::Test
