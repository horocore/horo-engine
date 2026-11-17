#include "renderer/MetalEditorViewportColorFixture.h"

#include "ColorOutputPackage.h"
#include "EditorBaselineLightingAssets.h"
#include "EditorClusteredLightingAssets.h"
#include "editor/renderer/metal/EditorViewportRendererMetal.h"
#include "editor/renderer/metal/MetalViewportResourceBridge.h"
#include "editor/renderer/metal/SdlMetalPresentationPort.h"
#include "integration/renderer/MetalColorOutputLutReference.h"
#include "runtime/renderer/frontend/RenderFrontendResourceAccess.h"
#include "runtime/renderer/modules/metal/MetalResourceInstances.h"

#import <Metal/Metal.h>
#include <SDL3/SDL.h>
#include <algorithm>
#include <bit>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <thread>
#include <utility>

namespace Horo::Editor::Test {
    namespace {
        using namespace Render;
        using namespace std::chrono_literals;
        constexpr FramebufferExtent Extent{64, 64};

        /** @brief Retains original typed failures at all real renderer/asset boundaries. */
        template <class T> T Take(Result<T> result) {
            INFO((result.HasError() ? result.ErrorValue().code.Value() + ": " + result.ErrorValue().message : "accepted"));
            REQUIRE(result.HasValue());
            return std::move(result).Value();
        }

        /** @brief Requires success without translating away the actual typed reason. */
        void Accepted(const Result<void> &result) {
            INFO((result.HasError() ? result.ErrorValue().code.Value() + ": " + result.ErrorValue().message : "accepted"));
            REQUIRE(result.HasValue());
        }

        /** @brief Uses only a host-provided actual package, with no search or fabricated artifact metadata. */
        std::filesystem::path Root(const char *name) {
            const char *value = std::getenv(name);
            if (!value)
                SKIP("Supply genuine baseline/clustered/color/exposure packages for full viewport qualification.");
            const std::filesystem::path root{value};
            REQUIRE(root.is_absolute());
            return root;
        }

        /** @brief Waits only inside this explicit native qualification's five-second completion budget. */
        bool Complete(id<MTLCommandBuffer> commands) {
            const auto deadline = std::chrono::steady_clock::now() + 5s;
            while (commands && commands.status != MTLCommandBufferStatusCompleted && commands.status != MTLCommandBufferStatusError) {
                if (std::chrono::steady_clock::now() >= deadline)
                    return false;
                std::this_thread::sleep_for(1ms);
            }
            return !commands || commands.status == MTLCommandBufferStatusCompleted;
        }

        /** @brief Preserves native owners while draining ordinary frontend retirement with a finite deadline. */
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

        /** @brief Hashes exact portable authored RGB bytes; this never evaluates interpolation or creates shader evidence. */
        Sha256Digest CubeDigest() {
            Sha256Builder digest;
            for (const float value : Render::Test::LutOracle::CubeRgb) {
                const auto bits = std::bit_cast<std::uint32_t>(value);
                const std::array bytes{std::byte(bits & 255U), std::byte((bits >> 8U) & 255U), std::byte((bits >> 16U) & 255U),
                                       std::byte(bits >> 24U)};
                REQUIRE(digest.Update(bytes));
            }
            return digest.Finalize();
        }

        /** @brief Prepares the same independently referenced actual nonidentity cube through the canonical public factory. */
        std::shared_ptr<const CookedColorTransform> Look(const CookedColorOutputProgram &program, ColorLutInterpolation mode) {
            ColorTransformDescriptor descriptor;
            descriptor.configurationRelease = program.Descriptor().configurationRelease;
            descriptor.configurationDigest = program.Descriptor().configurationDigest;
            descriptor.referenceDigest = Take(ParseSha256(Render::Test::LutOracle::ReferenceDigest));
            descriptor.payloadDigest = CubeDigest();
            descriptor.dimension = 2;
            descriptor.interpolation = mode;
            descriptor.domainMinimum = Render::Test::LutOracle::DomainMinimum;
            descriptor.domainMaximum = Render::Test::LutOracle::DomainMaximum;
            return Take(CookedColorTransform::Create(descriptor, Render::Test::LutOracle::CubeRgb));
        }
    }  // namespace

    using namespace Render;
    using namespace std::chrono_literals;

    /** @copydoc ViewportColorPixels::Scene */
    std::array<float, 3> ViewportColorPixels::Scene(std::uint32_t x, std::uint32_t y) const {
        REQUIRE(x < extent.width);
        REQUIRE(y < extent.height);
        const auto offset = (std::size_t{y} * extent.width + x) * 4;
        REQUIRE(offset + 4 <= rawScene.size());
        std::array<float, 3> result;
        for (std::size_t i = 0; i < result.size(); ++i)
            result[i] = float(std::bit_cast<_Float16>(rawScene[offset + i]));
        return result;
    }

