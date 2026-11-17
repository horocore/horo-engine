#include "ColorOutputPackage.h"
#include "EditorViewportColorGraph.h"
#include "EditorViewportColorPipeline.h"
#include "MetalColorOutputLutReference.h"
#include "editor/renderer/metal/MetalViewportResourceBridge.h"
#include "editor/renderer/metal/SdlMetalPresentationPort.h"
#include "runtime/renderer/frontend/RenderFrontendResourceAccess.h"
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
#include <nlohmann/json.hpp>
#include <thread>
#include <utility>
#include <vector>

namespace Horo::Editor::Test {
    namespace {
        using namespace Render;
        using namespace std::chrono_literals;
        constexpr FramebufferExtent Extent{16, 16};

        /** @brief Preserves original errors at real public frontend and common-owner boundaries. */
        template <class T> T Take(Result<T> result) {
            INFO((result.HasError()
                      ? result.ErrorValue().domain.Value() + "/" + result.ErrorValue().code.Value() + ": " + result.ErrorValue().message
                      : "accepted"));
            REQUIRE(result.HasValue());
            return std::move(result).Value();
        }

        /** @brief Requires actual success without replacing an original typed failure. */
        void Accepted(const Result<void> &result) {
            INFO((result.HasError() ? result.ErrorValue().code.Value() + ": " + result.ErrorValue().message : "accepted"));
            REQUIRE(result.HasValue());
        }

        /** @brief Resolves only the explicitly provided genuine package root. */
        std::filesystem::path Root(const char *name) {
            const char *value = std::getenv(name);
            if (!value)
                SKIP("Supply actual HORO_COLOR_OUTPUT_PACKAGE and HORO_EXPOSURE_GPU_PACKAGE for common-owner qualification.");
            const std::filesystem::path path{value};
            REQUIRE(path.is_absolute());
            return path;
        }

        /** @brief Contains native completion waiting to the explicit test-only deadline. */
        bool Terminal(id<MTLCommandBuffer> commands) {
            const auto deadline = std::chrono::steady_clock::now() + 5s;
            while (commands && commands.status != MTLCommandBufferStatusCompleted && commands.status != MTLCommandBufferStatusError) {
                if (std::chrono::steady_clock::now() >= deadline)
                    return false;
                std::this_thread::sleep_for(1ms);
            }
            return !commands || commands.status == MTLCommandBufferStatusCompleted;
        }

        /** @brief Drains the genuine selected backend while every borrowed native parent remains alive. */
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

        /** @brief Real same-backend-queue backpressure; never a fake readiness callback or unrelated device queue. */
        class QueueGate final {
            __strong id<MTLSharedEvent> event_;
            __strong id<MTLCommandBuffer> commands_;

        public:
            /** @brief Queues a genuine wait on the exact runtime queue returned by the matching backend bridge. */
            explicit QueueGate(const MetalEditorGraphicsBridge &bridge) {
                id<MTLDevice> device = (__bridge id<MTLDevice>)bridge.Device();
                id<MTLCommandQueue> queue = (__bridge id<MTLCommandQueue>)bridge.CommandQueue();
                event_ = [device newSharedEvent];
                commands_ = [queue commandBuffer];
                REQUIRE(event_);
                REQUIRE(commands_);
                REQUIRE(commands_.commandQueue == queue);
                [commands_ encodeWaitForEvent:event_ value:1];
                [commands_ commit];
            }

            /** @brief Always opens the gate before enclosing frontend destruction, including assertion unwinding. */
            ~QueueGate() {
                Release();
            }

            /** @brief Signals only this real event; no completion status or metadata is manufactured. */
            void Release() noexcept {
                if (event_)
                    event_.signaledValue = 1;
            }
        };

        /** @brief Portable authored RGB digest shared by nonidentity and identity creative assets. */
        Sha256Digest Digest(std::span<const float> rgb) {
            Sha256Builder digest;
            for (const float value : rgb) {
                const auto word = std::bit_cast<std::uint32_t>(value);
                const std::array bytes{std::byte(word & 255U), std::byte((word >> 8U) & 255U), std::byte((word >> 16U) & 255U),
                                       std::byte(word >> 24U)};
                REQUIRE(digest.Update(bytes));
            }
            return digest.Finalize();
        }

        /** @brief Creates an explicit enabled asset through the real public factory, without a CPU interpolation evaluator. */
        std::shared_ptr<const CookedColorTransform> Look(const CookedColorOutputProgram &program, ColorLutInterpolation mode,
                                                         bool identity) {
            std::array<float, 24> rgb = Render::Test::LutOracle::CubeRgb;
            ColorTransformDescriptor descriptor;
            descriptor.configurationRelease = program.Descriptor().configurationRelease;
            descriptor.configurationDigest = program.Descriptor().configurationDigest;
            descriptor.referenceDigest = Take(ParseSha256(Render::Test::LutOracle::ReferenceDigest));
            descriptor.dimension = 2;
            descriptor.interpolation = mode;
            descriptor.domainMinimum = identity ? Math::Vec3{-16, -16, -16} : Render::Test::LutOracle::DomainMinimum;
            descriptor.domainMaximum = identity ? Math::Vec3{16, 16, 16} : Render::Test::LutOracle::DomainMaximum;
            if (identity)
                for (std::size_t vertex = 0; vertex < 8; ++vertex)
                    for (std::size_t channel = 0; channel < 3; ++channel)
                        rgb[vertex * 3 + channel] = ((vertex >> channel) & 1U) ? 16.0F : -16.0F;
            descriptor.payloadDigest = Digest(rgb);
            return Take(CookedColorTransform::Create(descriptor, rgb));
        }

        /** @brief Genuine public texture/view pair retained by the test host. */
        struct Image final {
            RenderTextureHandle texture;
            RenderTextureViewHandle view;
        };

        /** @brief One accepted real submission and its synchronous encoded attachment identities. */
        struct Submitted final {
            RenderTimelinePoint signal;
            ExposureState produced;
            Image display, output;
        };

        /** @brief Actual completed texture observations only; production exposure buffers have no CopySource permission. */
        struct Pixels final {
            std::array<float, 4> linear{};
            std::array<std::uint8_t, 4> rgba{};
            std::array<std::uint32_t, 2> coordinate{};
        };

        /** @brief Runs the actual common owner/graph with production Metal ports, not a duplicated color lifecycle. */
        class Fixture final {
        public:
            SDL_Window *window{};
            RenderBackendRegistry registry;
            MetalEditorGraphicsBridge bridge;
            std::unique_ptr<SdlMetalPresentationPort> presentation;
            std::unique_ptr<RenderFrontend> frontend;
            std::shared_ptr<const Render::Internal::ColorOutputCookPackage> package;
            CookedExposureGpuKernel exposure;
            std::unique_ptr<EditorViewportColorPipeline> owner;
            std::unique_ptr<QueueGate> gate;
            std::vector<RenderQueueAssignment> queues;
            std::vector<RenderTimelinePoint> pending;
            Image scene, display, output;
            FramebufferExtent extent{Extent};
            RenderViewId view;
            std::uint64_t frameNumber{};
            std::chrono::steady_clock::time_point clock;
            __strong id<MTLCommandBuffer> observation;

            /** @brief Loads immutable real packages and creates the actual selected frontend before common-owner activation. */
            Fixture() {
                try {
                    package = std::make_shared<const Render::Internal::ColorOutputCookPackage>(
                        Take(Render::Internal::LoadColorOutputCookPackage(Root("HORO_COLOR_OUTPUT_PACKAGE"))));
                    exposure = Take(Render::Internal::LoadExposureGpuCookPackage(Root("HORO_EXPOSURE_GPU_PACKAGE")));
                    InitializeFrontend();
                    Resize(Extent);
                    clock = std::chrono::steady_clock::now();
                } catch (...) {
                    if (!Close())
                        std::terminate();
                    throw;
                }
            }

            /** @brief Opens any test gate before bounded owner/frontend/native-parent teardown. */
            ~Fixture() {
                if (!Close())
                    std::terminate();
            }