    /** @copydoc ViewportColorPixels::Final */
    std::array<std::uint8_t, 4> ViewportColorPixels::Final(std::uint32_t x, std::uint32_t y) const {
        REQUIRE(x < extent.width);
        REQUIRE(y < extent.height);
        const auto offset = (std::size_t{y} * extent.width + x) * 4;
        REQUIRE(offset + 4 <= encoded.size());
        return {encoded[offset], encoded[offset + 1], encoded[offset + 2], encoded[offset + 3]};
    }

    /** @brief Retains all production owners and real native command observations until bounded cleanup completes. */
    struct MetalEditorViewportColorFixture::Impl final {
        SDL_Window *window{};
        MetalEditorGraphicsBridge bridge;
        RenderBackendRegistry registry;
        std::unique_ptr<SdlMetalPresentationPort> presentation;
        std::unique_ptr<RenderFrontend> frontend;
        std::unique_ptr<EditorViewportRendererMetal> viewport;
        RenderTargetHandle target;
        RenderViewId view;
        std::uint64_t frameNumber{};
        std::chrono::steady_clock::time_point clock{std::chrono::steady_clock::now()};
        __strong id<MTLCommandBuffer> submitted, observation;

        /** @brief Releases real frontend queues before viewport, SDL or device parents disappear. */
        bool Close() noexcept {
            try {
                if (!Complete(observation) || !Complete(submitted))
                    return false;
                if (frontend && viewport)
                    frontend->DetachStaticMeshPassExecutor(*viewport);
                if (viewport)
                    viewport->Shutdown();
                if (frontend && !Drain(*frontend))
                    return false;
                viewport.reset();
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

        /** @brief Uses the same real production SDL/native backend bridge as the existing viewport smoke. */
        void InitializeFrontend() {
            REQUIRE(SDL_Init(SDL_INIT_VIDEO));
            REQUIRE(SDL_SetHint(SDL_HINT_VIDEO_METAL_AUTO_RESIZE_DRAWABLE, "0"));
            REQUIRE(SDL_SetHint(SDL_HINT_WINDOW_ACTIVATE_WHEN_SHOWN, "0"));
            window = SDL_CreateWindow("Horo actual mesh color/depth qualification", 64, 64, SDL_WINDOW_METAL | SDL_WINDOW_HIDDEN);
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
            viewport = std::make_unique<EditorViewportRendererMetal>(*frontend, bridge);
            Accepted(viewport->Initialize());
            view = Take(frontend->CreateRenderViewIdentity());
            Accepted(viewport->BindLightingViewIdentity(view));
            Accepted(frontend->AttachStaticMeshPassExecutor(*viewport));
        }

        /** @brief Admits the genuine complete color family before allocating any mesh viewport attachments. */
        void InitializeColor(std::optional<ColorLutInterpolation> look) {
            auto package = std::make_shared<const Render::Internal::ColorOutputCookPackage>(
                Take(Render::Internal::LoadColorOutputCookPackage(Root("HORO_COLOR_OUTPUT_PACKAGE"))));
            const EditorViewportColorInputs inputs{.package = package,
                                                   .exposure = Take(
                                                       Render::Internal::LoadExposureGpuCookPackage(Root("HORO_EXPOSURE_GPU_PACKAGE"))),
                                                   .lookMode = look ? ColorLookMode::Enabled : ColorLookMode::Disabled,
                                                   .look = look ? Look(*package->analytic.program, *look) : nullptr};
            Accepted(viewport->PrepareColorPipeline(inputs));
            ExposureSettings settings;
            settings.mode = ExposureMode::AutomaticHistogram;
            settings.minimumEv = 0;
            settings.maximumEv = 0;
            Accepted(viewport->RequestColorSettings(settings));
        }

        /** @brief Selects actual CPU-forward raster data without introducing a compute kernel or fallback. */
        void Baseline() {
            const auto assets = Take(LoadPackagedMetalBaselineLighting(Root("HORO_BASELINE_LIGHTING_ASSET_ROOT")));
            EditorViewportLightingProfile profile;
            profile.requested = LightPreparationMode::BaselineForwardCpu;
            profile.fallback = LightPreparationFallback::Forbidden;
            Accepted(viewport->PrepareBaselineLighting(assets.ProgramViews(), profile));
            const auto selection = Take(frontend->ResolveLightPreparation(profile.requested, profile.fallback, {}));
            REQUIRE(selection.mode == LightPreparationMode::BaselineForwardCpu);
            Accepted(viewport->RequestLightingProfile(profile, selection));
        }

        /** @brief Selects the actual clustered raster/storage/compute family with fallback forbidden. */
        void Clustered() {
            const auto assets = Take(LoadPackagedMetalClusteredLighting(Root("HORO_CLUSTERED_LIGHTING_ASSET_ROOT")));
            EditorViewportLightingProfile profile;
            profile.fallback = LightPreparationFallback::Forbidden;
            Accepted(viewport->PrepareClusteredLighting(assets.ProgramViews(), assets.kernel, profile));
            const auto selection =
                Take(frontend->ResolveLightPreparation(profile.requested, profile.fallback,
                                                       {.cullingKernel = true, .storageRaster = true, .depthPrepass = true}));
            REQUIRE(selection.mode == LightPreparationMode::ClusteredForwardNative);
            Accepted(viewport->RequestLightingProfile(profile, selection));
        }

        /** @brief Prepares only real resident CPU mesh and actual target generations at the existing cold boundary. */
        void Resources(const RenderSceneView &scene) {
            const auto deadline = std::chrono::steady_clock::now() + 5s;
            while (std::chrono::steady_clock::now() < deadline) {
                viewport->RequestExtent({Extent.width, Extent.height});
                const auto prepared = Take(viewport->PrepareResources(*frontend, scene));
                Take(frontend->ProcessResourceRequests());
                if (prepared) {
                    target = *prepared;
                    return;
                }
                std::this_thread::sleep_for(1ms);
            }
            FAIL("Actual viewport attachment preparation exceeded five seconds.");
        }

        /** @brief Preserves original retryable typed readiness while observing the actual host view-clock delta. */
        void Prepare(const StaticMeshPassDescriptor &pass) {
            const auto deadline = std::chrono::steady_clock::now() + 5s;
            while (std::chrono::steady_clock::now() < deadline) {
                const auto now = std::chrono::steady_clock::now();
                const auto elapsed = std::chrono::duration<float>(now - clock).count();
                REQUIRE(elapsed >= 0);
                viewport->SetColorFrameDelta(std::min(elapsed, 1.0F));
                clock = now;
                const auto ready = viewport->PrepareLightingFrame(pass, view);
                if (ready.HasValue())
                    return;
                const auto &code = ready.ErrorValue().code.Value();
                INFO((code + ": " + ready.ErrorValue().message));
                REQUIRE((code == ViewportColorErrors::Pending.code.Value() || code == LightCullingErrors::Pending.code.Value()));
                viewport->FinishColorFrame(false);
                std::this_thread::sleep_for(1ms);
            }
            FAIL("Actual viewport graph preparation exceeded five seconds.");
        }

        /** @brief Acquires only a real presentable frontend scope without fabricating native readiness. */
        RenderFrameScope Begin() {
            const auto deadline = std::chrono::steady_clock::now() + 5s;
            const auto number = ++frameNumber;
            while (std::chrono::steady_clock::now() < deadline) {
                SDL_PumpEvents();
                const auto available = frontend->PollPresentationAvailability(Extent);
                if (!available.HasValue() || !available.Value()) {
                    std::this_thread::sleep_for(1ms);
                    continue;
                }
                auto frame = frontend->BeginFrame({.frameNumber = number, .outputExtent = Extent});
                if (frame.HasValue())
                    return std::move(frame).Value();
                REQUIRE(frame.ErrorValue().code.Value() == RenderPresentationErrors::AcquisitionPending.code.Value());
            }
            FAIL("Actual native viewport frame acquisition exceeded five seconds.");
            std::terminate();
        }

        /** @brief Copies admitted source storage using only the actual selected backend queue after successful completion. */
        std::vector<std::byte> Copy(id<MTLTexture> texture, std::size_t rowBytes) {
            id<MTLDevice> device = (__bridge id<MTLDevice>)bridge.Device();
            id<MTLCommandQueue> queue = (__bridge id<MTLCommandQueue>)bridge.CommandQueue();
            const auto size = rowBytes * Extent.height;
            id<MTLBuffer> bytes = [device newBufferWithLength:size options:MTLResourceStorageModeShared];
            REQUIRE(bytes);
            observation = [queue commandBuffer];
            id<MTLBlitCommandEncoder> blit = [observation blitCommandEncoder];
            REQUIRE(blit);
            [blit copyFromTexture:texture
                             sourceSlice:0
                             sourceLevel:0
                            sourceOrigin:MTLOriginMake(0, 0, 0)
                              sourceSize:MTLSizeMake(Extent.width, Extent.height, 1)
                                toBuffer:bytes
                       destinationOffset:0
                  destinationBytesPerRow:rowBytes
                destinationBytesPerImage:size];
            [blit endEncoding];
            [observation commit];
            REQUIRE(Complete(observation));
            std::vector<std::byte> result(size);
            std::memcpy(result.data(), bytes.contents, size);
            return result;
        }
    };

    /** @copydoc MetalEditorViewportColorFixture::MetalEditorViewportColorFixture */
    MetalEditorViewportColorFixture::MetalEditorViewportColorFixture(ViewportColorLighting lighting,
                                                                     std::optional<ColorLutInterpolation> look)
        : impl_(std::make_unique<Impl>()) {
        try {
            impl_->InitializeFrontend();
            impl_->InitializeColor(look);
            if (lighting == ViewportColorLighting::Baseline)
                impl_->Baseline();
            else
                impl_->Clustered();
            impl_->clock = std::chrono::steady_clock::now();
        } catch (...) {
            if (!impl_->Close())
                std::terminate();
            throw;
        }
    }

    /** @copydoc MetalEditorViewportColorFixture::~MetalEditorViewportColorFixture */
    MetalEditorViewportColorFixture::~MetalEditorViewportColorFixture() {
        if (!impl_->Close())
            std::terminate();
    }

    /** @copydoc MetalEditorViewportColorFixture::Viewport */
    EditorViewportRendererMetal &MetalEditorViewportColorFixture::Viewport() noexcept {
        return *impl_->viewport;
    }

    /** @copydoc MetalEditorViewportColorFixture::Render */
    void MetalEditorViewportColorFixture::Render(const RenderSceneView &scene, bool grid) {
        REQUIRE(Complete(impl_->submitted));
        impl_->viewport->RequestGrid({.visible = grid});
        impl_->Resources(scene);
        const StaticMeshPassDescriptor pass{.target = impl_->target, .extent = Extent, .scene = scene};
        impl_->Prepare(pass);
        auto frame = impl_->Begin();
        const std::array passes{RenderPassDescriptor{.id = {1}, .kind = RenderPassKind::Graphics, .staticMesh = pass},
                                RenderPassDescriptor{.id = {2},
                                                     .kind = RenderPassKind::Graphics,
                                                     .primaryOutput = PrimaryOutputAttachment{}}};
        Accepted(impl_->viewport->ExecutePreparedFrame(frame, passes));
        impl_->submitted = (__bridge id<MTLCommandBuffer>)impl_->bridge.CurrentCommandBuffer();
        REQUIRE(impl_->submitted);
        const auto presented = frame.Present();
        impl_->viewport->FinishColorFrame(presented.HasValue());
        Accepted(presented);
        REQUIRE(Complete(impl_->submitted));
    }

    /** @copydoc MetalEditorViewportColorFixture::Read */
    ViewportColorPixels MetalEditorViewportColorFixture::Read() {
        REQUIRE(Complete(impl_->submitted));
        const auto rawId = Take(Detail::RenderFrontendResourceAccess::BackendInstance(*impl_->frontend, impl_->target));
        const auto *target = reinterpret_cast<Detail::MetalRenderTargetInstance *>(static_cast<std::uintptr_t>(rawId));
        const auto sceneId =
            Take(Detail::RenderFrontendResourceAccess::BackendInstance(*impl_->frontend, target->descriptor.colorAttachment));
        const auto *scene = reinterpret_cast<Detail::MetalTextureViewInstance *>(static_cast<std::uintptr_t>(sceneId));
        const auto finalId =
            Take(Detail::RenderFrontendResourceAccess::BackendInstance(*impl_->frontend, impl_->viewport->TextureView().sampledView));
        const auto *final = reinterpret_cast<Detail::MetalTextureViewInstance *>(static_cast<std::uintptr_t>(finalId));
        REQUIRE(scene->format == RenderTextureFormat::Rgba16Float);
        REQUIRE(final->format == RenderTextureFormat::Rgba8Unorm);
        REQUIRE(scene->texture.width == Extent.width);
        REQUIRE(scene->texture.height == Extent.height);
        REQUIRE(final->texture.width == Extent.width);
        REQUIRE(final->texture.height == Extent.height);
        REQUIRE((static_cast<std::uint32_t>(scene->usage) & static_cast<std::uint32_t>(RenderTextureUsage::CopySource)) != 0);
        REQUIRE((static_cast<std::uint32_t>(final->usage) & static_cast<std::uint32_t>(RenderTextureUsage::CopySource)) != 0);
        const auto rawBytes = impl_->Copy(scene->texture, Extent.width * 8);
        const auto finalBytes = impl_->Copy(final->texture, Extent.width * 4);
        ViewportColorPixels result{.extent = Extent};
        result.rawScene.resize(rawBytes.size() / sizeof(std::uint16_t));
        result.encoded.resize(finalBytes.size());
        std::memcpy(result.rawScene.data(), rawBytes.data(), rawBytes.size());
        std::memcpy(result.encoded.data(), finalBytes.data(), finalBytes.size());
        return result;
    }
}  // namespace Horo::Editor::Test