            /** @brief Selects the real production backend on an isolated unfocused surface. */
            void InitializeFrontend() {
                REQUIRE(SDL_Init(SDL_INIT_VIDEO));
                REQUIRE(SDL_SetHint(SDL_HINT_VIDEO_METAL_AUTO_RESIZE_DRAWABLE, "0"));
                REQUIRE(SDL_SetHint(SDL_HINT_WINDOW_ACTIVATE_WHEN_SHOWN, "0"));
                window = SDL_CreateWindow("Horo common editor color qualification", 64, 64, SDL_WINDOW_METAL | SDL_WINDOW_HIDDEN);
                REQUIRE(window);
                REQUIRE(SDL_SetWindowFocusable(window, false));
                presentation = std::make_unique<SdlMetalPresentationPort>(*window);
                Accepted(RegisterMetalRenderBackend(registry, *presentation, bridge));
                Accepted(registry.Seal());
                frontend = Take(RenderFrontend::Create(registry, RenderBackendId{"metal"},
                                                       {.requirePresentation = true,
                                                        .enableValidation = true,
                                                        .maxFramesInFlight = 2,
                                                        .presentMode = PresentMode::Immediate}));
                REQUIRE(SDL_ShowWindow(window));
                const auto topology = Take(Render::Detail::RenderFrontendResourceAccess::QueueAssignments(*frontend));
                queues.assign(topology.begin(), topology.end());
                view = Take(frontend->CreateRenderViewIdentity());
            }

            /** @brief Creates actual copy-readable attachments, with the common product owner's exact FP16/RGBA8 formats. */
            Image Texture(RenderTextureFormat format) {
                Image image;
                image.texture = Take(frontend->CreateTexture({.extent = extent,
                                                              .format = format,
                                                              .usage = RenderTextureUsage::Sampled | RenderTextureUsage::RenderAttachment |
                                                                       RenderTextureUsage::CopySource}))
                                    .handle;
                Take(frontend->ProcessResourceRequests());
                REQUIRE(Take(frontend->ResourceState(image.texture)) == RenderResourceState::Ready);
                image.view = Take(frontend->CreateTextureView({.texture = image.texture, .format = format})).handle;
                Take(frontend->ProcessResourceRequests());
                REQUIRE(Take(frontend->ResourceState(image.view)) == RenderResourceState::Ready);
                return image;
            }

            /** @brief Allocates new real attachment generations while older submitted images remain retained for observation. */
            void Resize(FramebufferExtent next) {
                extent = next;
                scene = Texture(RenderTextureFormat::Rgba16Float);
                display = Texture(RenderTextureFormat::Rgba16Float);
                output = Texture(RenderTextureFormat::Rgba8Unorm);
            }

            /** @brief Supplies the same selected-backend readiness/write ports used by the production viewport adapter. */
            void InitializeColor(ColorLutInterpolation mode = ColorLutInterpolation::Tetrahedral, bool identity = false) {
                EditorViewportColorResourcePort port{this, [](void *context, RenderBufferHandle buffer) {
                    auto &self = *static_cast<Fixture *>(context);
                    return MetalViewportResourceBridge::ColorBufferReadiness(*self.frontend, buffer);
                }, [](void *context, RenderBufferHandle buffer, std::span<const std::byte> bytes) {
                    auto &self = *static_cast<Fixture *>(context);
                    const std::array updates{MetalViewportBufferUpdate{buffer, bytes, 0}};
                    return MetalViewportResourceBridge::UpdateBuffers(*self.frontend, self.bridge, updates);
                }};
                owner = std::make_unique<EditorViewportColorPipeline>(*frontend, port);
                const EditorViewportColorInputs inputs{.package = package,
                                                       .exposure = exposure,
                                                       .lookMode = ColorLookMode::Enabled,
                                                       .look = Look(*package->analytic.program, mode, identity)};
                Accepted(owner->Initialize(inputs));
                ExposureSettings settings;
                settings.mode = ExposureMode::AutomaticHistogram;
                settings.minimumEv = 0;
                settings.maximumEv = 1;
                Accepted(owner->RequestSettings(settings));
                clock = std::chrono::steady_clock::now();
            }

            /** @brief Observes the actual host view-clock interval; no provider clock or completion time is fabricated. */
            float Delta() {
                const auto now = std::chrono::steady_clock::now();
                const auto delta = std::chrono::duration<float>(now - clock).count();
                clock = now;
                REQUIRE(delta >= 0);
                REQUIRE(delta <= 1);
                return delta;
            }

            /** @brief Retries only genuine common-owner Pending using the actual completion callback. */
            void Prepare() {
                const auto deadline = std::chrono::steady_clock::now() + 5s;
                while (std::chrono::steady_clock::now() < deadline) {
                    const auto prepared = owner->Prepare(view, extent, Delta());
                    if (prepared.HasValue()) {
                        Accepted(owner->PrepareBindings(scene.view, display.view));
                        return;
                    }
                    REQUIRE(prepared.ErrorValue().domain.Value() == ViewportColorErrors::Pending.domain.Value());
                    REQUIRE(prepared.ErrorValue().code.Value() == ViewportColorErrors::Pending.code.Value());
                    std::this_thread::sleep_for(1ms);
                }
                FAIL("Actual common color readiness exceeded five seconds.");
            }

            /** @brief Borrows only the selected frontend's real graphics queue identity. */
            RenderQueueId Queue() const {
                const auto found = std::ranges::find(queues, RenderQueueRole::Graphics, &RenderQueueAssignment::role);
                REQUIRE(found != queues.end());
                return found->queue;
            }

            /** @brief Acquires an actual presentation frame within one finite test deadline. */
            RenderFrameScope Begin() {
                const auto number = ++frameNumber;
                const auto deadline = std::chrono::steady_clock::now() + 5s;
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
                FAIL("Actual native frame acquisition exceeded five seconds.");
                std::terminate();
            }

            /** @brief Executes the actual common graph; the precreated overlay anchor intentionally forces non-creation pass order. */
            Submitted SubmitPrepared(Math::Vec3 source, bool cancel = false) {
                auto builder =
                    Take(RenderGraphBuilder::Create({.maxPasses = 8, .maxResources = 24, .maxUsages = 48, .maxDependencies = 16}));
                const auto sceneId = Take(builder.ImportTexture(scene.texture, RenderGraphResourceClass::Persistent));
                const auto displayId = Take(builder.ImportTexture(display.texture, RenderGraphResourceClass::Persistent));
                const auto outputId = Take(builder.ImportTexture(output.texture, RenderGraphResourceClass::Persistent));
                std::vector<RenderGraphImportedState> initial{{sceneId, {}, RenderGraphInitialContents::Discarded},
                                                              {displayId, {}, RenderGraphInitialContents::Discarded},
                                                              {outputId, {}, RenderGraphInitialContents::Discarded}};
                const auto clear = Take(builder.AddPass(RenderPassKind::Graphics, RenderQueueRole::Graphics));
                const auto overlay = Take(builder.AddPass(RenderPassKind::Graphics, RenderQueueRole::Graphics));
                Accepted(builder.AddUsage({clear, sceneId, RenderGraphAccess::Write, RenderGraphUsageKind::ColorAttachment}));
                EditorViewportColorGraph color;
                const auto analytic =
                    Take(color.AppendScene(builder, *owner, Queue(), initial, sceneId, displayId, display.view, clear, extent));
                Accepted(builder.AddDependency({analytic, overlay, RenderGraphDependencyKind::ExecutionOrder}));
                Take(color.AppendEncode(builder, *owner, outputId, output.view, overlay));
                const auto graph = Take(builder.Finalize());
                const auto schedule = Take(CompileRenderGraph(graph));
                const auto sync = Take(SynthesizeRenderGraphSynchronization(graph, schedule, queues, initial));
                const auto compiled = Take(CompileRenderGraphExecution(graph, schedule, sync, queues));
                color.Refresh(*owner);
                std::vector<RenderGraphPassWorkload> inventory{{clear, RenderGraphColorAttachment{sceneId,
                                                                                                  {.clearColor = {source.x, source.y,
                                                                                                                  source.z, 1}}}},
                                                               {overlay, std::monostate{}}};
                inventory.insert(inventory.end(), color.Workloads().begin(), color.Workloads().end());
                std::vector<RenderGraphPassWorkload> ordered;
                for (const auto &pass : compiled.Passes()) {
                    const auto found = std::ranges::find(inventory, pass.pass, &RenderGraphPassWorkload::pass);
                    REQUIRE(found != inventory.end());
                    ordered.push_back(*found);
                }
                auto frame = Begin();
                const auto issued = Take(frame.ReserveSubmission(RenderQueueRole::Graphics));
                const std::array submissions{RenderGraphQueueSubmission{issued, {0, compiled.Passes().size()}}};
                Accepted(frame.ExecuteSubmittedGraph(compiled, ordered, submissions));
                owner->Encoded();
                Submitted result{issued.signal, owner->Produced(), display, output};
                if (cancel) {
                    frame.Cancel();
                    owner->Finish(false);
                    return result;
                }
                const auto presented = frame.Present();
                owner->Finish(presented.HasValue());
                Accepted(presented);
                pending.push_back(issued.signal);
                return result;
            }

            /** @brief Uses actual common preparation before each real submitted graph. */
            Submitted Submit(Math::Vec3 source) {
                Prepare();
                return SubmitPrepared(source);
            }

            /** @brief Observes only a frontend-issued actual native completion point. */
            bool Wait(RenderTimelinePoint signal) {
                const auto deadline = std::chrono::steady_clock::now() + 5s;
                while (std::chrono::steady_clock::now() < deadline) {
                    const auto done = frontend->ObserveSubmission(
                        {signal, RenderCpuWaitPurpose::Poll, 0ns, frontend->Capabilities().support.deviceIncarnation});
                    if (done.HasError())
                        return false;
                    if (done.Value())
                        return true;
                    std::this_thread::sleep_for(1ms);
                }
                return false;
            }

            /** @brief Copies actual completed display/final pixels only; no production state buffer permission is bypassed. */
            Pixels Read(const Submitted &submitted) {
                REQUIRE(Wait(submitted.signal));
                const auto linearId =
                    Take(Render::Detail::RenderFrontendResourceAccess::BackendInstance(*frontend, submitted.display.view));
                const auto outputId = Take(Render::Detail::RenderFrontendResourceAccess::BackendInstance(*frontend, submitted.output.view));
                auto *linear = reinterpret_cast<Render::Detail::MetalTextureViewInstance *>(static_cast<std::uintptr_t>(linearId));
                auto *encoded = reinterpret_cast<Render::Detail::MetalTextureViewInstance *>(static_cast<std::uintptr_t>(outputId));
                id<MTLDevice> device = (__bridge id<MTLDevice>)bridge.Device();
                id<MTLCommandQueue> queue = (__bridge id<MTLCommandQueue>)bridge.CommandQueue();
                id<MTLBuffer> bytes = [device newBufferWithLength:512 options:MTLResourceStorageModeShared];
                observation = [queue commandBuffer];
                id<MTLBlitCommandEncoder> blit = [observation blitCommandEncoder];
                REQUIRE(bytes);
                REQUIRE(blit);
                Copy(blit, linear->texture, bytes, 0);
                Copy(blit, encoded->texture, bytes, 256);
                [blit endEncoding];
                [observation commit];
                REQUIRE(Terminal(observation));
                std::array<std::uint16_t, 4> half;
                std::memcpy(half.data(), bytes.contents, sizeof(half));
                Pixels pixels;
                pixels.coordinate = {static_cast<std::uint32_t>(encoded->texture.width / 2),
                                     static_cast<std::uint32_t>(encoded->texture.height / 2)};
                for (std::size_t i = 0; i < half.size(); ++i)
                    pixels.linear[i] = static_cast<float>(std::bit_cast<_Float16>(half[i]));
                std::memcpy(pixels.rgba.data(), static_cast<const std::byte *>(bytes.contents) + 256, 4);
                return pixels;
            }

            /** @brief Copies a real in-bounds pixel using the source texture's actual dimensions and native row alignment. */
            void Copy(id<MTLBlitCommandEncoder> blit, id<MTLTexture> texture, id<MTLBuffer> bytes, NSUInteger offset) {
                [blit copyFromTexture:texture
                                 sourceSlice:0
                                 sourceLevel:0
                                sourceOrigin:MTLOriginMake(texture.width / 2, texture.height / 2, 0)
                                  sourceSize:MTLSizeMake(1, 1, 1)
                                    toBuffer:bytes
                           destinationOffset:offset
                      destinationBytesPerRow:256
                    destinationBytesPerImage:256];
            }

            /** @brief Releases every real event gate before any finite completion/drain attempt, including assertion unwinding. */
            bool Close() noexcept {
                try {
                    gate.reset();
                    if (!Terminal(observation))
                        return false;
                    for (const auto signal : pending)
                        if (!Wait(signal))
                            return false;
                    if (owner && owner->Close().HasError())
                        return false;
                    owner.reset();
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
        };

        /** @brief Selects an independent OCIO LUT-plus-output observation without evaluating interpolation in the test. */
        const Render::Test::LutOracle::Case &Reference(std::string_view name, ColorLutInterpolation mode) {
            const auto &cases = Render::Test::LutOracle::Cases;
            const auto found = std::ranges::find_if(cases, [=](const auto &entry) {
                return entry.name == name && entry.interpolation == mode;
            });
            REQUIRE(found != cases.end());
            return *found;
        }

        /** @brief Checks the actual typed backpressure identity without consuming or inventing a frame receipt. */
        void Pending(const Result<void> &result) {
            REQUIRE(result.HasError());
            CHECK(result.ErrorValue().domain.Value() == ViewportColorErrors::Pending.domain.Value());
            CHECK(result.ErrorValue().code.Value() == ViewportColorErrors::Pending.code.Value());
        }

        /** @brief Binds an exact finite scene input to its independent pinned analytic CPU observation. */
        std::array<float, 3> AnalyticReference(const Render::Internal::ColorOutputCookPackage &package, std::string_view name,
                                               Math::Vec3 input) {
            REQUIRE(package.analytic.files[2].size() < 64U * 1024U);
            const auto corpus = nlohmann::json::parse(package.analytic.files[2]);
            REQUIRE(corpus.at("cases").size() < 128);
            for (const auto &entry : corpus.at("cases")) {
                if (entry.at("name") != name)
                    continue;
                const std::array<float, 3> expected{input.x, input.y, input.z};
                const auto actual = entry.at("input").get<std::array<float, 3>>();
                REQUIRE(actual == expected);
                return entry.at("displayLinearRgb").get<std::array<float, 3>>();
            }
            FAIL("Independent pinned analytic reference is missing.");
            std::terminate();
        }

        /** @brief Compares only actual texture output with independent values and the already measured format/encoding bounds. */
        void CheckPixels(const Pixels &pixels, std::array<float, 3> reference) {
            for (std::size_t i = 0; i < reference.size(); ++i) {
                int exponent{};
                const auto magnitude = std::abs(reference[i]);
                std::frexp(magnitude, &exponent);
                const double store = magnitude < std::ldexp(1.0F, -14) ? std::ldexp(1.0, -24) : std::ldexp(1.0, exponent - 11);
                CHECK(pixels.linear[i] == Catch::Approx(reference[i]).epsilon(0).margin(store + 2e-5));
                // Encode consumes the actual stored FP16 value, whose independent analytic error is checked above.
                const float linear = std::clamp(pixels.linear[i], 0.0F, 1.0F);
                const float encoded = linear <= .0031308F ? linear * 12.92F : 1.055F * std::pow(linear, 1.0F / 2.4F) - .055F;
                constexpr std::array<unsigned, 16> bayer{0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5};
                const auto phase = (pixels.coordinate[1] & 3U) * 4U + (pixels.coordinate[0] & 3U);
                const float noise = ((float(bayer[phase]) + .5F) / 16 - .5F) / 255;
                const float expected = std::clamp(encoded + noise, 0.0F, 1.0F);
                CHECK(float(pixels.rgba[i]) / 255 == Catch::Approx(expected).epsilon(0).margin(1.0 / 255));
            }
            CHECK(pixels.rgba[3] == 255);
        }
    }  // namespace

    TEST_CASE("Common editor color owner uses actual pending uploads and retries without inventing acceptance",
              "[renderer][metal][editor-color-backpressure]") {
        Fixture fixture;
        fixture.gate = std::make_unique<QueueGate>(fixture.bridge);
        fixture.InitializeColor();
        const auto prepared = fixture.owner->Prepare(fixture.view, fixture.extent, fixture.Delta());
        REQUIRE(prepared.HasError());
        CHECK(prepared.ErrorValue().domain.Value() == ViewportColorErrors::Pending.domain.Value());
        CHECK(prepared.ErrorValue().code.Value() == ViewportColorErrors::Pending.code.Value());
        CHECK_FALSE(fixture.owner->Diagnostics().candidate);
        CHECK(fixture.owner->Diagnostics().acceptedSubmission == 0);
        fixture.gate.reset();
        const auto &reference = Reference("tie-xyz", ColorLutInterpolation::Tetrahedral);
        CheckPixels(fixture.Read(fixture.Submit(reference.input)), reference.displayLinear);
    }

    TEST_CASE("Common editor color owner submits enabled nonidentity lookup through its actual graph and production ports",
              "[renderer][metal][editor-color-owner]") {
        Fixture fixture;
        fixture.InitializeColor();
        const auto &reference = Reference("tie-xyz", ColorLutInterpolation::Tetrahedral);
        const auto first = fixture.Submit(reference.input);
        CheckPixels(fixture.Read(first), reference.displayLinear);
        const auto second = fixture.Submit(reference.input);
        CheckPixels(fixture.Read(second), reference.displayLinear);
        CHECK(second.produced.sourceSubmission == first.produced.sourceSubmission + 1);
        CHECK(fixture.owner->Diagnostics().acceptedSubmission == second.produced.sourceSubmission);
        CHECK_FALSE(fixture.owner->Diagnostics().automaticEv.has_value());
    }

    TEST_CASE("Common editor color owner preserves accepted history after actual unsent cancellation and defers reset during a candidate",
              "[renderer][metal][editor-color-owner]") {
        Fixture fixture;
        fixture.InitializeColor();
        const auto &reference = Reference("tie-xyz", ColorLutInterpolation::Tetrahedral);
        const auto first = fixture.Submit(reference.input);
        fixture.Read(first);
        fixture.Prepare();
        fixture.SubmitPrepared(reference.input, true);
        CHECK(fixture.owner->Diagnostics().acceptedSubmission == first.produced.sourceSubmission);
        CHECK(fixture.owner->Previous().buffer == first.produced.buffer);
        fixture.Prepare();
        const auto material = fixture.owner->Material(0);
        const auto prior = fixture.owner->Previous();
        fixture.owner->Reset();
        CHECK(fixture.owner->Material(0) == material);
        CHECK(fixture.owner->Previous().buffer == prior.buffer);
        const auto accepted = fixture.SubmitPrepared(reference.input);
        fixture.Read(accepted);
        CHECK(fixture.owner->Diagnostics().resetPending);
        fixture.Prepare();
        CHECK_FALSE(fixture.owner->Previous().buffer.IsValid());
        const auto reset = fixture.SubmitPrepared(reference.input);
        CheckPixels(fixture.Read(reset), reference.displayLinear);
        CHECK(reset.produced.generation > accepted.produced.generation);
        CHECK(reset.produced.sourceSubmission == accepted.produced.sourceSubmission + 1);
    }

    TEST_CASE("Common editor enabled identity LUT preserves signed HDR analytic output for both interpolation modes",
              "[renderer][metal][editor-color-owner]") {
        for (const auto mode : std::array{ColorLutInterpolation::Trilinear, ColorLutInterpolation::Tetrahedral}) {
            Fixture fixture;
            fixture.InitializeColor(mode, true);
            const std::array inputs{Math::Vec3{4, .0078125F, -.125F}, Math::Vec3{-.25F, 12, .00390625F}, Math::Vec3{.03125F, .125F, 2},
                                    Math::Vec3{1, .25F, .0625F}};
            for (std::size_t i = 0; i < inputs.size(); ++i) {
                const auto name = "half-mixed-" + std::to_string(i);
                const auto reference = AnalyticReference(*fixture.package, name, inputs[i]);
                CheckPixels(fixture.Read(fixture.Submit(inputs[i])), reference);
            }
        }
    }

    TEST_CASE("Common editor color owner retains accepted leases while a free slot prepares an in-flight resize",
              "[renderer][metal][editor-color-owner]") {
        Fixture fixture;
        fixture.InitializeColor();
        const auto &first = Reference("tie-xyz", ColorLutInterpolation::Tetrahedral);
        CheckPixels(fixture.Read(fixture.Submit(first.input)), first.displayLinear);
        fixture.Prepare();
        auto settings = fixture.owner->Settings();
        settings.brightenEvPerSecond = 4;
        const auto generation = fixture.owner->Diagnostics().settingsGeneration;
        Pending(fixture.owner->RequestSettings(settings));
        CHECK(fixture.owner->Diagnostics().settingsGeneration == generation);
        fixture.gate = std::make_unique<QueueGate>(fixture.bridge);
        const auto held = fixture.SubmitPrepared(first.input);
        Accepted(fixture.owner->RequestSettings(settings));
        CHECK(fixture.owner->Diagnostics().settingsGeneration == generation + 1);
        fixture.Resize({8, 8});
        fixture.Prepare();
        CHECK(fixture.owner->Diagnostics().candidate);
        CHECK(fixture.owner->Diagnostics().acceptedSubmission == held.produced.sourceSubmission);
        const auto resizedMaterial = fixture.owner->Material(0);
        REQUIRE(resizedMaterial);
        fixture.owner->Finish(false);
        CHECK_FALSE(fixture.owner->Diagnostics().candidate);
        CHECK(fixture.owner->Diagnostics().acceptedSubmission == held.produced.sourceSubmission);
        fixture.gate.reset();
        CheckPixels(fixture.Read(held), first.displayLinear);
        const auto &second = Reference("unequal-xyz", ColorLutInterpolation::Tetrahedral);
        const auto resized = fixture.Submit(second.input);
        CheckPixels(fixture.Read(resized), second.displayLinear);
        CHECK(resized.display.view != held.display.view);
        CHECK(resized.produced.sourceSubmission == held.produced.sourceSubmission + 1);
        CHECK(resized.produced.generation > held.produced.generation);
        CHECK(resized.produced.settingsGeneration == generation + 1);
        settings.timeSource = ExposureTimeSource::Simulation;
        const auto denied = fixture.owner->RequestSettings(settings);
        REQUIRE(denied.HasError());
        CHECK(denied.ErrorValue().domain.Value() == ExposureGpuErrors::Unsupported.domain.Value());
        CHECK(denied.ErrorValue().code.Value() == ExposureGpuErrors::Unsupported.code.Value());
        CHECK(fixture.owner->Diagnostics().settingsGeneration == generation + 1);
    }

    TEST_CASE("Common editor automatic exposure consumes the actual dark-frame GPU scale in the next lookup",
              "[renderer][metal][editor-color-owner]") {
        Fixture fixture;
        fixture.InitializeColor();
        auto settings = fixture.owner->Settings();
        settings.brightenEvPerSecond = 64;
        Accepted(fixture.owner->RequestSettings(settings));
        // Retire only unencoded candidates while observing real readiness for each bounded storage slot.
        for (std::size_t i = 0; i < EditorViewportColorPipeline::SlotCount; ++i) {
            fixture.Prepare();
            fixture.owner->Finish(false);
        }
        std::this_thread::sleep_for(20ms);
        fixture.Prepare();
        REQUIRE(fixture.owner->Dispatch().exposure.timing[0] >= 1.0F / 64);
        REQUIRE(fixture.owner->Dispatch().exposure.adaptation[2] == 64);
        const auto dark = fixture.SubmitPrepared({.0009765625F, .0009765625F, .0009765625F});
        REQUIRE(fixture.Wait(dark.signal));
        const auto &reference = Reference("tie-xyz", ColorLutInterpolation::Tetrahedral);
        const auto lit = fixture.Submit({.5F, .5F, .5F});
        CHECK(lit.produced.sourceSubmission == dark.produced.sourceSubmission + 1);
        CheckPixels(fixture.Read(lit), reference.displayLinear);
        CHECK_FALSE(fixture.owner->Diagnostics().automaticEv.has_value());
    }
}  // namespace Horo::Editor::Test
