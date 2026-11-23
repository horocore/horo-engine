/** @file EditorVulkanPresentationSmoke.cpp
 * @brief Real SDL/Vulkan GUI and cooked viewport composition; no simulated native admission. */
#include "../../unit/runtime/renderer/UiSubmissionSourceFixture.h"
#include "CameraCutNativeTestSupport.h"
#include "ColorOutputPackage.h"
#include "EditorImageProviderHost.h"
#include "EditorNativeReconstructionReleaseLayout.h"
#include "EditorVulkanShaderAssets.h"
#include "Horo/Assets/AssetCookOutput.h"
#include "Horo/Foundation/Platform.h"
#include "Horo/Runtime/Render/RenderFrontend.h"
#include "Horo/Runtime/RuntimeHost.h"
#include "Horo/Runtime/Ui/UiRenderGeometry.h"
#include "Horo/Runtime/Ui/UiStyleAsset.h"
#include "RuntimeUiPaintPackagedKernel.h"
#include "RuntimeUiPaintProjectSources.h"
#include "VulkanBackendModule.h"
#include "VulkanEditorGraphicsBridge.h"
#include "VulkanVfxInstancedReadback.h"
#include "editor/renderer/vulkan/EditorGuiRendererVulkan.h"
#include "editor/renderer/vulkan/EditorViewportRendererVulkan.h"
#include "editor/renderer/vulkan/SdlVulkanPresentationPort.h"
#include "editor/renderer/vulkan/host/EditorVulkanXrViewportAccess.h"
#include "runtime/renderer/frontend/RenderFrontendResourceAccess.h"

#include <SDL3/SDL.h>
#include <algorithm>
#include <bit>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <chrono>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <imgui.h>
#include <iostream>
#include <memory>
#include <optional>
#include <string_view>
#include <thread>
#include <utility>

namespace Horo::Tests::VulkanEditor {
    using namespace Render;
    using namespace Editor;

    template <typename T> void Check(const Result<T> &result, const std::string_view operation = "native composition") {
        if (result.HasError()) {
            const auto &error = result.ErrorValue();
            INFO("Vulkan / SDL WSI / " << operation);
            INFO(error.domain.Value() << "/" << error.code.Value() << ": " << error.message);
            FAIL("Vulkan operation failed");
        }
    }

    /** @brief Keeps process/window initialization exception-safe before the complete host exists. */
    struct SdlContext {
        SdlContext() {
            REQUIRE(SDL_Init(SDL_INIT_VIDEO));
        }

        ~SdlContext() {
            SDL_Quit();
        }
    };

    /** @brief Test host retains the production native parents through every finite drain poll. */
    struct Host {
        SdlContext sdl;
        std::unique_ptr<SDL_Window, decltype(&SDL_DestroyWindow)> window{nullptr, &SDL_DestroyWindow};
        std::unique_ptr<SdlVulkanPresentationPort> port;
        std::unique_ptr<Test::VfxInstanced::ObservedLoader> viewportObservation;
        std::unique_ptr<Render::Detail::VulkanEditorGraphicsBridge> bridge;
        std::unique_ptr<RenderBackendRegistry> registry;
        std::unique_ptr<RenderFrontend> frontend;
        std::unique_ptr<EditorGuiRendererVulkan> gui;
        std::unique_ptr<EditorViewportRendererVulkan> viewport;
        bool drained{};
        RenderTargetHandle target;
        const FramebufferExtent extent{640, 480};
        static constexpr EditorViewportExtent CameraQualificationExtent{64, 32};

        explicit Host(bool compute = false, bool captureViewportColor = false) {
            window.reset(
                SDL_CreateWindow("Horo Vulkan qualification", extent.width, extent.height, SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE));
            REQUIRE(window);
            port = std::make_unique<SdlVulkanPresentationPort>(*window);
            if (captureViewportColor)
                viewportObservation = std::make_unique<Test::VfxInstanced::ObservedLoader>(*port);
            bridge = std::make_unique<Render::Detail::VulkanEditorGraphicsBridge>();
            registry = std::make_unique<RenderBackendRegistry>();
            auto &loader = viewportObservation ? static_cast<IVulkanLoaderPort &>(*viewportObservation) : *port;
            Check(RegisterVulkanRenderBackend(*registry, loader, bridge.get()));
            Check(registry->Seal());
            RenderBackendConfig config{.requirePresentation = true, .enableValidation = true, .maxFramesInFlight = 2};
            if (compute) {
                const auto graphics = std::ranges::find(config.queueAssignments, RenderQueueRole::Graphics, &RenderQueueAssignment::role);
                REQUIRE(graphics != config.queueAssignments.end());
                config.queueAssignments.push_back({RenderQueueRole::Compute, graphics->queue});
            }
            auto created = RenderFrontend::Create(*registry, RenderBackendId{"vulkan"}, config, {}, {.budget = {.maximumBlocks = 4096}});
            Check(created);
            frontend = std::move(created).Value();
            Check(frontend->Resize(extent));
            ImGui::CreateContext();
            ImGui::GetIO().IniFilename = nullptr;
            gui = std::make_unique<EditorGuiRendererVulkan>(*window, *frontend, *bridge);
            Check(gui->Initialize());
        }

        ~Host() {
            if (frontend && !Drain())
                std::terminate();
            ReleaseParents();
        }

        bool Drain() {
            if (drained)
                return true;
            if (frontend->BeginShutdown().HasError())
                return false;
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
            do {
                const auto progress = frontend->PollDrain();
                if (progress.HasError())
                    return false;
                if (progress.Value() == RenderBackendDrainProgress::Drained) {
                    drained = true;
                    return true;
                }
                // Explicit native qualification budget only; production never waits here.
                std::this_thread::sleep_for(std::chrono::milliseconds{1});
            } while (std::chrono::steady_clock::now() < deadline);
            return false;
        }

        void ReleaseParents() {
            viewport.reset();
            gui.reset();
            if (ImGui::GetCurrentContext())
                ImGui::DestroyContext();
            frontend.reset();
            // Registration factories also retain the native runtime in this test host.
            registry.reset();
            bridge.reset();
            viewportObservation.reset();
            port.reset();
            window.reset();
        }

        void RecordCapabilities() const {
            const auto borrowed = bridge->Port()->QueryDevice();
            Check(borrowed);
            const auto &device = borrowed.Value();
            const auto properties =
                std::bit_cast<PFN_vkGetPhysicalDeviceProperties>(device.instanceResolver(device.instance, "vkGetPhysicalDeviceProperties"));
            REQUIRE(properties);
            VkPhysicalDeviceProperties identity{};
            properties(device.physical, &identity);
            std::cout << "Vulkan device " << identity.deviceName << ", driver " << identity.driverVersion << ", API "
                      << VK_API_VERSION_MAJOR(identity.apiVersion) << '.' << VK_API_VERSION_MINOR(identity.apiVersion) << '.'
                      << VK_API_VERSION_PATCH(identity.apiVersion) << '\n';
            const auto query =
                std::bit_cast<PFN_vkGetPhysicalDeviceFeatures2>(device.instanceResolver(device.instance, "vkGetPhysicalDeviceFeatures2"));
            REQUIRE(query);
            VkPhysicalDeviceSwapchainMaintenance1FeaturesEXT maintenance{
                .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_EXT};
            VkPhysicalDeviceFeatures2 features{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &maintenance};
            query(device.physical, &features);
            REQUIRE(maintenance.swapchainMaintenance1 == VK_TRUE);
            std::cout << "{\"swapchainMaintenance1\":true,\"sameInstanceSurfaceAdmitted\":true,\"frames\":" << device.frames
                      << ",\"format\":" << static_cast<unsigned>(device.outputFormat) << "}\n";
        }

        void CreateViewport(const RenderSceneView &scene = {}, const EditorViewportColorInputs *color = nullptr,
                            const EditorSignedSpatialProvider *spatial = nullptr, bool crossFade = false) {
            const char *root = std::getenv("HORO_EDITOR_VULKAN_ASSET_ROOT");
            REQUIRE(root);
            auto cooked = LoadPackagedVulkanViewportShader(std::filesystem::path{root});
            Check(cooked);
            auto viewportCook = std::move(cooked).Value();
            if (crossFade) {
                auto blend = LoadPackagedVulkanCameraCrossFadeShader(std::filesystem::path{root});
                Check(blend, "load exact camera cross-fade material");
                viewportCook.cameraCrossFade.emplace(std::move(blend).Value());
            }
            viewport = std::make_unique<EditorViewportRendererVulkan>(*frontend, *bridge, std::move(viewportCook));
            Check(viewport->Initialize(), "initialize viewport");
            if (crossFade) {
                auto shadow = DirectionalShadowProfile{};
                shadow.cascadeCount = 1;
                Check(viewport->RequestDirectionalShadowProfile(shadow), "select bounded cross-fade shadow profile");
                Check(viewport->EnableCameraCrossFade(), "enable cooked camera cross-fade");
            }
            if (color) {
                auto linear = spatial ? LoadEditorVulkanSceneLinearCook(spatial->scenePayload, spatial->sceneReflection)
                                      : LoadPackagedVulkanSceneLinearShader(std::filesystem::path{root});
                Check(linear, "load exact scene-linear material");
                Check(viewport->PrepareManualColor(std::move(linear).Value(), *color, {}, spatial ? &spatial->kernel : nullptr),
                      "activate the selected Vulkan viewport color recipe");
            }
            const auto view = frontend->CreateRenderViewIdentity();
            Check(view);
            Check(viewport->BindLightingViewIdentity(view.Value()));
            EditorViewportLightingProfile lighting;
            lighting.requested = LightPreparationMode::BaselineForwardCpu;
            const auto selected = frontend->ResolveLightPreparation(lighting.requested, lighting.fallback, {});
            Check(selected);
            Check(viewport->RequestLightingProfile(lighting, selected.Value()));
            const auto size = crossFade ? CameraQualificationExtent : EditorViewportExtent{320, 240};
            viewport->RequestExtent(size);
            viewport->RequestGrid({.visible = false});
            SynchronizeViewport(scene, size);
        }

        void SynchronizeViewport(const RenderSceneView &scene, const EditorViewportExtent requested = {320, 240}) {
            for (std::size_t attempt = 0; attempt < 8; ++attempt) {
                const auto prepared = viewport->PrepareResources(*frontend, scene);
                Check(prepared, "prepare exact viewport resources");
                Check(frontend->ProcessResourceRequests());
                if (prepared.Value().has_value() && viewport->RequestedExtent() == requested) {
                    target = *prepared.Value();
                    return;
                }
            }
            FAIL("Viewport resources did not become ready within the bounded preparation budget");
        }

        RenderFrameScope Frame(const std::uint64_t number) {
            return Frame(number, extent);
        }

        RenderFrameScope Frame(const std::uint64_t number, const FramebufferExtent output) {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
            do {
                const auto available = frontend->PollPresentationAvailability(output);
                Check(available, "poll swapchain acquisition");
                if (available.Value()) {
                    auto frame = frontend->BeginFrame({number, output});
                    Check(frame, "begin acquired frame");
                    return std::move(frame).Value();
                }
                SDL_PumpEvents();
                std::this_thread::sleep_for(std::chrono::milliseconds{1});
            } while (std::chrono::steady_clock::now() < deadline);
            FAIL("Vulkan / SDL WSI / poll swapchain acquisition: no retired slot ready within 5 seconds");
            std::terminate();
        }

        StaticMeshPassDescriptor ViewportPass(const RenderSceneView &scene) const {
            const auto size = viewport->RequestedExtent();
            return {.target = target, .extent = {size.width, size.height}, .scene = scene};
        }

        void PrepareScene(const RenderSceneView &scene = {}) {
            Check(viewport->PrepareLightingFrame(ViewportPass(scene), viewport->LightingViewIdentity()), "prepare viewport graph");
        }

        void ExecuteSceneAndGui(RenderFrameScope &frame, const RenderSceneView &scene = {}) {
            const std::array passes{RenderPassDescriptor{.kind = RenderPassKind::Graphics, .staticMesh = ViewportPass(scene)},
                                    RenderPassDescriptor{.kind = RenderPassKind::Graphics, .primaryOutput = PrimaryOutputAttachment{}}};
            Check(viewport->ExecutePreparedFrame(frame, passes));
            const auto view = viewport->TextureView();
            REQUIRE(view.IsValid());
            ComposeGui(frame, &view);
        }

        void ExecuteGuiOnly(RenderFrameScope &frame) {
            Check(bridge->Port()->PreparePrimaryOutput(frame.Token(), {}), "prepare acquired SDR output");
            Check(frame.Execute({}), "admit the frame for GUI-only submission");
            ComposeGui(frame, nullptr);
        }

        void ComposeGui(RenderFrameScope &frame, const EditorViewportTextureView *view) {
            Check(gui->BeginFrame());
            ImGui::NewFrame();
            if (view) {
                ImGui::GetForegroundDrawList()->AddImage(static_cast<ImTextureID>(view->textureId), {0, 0}, {320, 240});
                gui->BindViewportTexture(*view);
            }
            ImGui::GetForegroundDrawList()->AddText({8, 8}, IM_COL32_WHITE, "Canonical Vulkan GUI tail");
            ImGui::Render();
            Check(frame.ComposeGui(*gui));
        }
    };

    /** @brief Polls renderer-owned staging through the ordinary cold resource boundary. */
    EditorViewportImageSnapshot WaitForViewportCrop(Host &host, const RenderSceneView &scene, std::uint64_t id,
                                                    EditorViewportExtent extent = {320, 240}) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
        do {
            host.SynchronizeViewport(scene, extent);
            auto image = host.viewport->ReadImageCapture(id);
            REQUIRE(image);
            if (image->phase != EditorViewportImagePhase::Pending)
                return std::move(*image);
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        } while (std::chrono::steady_clock::now() < deadline);
        FAIL("The exact viewport crop did not complete.");
        std::terminate();
    }

    /** @brief Reads the exact published viewport attachment after a real native frame. */
    std::vector<std::uint8_t> ReadViewportColor(Host &host, const RenderSamplerHandle readbackSampler) {
        using ResourceAccess = Render::Detail::RenderFrontendResourceAccess;
        const auto view = ResourceAccess::BackendInstance(*host.frontend, host.viewport->TextureView().sampledView);
        const auto sampler = ResourceAccess::BackendInstance(*host.frontend, readbackSampler);
        Check(view, "resolve viewport GUI texture");
        Check(sampler, "resolve readback sampler");
        const auto sample = host.bridge->Port()->BorrowSample({view.Value(), sampler.Value(), 0});
        Check(sample, "borrow exact native viewport GUI sample");
        using Readback = Test::VfxInstanced::NativeReadback;
        Readback readback{*host.viewportObservation, Readback::Capture::ViewportColor};
        readback.Prepare();
        return readback.ReadViewportColor(sample.Value().view);
    }

    /** @brief Uses the ordinary scene and GUI tail, then reads the exact production color attachment. */
    std::vector<std::uint8_t> PresentCameraColor(Host &host, const RenderSceneView &scene, const std::uint64_t frameId,
                                                 const RenderSamplerHandle readbackSampler,
                                                 const std::optional<FramebufferExtent> output = std::nullopt) {
        REQUIRE(host.viewportObservation);
        host.viewportObservation->ResetDraws();
        host.PrepareScene(scene);
        auto frame = host.Frame(frameId, output.value_or(host.extent));
        host.ExecuteSceneAndGui(frame, scene);
        Check(frame.Present());
        return ReadViewportColor(host, readbackSampler);
    }

    /** @brief Authors the two real orthographic viewpoints used by the native image oracle. */
    std::unique_ptr<Runtime::RuntimeScene> CapacityCameraScene(const RenderCameraView &source, const RenderCameraView &destination) {
        Runtime::SceneDefinitionBuilder builder{{1}, {1}};
        const std::array views{source, destination};
        for (std::size_t index = 0; index < views.size(); ++index) {
            Runtime::RuntimeEntityDefinition entity;
            entity.object = {index + 1};
            entity.localTransform.translation = views[index].position;
            entity.components.camera = Runtime::CameraComponent{.projection = Runtime::CameraProjection::Orthographic,
                                                                .orthographicHeight = views[index].projection.orthographicHeight,
                                                                .nearPlane = views[index].projection.nearPlane,
                                                                .farPlane = views[index].projection.farPlane};
            builder.Add(entity);
        }
        return CameraCuts::Take(Runtime::RuntimeScene::Create(CameraCuts::Take(std::move(builder).Build()), {1}));
    }

    /** @brief Owns the illuminated triangle and exact orthographic endpoints used by native camera pixel cases. */
    struct CameraViewportFixture final {
        std::array<MeshVertex, 3> vertices{MeshVertex{{-1, -1, 0}, {0, 0, 1}, {}}, MeshVertex{{1, -1, 0}, {0, 0, 1}, {}},
                                           MeshVertex{{0, 1, 0}, {0, 0, 1}, {}}};
        std::array<std::uint32_t, 3> indices{0, 1, 2};
        RenderMeshSourceHandle mesh{{1}, 1};
        std::array<RenderMeshResourceView, 1> meshes{RenderMeshResourceView{mesh, vertices, indices, {{-1, -1, 0}, {1, 1, 0}}}};
        std::array<RenderStaticMeshInstance, 1> instances{
            RenderStaticMeshInstance{.mesh = mesh, .presentation = {.tint = {1, 0, 1}, .tintStrength = 1}}};
        std::array<RenderLight, 1> lights{RenderLight{.direction = {0, 0, -1}, .stableIdentity = 1}};

        [[nodiscard]] RenderSceneView View(const float x) const noexcept {
            return {.camera = {.position = {x, 0, 5},
                               .target = {x, 0, 0},
                               .projection = {.kind = RenderProjectionKind::Orthographic, .orthographicHeight = 4}},
                    .meshResources = meshes,
                    .instances = instances,
                    .lights = lights,
                    .lightSceneDomain = 1,
                    .lightSnapshotRevision = 1};
        }
    };

    /** @brief Counts the fixture's opaque magenta pixels independently of the clear color and destination alpha. */
    std::size_t CountCameraTint(const std::span<const std::uint8_t> pixels) {
        std::size_t count{};
        for (std::size_t index = 0; index + 3 < pixels.size(); index += 4)
            count += pixels[index] >= 240 && pixels[index + 1] <= 8 && pixels[index + 2] >= 240;
        return count;
    }

    TEST_CASE("Native Vulkan viewport samples Camera and World canvases with exact scene depth",
              "[native-smoke][vulkan][gui][viewport][canvas]") {
        using namespace Runtime::Ui;
        using Render::Test::UiOwner;
        using Render::Test::UiRequire;
        const auto *root = std::getenv("HORO_VULKAN_UI_PAINT_ASSET_ROOT");
        if (!root || !std::getenv("HORO_EDITOR_VULKAN_ASSET_ROOT"))
            SKIP("Supply locked native Vulkan viewport and canvas shader packages.");
        const auto consumer = UiRequire(LoadPackagedVulkanWorldCanvasShader(root));
        const auto writer = UiRequire(Application::Internal::LoadPackagedProjectUiVulkanSceneKernel(root, ShaderTargetBackend::Vulkan, {}));
        const CameraViewportFixture fixture;
        const auto scene = fixture.View(0);
        Host host{false, true};
        host.CreateViewport(scene, nullptr, nullptr, true);
        const auto pass = host.ViewportPass(scene);
        const auto sampler = UiRequire(host.frontend->CreateSampler({})).handle;
        Check(host.frontend->ProcessResourceRequests());
        const auto tree = Render::Test::UiTree();
        const UiRenderViewId view{UiOwner(), 9, 1};
        const UiRenderSnapshotLimits limits{1, 0, 0, 0, 0, 1, 0};
        auto extractor = UiRequire(UiRenderExtractor::Create({view, limits, 1}));
        auto arena = UiRequire(UiRenderGeometryArena::Create({view, {4, 6, 1}, 1}));
        const UiResolvedScreenCanvas projection{.logicalExtent = {4096, 2048}, .pixelExtent = {64, 32}, .pixelsPerDip = {1, 1}};
        const auto descriptor = Runtime::Ui::Test::SnapshotDescriptor(tree, UiRequire(UiInteractionRevision::Create(1)),
                                                                      UiRequire(UiRenderSnapshotRevision::Create(1)), view, limits);
        const std::array commands{UiDrawCommand{UiRequire(tree.Root()).handle,
                                                {{0, 0}, {4096, 2048}},
                                                0,
                                                NoUiRenderIndex,
                                                NoUiRenderIndex,
                                                1,
                                                UiSolidDraw{{0, 1, 0, 1}}}};
        const std::array transforms{UiLogicalTransform{}};
        const auto snapshot = UiRequire(extractor.Extract(tree, descriptor, {.commands = commands, .transforms = transforms}));
        const auto geometry = UiRequire(arena.Build(snapshot, projection));
        const auto center = (pass.extent.height / 2 * pass.extent.width + pass.extent.width / 2) * 4;
        const auto centerPixel = [&](const std::vector<std::uint8_t> &pixels) {
            REQUIRE(center + 2 < pixels.size());
            return std::array{int{pixels[center]}, int{pixels[center + 1]}, int{pixels[center + 2]}};
        };
        const auto opaque = centerPixel(PresentCameraColor(host, scene, 1, sampler));
        CAPTURE(opaque);
        REQUIRE(opaque[0] > 240);

        using enum UiRenderCompositionSpace;
        const auto space = GENERATE(Camera, World);
        const UiPaintProgramDescriptor paint{projection, {32, 64, 4}, 2, 32};
        const EditorViewportRendererVulkan::ViewportCanvasPreparation prepared{paint, snapshot,         {}, *writer, consumer,
                                                                               space, {16, 64, 192, 32}};
        Check(host.viewport->PrepareViewportCanvas(pass, prepared));
        const UiPaintProgram *programIdentity{};
        const auto render = [&](const std::uint64_t number, const std::optional<Math::Mat4> &placement) {
            auto frame = host.Frame(number);
            const std::array passes{RenderPassDescriptor{.id = {1}, .kind = RenderPassKind::Graphics, .staticMesh = pass},
                                    RenderPassDescriptor{.id = {2},
                                                         .kind = RenderPassKind::Graphics,
                                                         .primaryOutput = PrimaryOutputAttachment{}}};
            Check(host.viewport->ExecutePreparedCanvasFrame(frame, passes, placement, [&](const auto &program, std::uint32_t) {
                if (programIdentity)
                    CHECK(programIdentity == &program);
                programIdentity = &program;
                return Result<UiRenderSubmission>::Success({.geometry = &geometry});
            }));
            const auto output = host.viewport->TextureView();
            host.ComposeGui(frame, &output);
            Check(frame.Present());
            host.viewport->FinishColorFrame(true);
            return centerPixel(ReadViewportColor(host, sampler));
        };
        auto placement = Math::Mat4::Identity();
        placement.values[14] = 1;
        const auto front = render(2, space == World ? std::optional{placement} : std::nullopt);
        CHECK(front[1] > 240);
        CHECK(front[0] < 8);
        if (space == World) {
            placement.values[14] = -1;
            Check(host.viewport->PrepareViewportCanvas(pass, prepared));
            const auto behind = render(3, placement);
            CHECK(behind == opaque);
        }
        const EditorViewportImageRegion region{pass.extent.width / 2, pass.extent.height / 2, 1, 1};
        const bool revokeBeforeRecording = GENERATE(false, true);
        bool authorized = true;
        const auto capture = UiRequire(host.viewport->RequestImageCapture(region, [&] {
            return authorized;
        }));
        host.SynchronizeViewport(scene, Host::CameraQualificationExtent);
        Check(host.viewport->PrepareViewportCanvas(pass, prepared));
        authorized = !revokeBeforeRecording;
        programIdentity = nullptr;
        placement.values[14] = 1;
        const auto composited = render(4, space == World ? std::optional{placement} : std::nullopt);
        const auto image = WaitForViewportCrop(host, scene, capture, Host::CameraQualificationExtent);
        if (revokeBeforeRecording) {
            CHECK(image.phase == EditorViewportImagePhase::Cancelled);
            CHECK_FALSE(image.artifact);
        } else {
            REQUIRE(image.phase == EditorViewportImagePhase::Ready);
            REQUIRE(image.artifact);
            CHECK(image.artifact->region == region);
            REQUIRE(image.artifact->rgba.size() == region.Bytes());
            const auto &rgba = image.artifact->rgba;
            CHECK((std::array{std::to_integer<int>(rgba[0]), std::to_integer<int>(rgba[1]), std::to_integer<int>(rgba[2])} == composited));
        }
        CHECK(composited == front);
        Check(host.frontend->ReleaseSampler(sampler));
        CHECK(host.Drain());
    }

    /** @brief Publishes a cooked project asset and opens its authored canvas via the actual Scene participant. */
    TEST_CASE("Native Vulkan prepares both XR canvas eye passes with or without paired motion targets",
              "[native-smoke][vulkan][xr][canvas]") {
        using namespace Runtime::Ui;
        using Viewport = EditorVulkanXrViewportAccess;
        using Render::Test::UiOwner;
        using Render::Test::UiRequire;
        const auto *uiRoot = SDL_getenv("HORO_VULKAN_UI_PAINT_ASSET_ROOT");
        const auto *xrRoot = SDL_getenv("HORO_VULKAN_XR_ASSET_ROOT");
        if (!uiRoot || !xrRoot || !SDL_getenv("HORO_EDITOR_VULKAN_ASSET_ROOT"))
            SKIP("Supply the locked viewport, multiview and canvas shader packages.");
        const bool paired = GENERATE(false, true);

        const auto consumer = UiRequire(LoadPackagedVulkanWorldCanvasShader(uiRoot));
        const auto writer =
            UiRequire(Application::Internal::LoadPackagedProjectUiVulkanSceneKernel(uiRoot, ShaderTargetBackend::Vulkan, {}));
        const CameraViewportFixture fixture;
        const auto scene = fixture.View(0);
        Host host;
        host.CreateViewport(scene);
        auto projectionProgram = UiRequire(LoadPackagedVulkanXrProjection(xrRoot));
        Check(host.viewport->PrepareXrProgram(std::move(projectionProgram.cook), projectionProgram.program));
        const auto device = UiRequire(Viewport::Device(*host.viewport));
        constexpr FramebufferExtent eyeAllocation{64, 32};
        const RenderTextureDescriptor color{.extent = eyeAllocation,
                                            .format = RenderTextureFormat::Rgba8Unorm,
                                            .layerCount = 2,
                                            .usage = RenderTextureUsage::RenderAttachment | RenderTextureUsage::Sampled};
        const RenderGraphLogicalState acquire{RenderGraphSynchronizationAccess::ReadWrite,
                                              RenderGraphSynchronizationOperation::ColorAttachment, RenderGraphPipelineScope::Graphics,
                                              RenderGraphTextureLayout::ColorAttachment, device.graphicsIdentity};
        auto release = acquire;
        release.scope = RenderGraphPipelineScope::External;
        const auto tree = Render::Test::UiTree();
        const UiRenderViewId view{UiOwner(), 9, 1};
        const UiRenderSnapshotLimits limits{1, 0, 0, 0, 0, 1, 0};
        auto extractor = UiRequire(UiRenderExtractor::Create({view, limits, 1}));
        const auto descriptor = Runtime::Ui::Test::SnapshotDescriptor(tree, UiRequire(UiInteractionRevision::Create(1)),
                                                                      UiRequire(UiRenderSnapshotRevision::Create(1)), view, limits);
        const std::array commands{UiDrawCommand{UiRequire(tree.Root()).handle,
                                                {{0, 0}, {4096, 2048}},
                                                0,
                                                NoUiRenderIndex,
                                                NoUiRenderIndex,
                                                1,
                                                UiSolidDraw{{0, 1, 0, 1}}}};
        const std::array transforms{UiLogicalTransform{}};
        const auto snapshot = UiRequire(extractor.Extract(tree, descriptor, {.commands = commands, .transforms = transforms}));
        const UiResolvedScreenCanvas projection{.logicalExtent = {4096, 2048}, .pixelExtent = {64, 32}, .pixelsPerDip = {1, 1}};
        const UiPaintProgramDescriptor paint{projection, {32, 64, 4}, 1, 32};
        const Viewport::CanvasPreparation canvas{paint, snapshot, {}, *writer, consumer};
        if (paired) {
            if (!std::filesystem::exists(std::filesystem::path{xrRoot} / "shaders/runtime/multiview-motion-vulkan.horoshdr"))
                SKIP("Supply the locked multiview motion shader package for paired preparation.");
            host.viewport->StageXrMotionProgram(UiRequire(LoadPackagedVulkanMultiviewMotionShader(xrRoot)));
            auto motionColor = color;
            motionColor.format = RenderTextureFormat::Rgba16Float;
            auto motionDepth = color;
            motionDepth.format = RenderTextureFormat::Depth32Float;
            auto acquireDepth = acquire;
            acquireDepth.operation = RenderGraphSynchronizationOperation::DepthStencilAttachment;
            acquireDepth.layout = RenderGraphTextureLayout::DepthStencilAttachment;
            auto releaseDepth = acquireDepth;
            releaseDepth.scope = RenderGraphPipelineScope::External;
            REQUIRE(UiRequire(Viewport::PrepareMotionPipeline(*host.viewport, motionColor.format, motionDepth.format)));
            const Viewport::AuxiliaryPlan auxiliary{{motionColor, acquire, release},
                                                    {motionDepth, acquireDepth, releaseDepth},
                                                    {eyeAllocation, eyeAllocation}};
            const auto prepared =
                UiRequire(Viewport::PreparePairedTargets(*host.viewport, scene.meshResources, color, acquire, release, auxiliary, 0));
            CHECK(Viewport::IsPrepared(*host.viewport, prepared));
        } else {
            Check(Viewport::Prepare(*host.viewport, scene.meshResources, color, acquire, release, false));
        }
        REQUIRE(UiRequire(Viewport::PrepareCanvas(*host.viewport, scene.meshResources, canvas)));
        Check(Viewport::Retire(*host.viewport));
        CHECK(host.Drain());
    }

    struct AuthoredViewportCanvas final {
        using Port = Application::Internal::RuntimeUiPaintPort;

        Assets::AssetRegistry registry;
        Assets::MemoryAssetProvider provider;
        JobSystem jobs{JobSystemConfig{2, 32}};
        Assets::AssetLoadService assetLoads{jobs, provider};
        Runtime::Ui::UiRuntimeAssetLoadService uiLoads{registry, assetLoads};
        Runtime::RuntimeSceneService scenes{registry, assetLoads, {}, {100}};
        Runtime::Ui::UiCanvasAssetReference reference;

        AuthoredViewportCanvas(Port port, const std::filesystem::path &packages, const Runtime::Ui::UiRenderMode mode) {
            using namespace Runtime::Ui;
            using namespace Application::Internal;
            using Render::Test::UiAsset;
            using Render::Test::UiBytes;
            using Render::Test::UiOwner;
            using Render::Test::UiRequire;
            const auto asset = UiAsset(91);
            const auto kind = UiRequire(Assets::AssetTypeId::Parse("runtime.ui.document"));
            const auto target = UiRequire(Assets::AssetCookTargetId::Parse("headless-null"));
            reference = {asset, UiRequire(UiDocumentId::Create(UiBytes(1))), UiRequire(UiCanvasId::Create(UiBytes(2))),
                         UiRequire(UiDocumentRevision::Create(1))};
            const auto root = UiRequire(UiElementId::Create(UiBytes(3)));
            const auto panel = UiRequire(UiElementId::Create(UiBytes(4)));
            UiDocumentBuilder document{reference.document, reference.minimumRevision};
            Check(document.AddCanvas({.id = reference.canvas, .rootElement = root, .renderMode = mode, .referenceResolution = {64, 32}}));
            auto canvas = UiRequire(CreateUiElement(UiElementKind::Canvas, root, {}));
            canvas.presentation->layout.width = UiLength::Percent(64);
            canvas.presentation->layout.height = UiLength::Percent(64);
            Check(document.AddElement(std::move(canvas)));
            auto surface = UiRequire(CreateUiElement(UiElementKind::Panel, panel, root));
            surface.presentation->layout.width = UiLength::Percent(64);
            surface.presentation->layout.height = UiLength::Percent(64);
            surface.presentation->inlineStyle.push_back(
                {CoreUiStylePropertyId(UiCoreStyleProperty::Fill), UiStyleValueSource::Literal(UiStyleColor{0, 1, 0, 1}), false});
            Check(document.AddElement(std::move(surface)));
            const auto compiled = UiRequire(CookedUiDocument::Cook(UiRequire(std::move(document).Build())));
            Assets::AssetCookArtifact artifact{.id = asset, .type = kind, .target = target};
            artifact.payload.assign(compiled.Payload().begin(), compiled.Payload().end());
            artifact.payloadDigest = ComputeSha256(std::as_bytes(std::span{artifact.payload}));
            provider.Insert(asset, UiRequire(Assets::EncodeCookedArtifact(artifact)));
            REQUIRE(registry
                        .Publish({{asset, kind, UiRequire(ProjectPath::Parse("assets/canvas.ui")),
                                   UiRequire(ProjectPath::Parse("assets/canvas.ui.horo"))}})
                        .status == Assets::AssetRegistryBuildStatus::Complete);
            auto allocator = std::make_shared<UiElementSlotAllocator>(UiRequire(UiElementSlotAllocator::Create(UiOwner())));
            auto sources =
                UiRequire(RuntimeUiPaintProjectSources::Create(port, std::move(allocator), {.vulkanSceneCanvasAssets = packages}));
            Check(scenes.AddActivationParticipant(sources->MakeParticipant(scenes, registry, uiLoads, target)));
            Check(scenes.Startup({}));
            Publish(1, 1);
        }

        /** @brief Replaces the complete authored scene at the checked owner safe point. */
        void Publish(const std::uint64_t revision, const float z) {
            using Render::Test::UiRequire;
            Runtime::SceneDefinitionBuilder scene{{5}, {revision}};
            Runtime::RuntimeEntityDefinition entity{.object = {1}, .localTransform = {.translation = {0, 0, z}}};
            entity.components.uiCanvas = Runtime::UiCanvasComponent{reference};
            scene.Add(std::move(entity));
            Check(scenes.QueuePreparation(UiRequire(std::move(scene).Build())));
            const CancellationToken cancellation;
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
            while (std::chrono::steady_clock::now() < deadline) {
                Check(scenes.OnPhase(Runtime::RuntimePhase::CommitDeferredLifecycleChanges,
                                     {.frameNumber = revision, .cancellation = cancellation}));
                if (const auto failed = scenes.TakeOperationError())
                    Check(Result<void>::Failure(*failed), "publish authored project and canvas");
                const auto current = scenes.ActiveScene();
                if (current && current->DefinitionRevision() == Runtime::SceneDefinitionRevision{revision})
                    return;
                std::this_thread::sleep_for(std::chrono::milliseconds{1});
            }
            FAIL("Authored Scene canvas did not reach its publication deadline");
        }
    };

    /** @brief Drives one test render callback at the same phase as the canonical editor consumer. */
    class AuthoredCanvasRenderer final : public Runtime::RuntimeLifecycleParticipant {
    public:
        explicit AuthoredCanvasRenderer(std::function<void(std::uint64_t)> draw) : draw_(std::move(draw)) {}

        Result<void> Startup(const CancellationToken &) override {
            return Result<void>::Success();
        }

        Result<void> OnFixedUpdate(const Runtime::FixedStepContext &) override {
            return Result<void>::Success();
        }

        void Shutdown() noexcept override {}

        Result<void> OnPhase(const Runtime::RuntimePhase phase, const Runtime::FrameContext &context) override {
            if (phase == Runtime::RuntimePhase::RenderExecution)
                draw_(context.frameNumber);
            return Result<void>::Success();
        }

    private:
        std::function<void(std::uint64_t)> draw_;
    };

    /** @brief Drives admitted ray samples through the exact application World pointer port. */
    class AuthoredWorldPointer final : public Runtime::Ui::UiEventHandler {
    public:
        using Port = Application::Internal::RuntimeUiPaintPort;
        using Presented = Application::Internal::RuntimeUiPaintPresentedWorldCanvas;

        void Press(const Port &port, Runtime::SceneRuntimeId scene, const Presented &target, const Math::Ray &ray) {
            const auto update = [&](bool down, std::uint64_t time) {
                const auto result = pointer_.Update(port, scene, target, ray, down, time, *this);
                Check(result, "World pointer");
                return result.Value().activePointers;
            };
            REQUIRE(update(true, 1) == 0);
            CHECK(update(false, 2) == 0);
            CHECK_FALSE(Render::Test::UiRequire(port.TryDequeueWorldFeedback(scene)));
            CHECK(update(true, 3) == 1);
            const auto cue = Render::Test::UiRequire(port.TryDequeueWorldFeedback(scene));
            REQUIRE(cue);
            CHECK(cue->kind == Runtime::Ui::UiFeedbackKind::Focus);
            CHECK(cue->source.owner.instance == target.instance);
            CHECK(cue->source.owner.interaction == target.presentation.interactionRevision);
            CHECK(cue->focusScope.has_value());
            const Port unavailable;
            const auto deferred = pointer_.Cancel(unavailable);
            REQUIRE(deferred.HasError());
            CHECK(deferred.ErrorValue().code.Value() == Runtime::Ui::UiErrors::EventDispatchLifecycleUnavailable.code.Value());
            CHECK(update(true, 4) == 0);
            CHECK(update(false, 5) == 0);
            CHECK(update(true, 6) == 1);
            CHECK(pressed_);
            CHECK(pressCount_ == 2);
            CHECK_FALSE(Render::Test::UiRequire(port.TryDequeueWorldFeedback(scene)));
            lastRay_ = ray;
        }

        void VerifyUnavailable(const Port &port, Runtime::SceneRuntimeId scene, const Presented &previous) {
            const auto blocked = pointer_.Update(port, scene, previous, lastRay_, true, 7, *this);
            REQUIRE(blocked.HasError());
            CHECK(blocked.ErrorValue().code.Value() == Runtime::Ui::UiErrors::HitTestNotPresented.code.Value());
            Check(pointer_.Cancel(port));
            CHECK_FALSE(Render::Test::UiRequire(port.TryDequeueWorldFeedback(scene)));
        }

        [[nodiscard]] bool Pressed() const noexcept {
            return pressed_;
        }

        Result<Runtime::Ui::UiEventResponse> Handle(Runtime::Ui::UiElementHandle, const Runtime::Ui::UiEventPhase phase,
                                                    const Runtime::Ui::UiRoutedEvent &event) override {
            if (phase == Runtime::Ui::UiEventPhase::Target && event.gesture && event.gesture->kind == Runtime::Ui::UiGestureKind::Press) {
                pressed_ = true;
                ++pressCount_;
            }
            return Result<Runtime::Ui::UiEventResponse>::Success({});
        }

        Result<void> ApplyDefault(Runtime::Ui::UiElementHandle, const Runtime::Ui::UiRoutedEvent &) override {
            return Result<void>::Success();
        }

    private:
        Application::Internal::RuntimeUiPaintWorldPointer pointer_;
        Math::Ray lastRay_{};
        std::uint32_t pressCount_{};
        bool pressed_{};
    };

    TEST_CASE("Native Vulkan presents an authored project canvas before admitting its interaction",
              "[native-smoke][vulkan][gui][viewport][authored-canvas]") {
        using namespace Runtime::Ui;
        using namespace Application::Internal;
        using Render::Test::UiOwner;
        using Render::Test::UiRequire;
        const auto *root = std::getenv("HORO_VULKAN_UI_PAINT_ASSET_ROOT");
        if (!root || !std::getenv("HORO_EDITOR_VULKAN_ASSET_ROOT"))
            SKIP("Supply locked native Vulkan viewport and canvas shader packages.");
        const auto consumer = UiRequire(LoadPackagedVulkanWorldCanvasShader(root));
        using CanvasRoute = std::tuple<UiRenderMode, bool>;
        const auto [mode, deferHeadsetReceipt] = GENERATE(
            table<UiRenderMode, bool>({CanvasRoute{UiRenderMode::ScreenSpaceCamera, false}, CanvasRoute{UiRenderMode::WorldSpace, false},
                                       CanvasRoute{UiRenderMode::WorldSpace, true}}));
        const CameraViewportFixture fixture;
        Host host{false, true};
        DeterministicClock clock;
        auto runtime = UiRequire(Runtime::RuntimeHost::Create(clock));
        auto paint = UiRequire(RuntimeUiPaintHost::Compose(*host.frontend, runtime->DispatchSource(), {UiOwner()}));
        const auto port = paint.port;
        Check(port.UpdateOutput({{host.extent.width, host.extent.height}, {1, 1}, {}}));
        Check(port.UpdateCameraOutput({Host::CameraQualificationExtent.width, Host::CameraQualificationExtent.height}));
        AuthoredViewportCanvas project{port, root, mode};
        auto scene = fixture.View(0);
        auto sceneId = project.scenes.ActiveScene()->RuntimeId();
        scene.lightSceneDomain = sceneId.value;
        host.CreateViewport(scene, nullptr, nullptr, true);
        REQUIRE(host.viewport->SupportsViewportCanvas());
        const auto sampler = UiRequire(host.frontend->CreateSampler({})).handle;
        Check(host.frontend->ProcessResourceRequests());
        Check(runtime->AddParticipant(std::move(paint.participant)));
        UiRenderViewId view;
        bool draw = true;
        bool verifyInitialAdmission = true;
        bool headsetProjected = true;
        const auto worldPointer = std::make_unique<AuthoredWorldPointer>();
        std::optional<RuntimeUiPaintPresentedWorldCanvas> previousWorld;
        Check(runtime->AddParticipant(std::make_unique<AuthoredCanvasRenderer>([&](const std::uint64_t number) {
            std::optional<RuntimeUiPaintHeadsetCanvasReceipt> headsetReceipt;
            RuntimeUiInstanceId headsetInstance;
            const auto pass = host.ViewportPass(scene);
            host.PrepareScene(scene);
            if (draw) {
                const auto source = UiRequire(port.ReadSceneCanvasFrame(number, sceneId));
                REQUIRE(source);
                view = source->prototype->Descriptor().view;
                if (deferHeadsetReceipt && worldPointer->Pressed() && headsetProjected && !verifyInitialAdmission)
                    CHECK(std::ranges::any_of(source->prototype->Commands(), [](const UiDrawCommand &command) {
                        const auto *cue = std::get_if<UiVisualIndicatorDraw>(&command.payload);
                        return cue && cue->state.Contains(UiVisualStateMask{UiVisualState::Focused});
                    }));
                if (verifyInitialAdmission)
                    CHECK_FALSE(port.InputEligible(view));
                const auto space = source->worldCanvas ? UiRenderCompositionSpace::World : UiRenderCompositionSpace::Camera;
                std::optional<UiWorldCanvasProjection> plane;
                std::optional<Math::Mat4> transform;
                if (source->worldCanvas) {
                    const auto observer = source->worldCanvas->currentScene.lock();
                    REQUIRE(observer);
                    const auto current = (*observer)();
                    REQUIRE(current);
                    plane = UiRequire(CaptureRuntimeUiPaintWorldCanvasPlane(*current, *source->worldCanvas));
                    transform = UiRequire(BuildRuntimeUiPaintWorldCanvasTransform(*plane));
                }
                Check(host.viewport->PrepareViewportCanvas(pass, {*source->program,
                                                                  *source->prototype,
                                                                  source->textures,
                                                                  *source->kernel,
                                                                  consumer,
                                                                  space,
                                                                  {32, 128, 512, 128}}));
                auto frame = host.Frame(number);
                const std::array passes{RenderPassDescriptor{.kind = RenderPassKind::Graphics, .staticMesh = pass},
                                        RenderPassDescriptor{.kind = RenderPassKind::Graphics, .primaryOutput = PrimaryOutputAttachment{}}};
                Check(host.viewport->ExecutePreparedCanvasFrame(frame, passes, transform, [&](const auto &program, std::uint32_t slot) {
                    return port.TakeSceneCanvasSubmission(source->instance, number, program, slot);
                }));
                if (deferHeadsetReceipt) {
                    REQUIRE(plane);
                    headsetInstance = source->instance;
                    headsetReceipt = UiRequire(port.DetachHeadsetCanvas(headsetInstance, number, *plane));
                } else {
                    Check(port.SceneCanvasEncoded(source->instance, number, plane));
                }
                if (verifyInitialAdmission)
                    CHECK_FALSE(port.InputEligible(view));
                const auto output = host.viewport->TextureView();
                host.ComposeGui(frame, &output);
                Check(frame.Present());
            } else {
                auto frame = host.Frame(number);
                host.ExecuteSceneAndGui(frame, scene);
                Check(frame.Present());
            }
            Check(port.Presented(number, true));
            if (headsetReceipt) {
                if (verifyInitialAdmission) {
                    CHECK_FALSE(port.InputEligible(view));
                    CHECK_FALSE(UiRequire(port.ReadPresentedWorldCanvas(sceneId)));
                }
                Check(port.HeadsetCanvasPresented(headsetInstance, *headsetReceipt, headsetProjected));
                const auto hit = UiRequire(port.ReadPresentedWorldCanvas(sceneId));
                CHECK(hit.has_value() == headsetProjected);
                if (hit) {
                    CHECK(hit->instance == headsetInstance);
                    CHECK(hit->presentation.view == view);
                    CHECK(hit->hit.Descriptor().renderMode == UiRenderMode::WorldSpace);
                    auto presented = UiRequire(UiPresentedInteractionState::Create(view, hit->presentation.canvas));
                    Check(presented.Apply(hit->presentation));
                    const auto &placed = *hit->presentation.worldProjection;
                    const auto center = placed.origin + placed.horizontalExtent * 0.5F + placed.verticalExtent * 0.5F;
                    const Math::Ray ray{center + Math::Vec3{0, 0, 1}, {0, 0, -1}, 0, 2};
                    const auto targeted = UiRequire(hit->hit.HitTestWorld({view, hit->presentation.canvas, ray}, presented));
                    REQUIRE(targeted);
                    CHECK(targeted->logicalPosition.x == hit->hit.Descriptor().logicalExtent.width / 2);
                    if (!worldPointer->Pressed())
                        worldPointer->Press(port, sceneId, *hit, ray);
                    previousWorld = hit;
                } else if (previousWorld) {
                    worldPointer->VerifyUnavailable(port, sceneId, *previousWorld);
                    previousWorld.reset();
                }
            }
        })));
        Check(runtime->Startup());
        clock.Advance(Duration::FromMilliseconds(16));
        Check(runtime->RunFrame());
        REQUIRE(view.IsValid());
        CHECK(port.InputEligible(view));
        const auto pass = host.ViewportPass(scene);
        const auto pixels = ReadViewportColor(host, sampler);
        const auto center = (pass.extent.height / 2 * pass.extent.width + pass.extent.width / 2) * 4;
        REQUIRE(center + 2 < pixels.size());
        CHECK(pixels[center + 1] > 240);

        if (deferHeadsetReceipt) {
            verifyInitialAdmission = false;
            clock.Advance(Duration::FromMilliseconds(16));
            Check(runtime->RunFrame());
            CHECK(port.InputEligible(view));
            headsetProjected = false;
            clock.Advance(Duration::FromMilliseconds(16));
            Check(runtime->RunFrame());
            CHECK_FALSE(port.InputEligible(view));
            verifyInitialAdmission = true;
            headsetProjected = true;
        }

        if (mode == UiRenderMode::WorldSpace) {
            const auto retired = view;
            const auto retiredScene = sceneId;
            project.Publish(2, -1);
            sceneId = project.scenes.ActiveScene()->RuntimeId();
            scene.lightSceneDomain = sceneId.value;
            CHECK_FALSE(port.InputEligible(retired));
            CHECK_FALSE(UiRequire(port.ReadPresentedWorldCanvas(retiredScene)));
            clock.Advance(Duration::FromMilliseconds(16));
            Check(runtime->RunFrame());
            CHECK(view != retired);
            CHECK(port.InputEligible(view));
            const auto occluded = ReadViewportColor(host, sampler);
            REQUIRE(center + 2 < occluded.size());
            CHECK(occluded[center] > 240);
            CHECK(occluded[center + 1] < 8);
        }

        draw = false;
        clock.Advance(Duration::FromMilliseconds(16));
        Check(runtime->RunFrame());
        CHECK_FALSE(port.InputEligible(view));
        CHECK_FALSE(UiRequire(port.ReadPresentedWorldCanvas(sceneId)));
        project.scenes.Shutdown();
        runtime->Shutdown();
        host.viewport->ReleaseViewportCanvas();
        Check(host.frontend->ReleaseSampler(sampler));
        CHECK(host.Drain());
        CHECK(UiRequire(port.CollectRetired()));
    }

    /** @brief Uses the current renderer budget to publish and present a live fallback at the original sequence position. */
    void PresentCameraAfterCapacityLoss(Host &host, const RenderCameraView &source, RenderSceneView pressure,
                                        const RenderSamplerHandle sampler) {
        using namespace Cinematic;
        const auto scene = CapacityCameraScene(source, pressure.camera);
        const Runtime::CameraViewContext context{{1, 1}, scene->View().RuntimeId(), Runtime::CameraViewDomain::Runtime};
        const auto admission = [&] {
            const auto capacity = host.viewport->CameraCrossFadeCapacity();
            return Runtime::CameraCrossFadeAdmission{context, capacity.generation, capacity.availableViews};
        };
        auto camera =
            CameraCuts::Take(Runtime::CameraService::Create(context, Runtime::CameraTransitionSupport::SingleViewBlend, admission()));
        auto runtime = CameraCuts::Take(CinematicRuntimeService::Create({{1, 1}}));
        const std::array bindings{CameraCutBinding{{1, 1}, {1}}, CameraCutBinding{{2, 1}, {2}}};
        auto activation = CameraCuts::TransitionActivation(CameraTransitionTier::DualViewCrossFade, CameraBlendCurve::SmoothStep,
                                                           CameraTransitionFallback::LinearThenHardCut, 3);
        auto playback = CameraCuts::Take(
            CinematicCameraPlayback::Activate(runtime, camera, std::move(activation),
                                              {bindings, 8, CameraCutEndPolicy::HoldLastUntilPlayerEnd, 0, {1, 1}}, scene->View()));
        Check(runtime.Play(playback.Player()));
        Check(runtime.Seek(playback.Player(), 6));
        Check(playback.Publish(scene->View()));
        const auto before = CameraCuts::Take(camera.Commit(1, scene->View(), std::nullopt));
        REQUIRE(before.crossFade);

        host.SynchronizeViewport(pressure, Host::CameraQualificationExtent);
        REQUIRE(admission().availableViews == 1);
        Check(camera.RefreshCrossFadeAdmission(admission()));
        Check(playback.Publish(scene->View()));
        const auto after = CameraCuts::Take(camera.Commit(2, scene->View(), std::nullopt));
        CHECK_FALSE(after.crossFade);
        CHECK(after.selected.transitionResolution == Runtime::CameraTransitionResolution::LinearFallback);
        CHECK(after.selected.values.position.x == Catch::Approx(-1.0F / 3.0F));
        CHECK(CameraCuts::Take(runtime.Snapshot(playback.Player())).position == 6);
        pressure.camera = after.selected.values;
        Check(host.viewport->PrepareCameraCrossFadeFrame(std::nullopt));
        const auto pixels = PresentCameraColor(host, pressure, 4, sampler);
        CHECK(CountCameraTint(pixels) > 16);
    }

    TEST_CASE("Native Vulkan publishes an authorized viewport crop without interrupting presentation",
              "[native-smoke][vulkan][gui][viewport][image-capture]") {
        if (!std::getenv("HORO_EDITOR_VULKAN_ASSET_ROOT"))
            SKIP("Supply the locked Vulkan viewport shader package.");
        bool authorized = true;
        Host host;
        host.CreateViewport();
        constexpr EditorViewportImageRegion region{7, 5, 2, 2};
        CHECK(host.viewport
                  ->RequestImageCapture({0, 0, 257, 1}, [] {
            return true;
        }).HasError());
        const auto requested = host.viewport->RequestImageCapture(region, [&] {
            return authorized;
        });
        Check(requested, "request an exact viewport rectangle");
        CHECK(host.viewport
                  ->RequestImageCapture(region, [] {
            return true;
        }).HasError());
        CHECK_FALSE(host.viewport->ReadImageCapture(requested.Value() + 1));
        CHECK_FALSE(host.viewport->CancelImageCapture(requested.Value() + 1));
        REQUIRE(host.viewport->ReadImageCapture(requested.Value())->phase == EditorViewportImagePhase::Pending);
        host.SynchronizeViewport({});
        host.PrepareScene();
        auto pass = host.ViewportPass({});
        pass.clearColor = {1, 0, 0, 1};
        auto frame = host.Frame(1);
        const std::array passes{RenderPassDescriptor{.kind = RenderPassKind::Graphics, .staticMesh = pass},
                                RenderPassDescriptor{.kind = RenderPassKind::Graphics, .primaryOutput = PrimaryOutputAttachment{}}};
        Check(host.viewport->ExecutePreparedFrame(frame, passes), "record viewport and bounded copy");
        const auto view = host.viewport->TextureView();
        REQUIRE(view.IsValid());
        host.ComposeGui(frame, &view);
        Check(frame.Present(), "present the same attachment after the copy");
        host.viewport->FinishColorFrame(true);

        const auto image = WaitForViewportCrop(host, {}, requested.Value());
        REQUIRE(image.phase == EditorViewportImagePhase::Ready);
        REQUIRE(image.artifact);
        CHECK(image.artifact->region == region);
        REQUIRE(image.artifact->rgba.size() == region.Bytes());
        const std::array red{std::byte{255}, std::byte{0}, std::byte{0}, std::byte{255}};
        for (std::size_t offset = 0; offset < region.Bytes(); offset += red.size())
            CHECK(std::ranges::equal(std::span{image.artifact->rgba}.subspan(offset, red.size()), red));
        authorized = false;
        CHECK(host.viewport->ReadImageCapture(requested.Value())->phase == EditorViewportImagePhase::Cancelled);
        CHECK(image.publication.expired());
        CHECK(host.Drain());
    }

    TEST_CASE("Native Vulkan camera cross-fade composes both real viewport rasters before GUI sampling",
              "[native-smoke][vulkan][gui][viewport][cross-fade]") {
        const auto *root = std::getenv("HORO_EDITOR_VULKAN_ASSET_ROOT");
        if (!root || !std::filesystem::exists(std::filesystem::path{root} / "shaders/editor/viewport-cross-fade-vulkan.horoshdr"))
            SKIP("Supply the locked viewport and camera cross-fade shader packages.");
        const CameraViewportFixture scene;
        const auto source = scene.View(-1.0F);
        const auto destination = scene.View(1.0F);
        Host host{false, true};
        host.CreateViewport(destination, nullptr, nullptr, true);
        auto pressure = destination;
        const std::vector<RenderStaticMeshInstance> pressureInstances(RenderMaterialPass::MaximumDraws - 8, scene.instances.front());
        pressure.instances = pressureInstances;
        host.SynchronizeViewport(pressure, Host::CameraQualificationExtent);
        CHECK(host.viewport->CameraCrossFadeCapacity().availableViews == 1);
        host.SynchronizeViewport(destination, Host::CameraQualificationExtent);
        const auto capacity = host.viewport->CameraCrossFadeCapacity();
        REQUIRE(capacity.availableViews == 2);
        const auto sampler = host.frontend->CreateSampler({});
        Check(sampler, "create native readback sampler");
        Check(host.frontend->ProcessResourceRequests(), "prepare native readback sampler");
        const auto a = PresentCameraColor(host, source, 1, sampler.Value().handle);
        const auto b = PresentCameraColor(host, destination, 2, sampler.Value().handle);
        Check(host.viewport->PrepareCameraCrossFadeFrame(EditorViewportCameraCrossFade{source.camera, 0.5F, capacity.generation}));
        const auto blend = PresentCameraColor(host, destination, 3, sampler.Value().handle);
        REQUIRE(a.size() == b.size());
        REQUIRE(a.size() == blend.size());
        const auto sourceTint = CountCameraTint(a);
        const auto destinationTint = CountCameraTint(b);
        std::size_t distinguishable{};
        int maximumError{};
        for (std::size_t index = 0; index < blend.size(); ++index) {
            distinguishable += std::abs(int{a[index]} - int{b[index]}) >= 16;
            maximumError = std::max(maximumError, std::abs(int{blend[index]} * 2 - int{a[index]} - int{b[index]}));
        }
        CAPTURE(sourceTint, destinationTint, distinguishable, maximumError);
        CHECK(sourceTint > 16);
        CHECK(destinationTint > 16);
        CHECK(distinguishable > 16);
        CHECK(maximumError <= 2);
        PresentCameraAfterCapacityLoss(host, source.camera, pressure, sampler.Value().handle);
        Check(host.frontend->ReleaseSampler(sampler.Value().handle), "release readback sampler");
        CHECK(host.Drain());
    }

    TEST_CASE("Native Vulkan partial GUI preparation releases native children before SDL parents", "[native-smoke][vulkan][gui]") {
        Host host;
        REQUIRE(ImGui::GetIO().BackendRendererUserData);
        REQUIRE(host.Drain());
        CHECK(ImGui::GetIO().BackendRendererUserData == nullptr);
        CHECK(host.port->GetInstanceProcAddr() == nullptr);
        CHECK(SDL_GetWindowID(host.window.get()) != 0);
        host.gui.reset();
        CHECK(ImGui::GetIO().BackendPlatformUserData == nullptr);
        CHECK(SDL_GetWindowID(host.window.get()) != 0);
    }

    /** @brief Completes the actual native resize or restore within the qualification deadline. */
    void RealizeNativePresentation(Host &host, const FramebufferExtent output, const std::chrono::steady_clock::time_point deadline) {
        for (;;) {
            const auto resized = host.frontend->Resize(output);
            if (resized.HasValue())
                return;
            const auto &error = resized.ErrorValue();
            INFO("Vulkan / swapchain / realize current output: " << error.domain.Value() << "/" << error.code.Value() << ": "
                                                                 << error.message);
            REQUIRE(error.code.Value() == RenderPresentationErrors::DrawableUnavailable.code.Value());
            REQUIRE(std::chrono::steady_clock::now() < deadline);
            SDL_PumpEvents();
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }
    }

    /** @brief Waits only in qualification for the actual SDL extent and the native surface that admits it. */
    FramebufferExtent ResizeNativePresentation(Host &host, const FramebufferExtent logical) {
        INFO("Vulkan / SDL window / resize requested logical extent to " << logical.width << "x" << logical.height);
        REQUIRE(SDL_SetWindowSize(host.window.get(), static_cast<int>(logical.width), static_cast<int>(logical.height)));
        FramebufferExtent output;
        const auto pollWindow = [&] {
            int width{}, height{};
            INFO(SDL_GetError());
            REQUIRE(SDL_GetWindowSizeInPixels(host.window.get(), &width, &height));
            if (width > 0 && height > 0)
                output = {static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height)};
            return output.IsValid() && output != host.extent;
        };
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
        while (!pollWindow()) {
            REQUIRE(std::chrono::steady_clock::now() < deadline);
            SDL_PumpEvents();
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }
        RealizeNativePresentation(host, output, deadline);
        return output;
    }

    /** @brief Observes an actual window-manager flag change in the finite native test lane. */
    bool WaitForWindowMinimized(SDL_Window *window, const bool minimized) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
        do {
            SDL_PumpEvents();
            if (((SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED) != 0) == minimized)
                return true;
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        } while (std::chrono::steady_clock::now() < deadline);
        return false;
    }

    TEST_CASE("Native Vulkan suspends and resumes GUI presentation across an actual SDL minimize and restore",
              "[native-smoke][vulkan][presentation][suspension]") {
        Host host;
        INFO("Vulkan / SDL window suspend and restore / " << SDL_GetCurrentVideoDriver());
        host.RecordCapabilities();
        {
            auto initial = host.Frame(1);
            host.ExecuteGuiOnly(initial);
            Check(initial.Present(), "present before minimizing");
        }

        if (!SDL_MinimizeWindow(host.window.get()) || !WaitForWindowMinimized(host.window.get(), true))
            SKIP("The window manager did not acknowledge SDL minimization: " << SDL_GetError());
        const auto suspended = host.frontend->PollPresentationAvailability({});
        Check(suspended, "poll the minimized window without a drawable");
        REQUIRE_FALSE(suspended.Value());

        INFO(SDL_GetError());
        REQUIRE(SDL_RestoreWindow(host.window.get()));
        REQUIRE(WaitForWindowMinimized(host.window.get(), false));
        int width{}, height{};
        REQUIRE(SDL_GetWindowSizeInPixels(host.window.get(), &width, &height));
        REQUIRE(width > 0);
        REQUIRE(height > 0);
        const FramebufferExtent restored{static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height)};
        RealizeNativePresentation(host, restored, std::chrono::steady_clock::now() + std::chrono::seconds{5});
        {
            auto frame = host.Frame(2, restored);
            host.ExecuteGuiOnly(frame);
            Check(frame.Present(), "present at the observed restored extent");
        }
        REQUIRE(host.Drain());
        CHECK(host.frontend->MemorySnapshot().allocationCount == 0);
        CHECK(host.port->GetInstanceProcAddr() == nullptr);
        CHECK(SDL_GetWindowID(host.window.get()) != 0);
    }

    TEST_CASE("Native Vulkan presents the active cinematic camera at a new SDL pixel extent",
              "[native-smoke][vulkan][gui][cinematic][resize]") {
        const auto *root = std::getenv("HORO_EDITOR_VULKAN_ASSET_ROOT");
        if (!root || !std::filesystem::exists(std::filesystem::path{root} / "shaders/editor/viewport-cross-fade-vulkan.horoshdr"))
            SKIP("Supply the locked viewport and camera cross-fade shader packages.");
        using namespace Cinematic;
        const CameraViewportFixture fixture;
        auto scene = fixture.View(1.0F);
        auto authored = CapacityCameraScene(fixture.View(-1.0F).camera, scene.camera);
        const Runtime::CameraViewContext context{{1, 1}, authored->View().RuntimeId(), Runtime::CameraViewDomain::Runtime};
        auto camera = CameraCuts::Take(Runtime::CameraService::Create(context));
        auto runtime = CameraCuts::Take(CinematicRuntimeService::Create({{1, 1}}));
        const std::array bindings{CameraCutBinding{{1, 1}, {1}}, CameraCutBinding{{2, 1}, {2}}};
        auto playback = CameraCuts::Take(
            CinematicCameraPlayback::Activate(runtime, camera,
                                              CameraCuts::TransitionActivation(CameraTransitionTier::SingleViewBlend,
                                                                               CameraBlendCurve::Linear, CameraTransitionFallback::Fail, 3),
                                              {bindings, 8, CameraCutEndPolicy::HoldLastUntilPlayerEnd, 0, {1, 1}}, authored->View()));
        Check(runtime.Play(playback.Player()));
        const auto publishCamera = [&](const SequenceTime time, const std::uint64_t frame) {
            Check(runtime.Seek(playback.Player(), time));
            Check(playback.Publish(authored->View()));
            return CameraCuts::Take(camera.Commit(frame, authored->View(), std::nullopt)).selected.values;
        };

        Host host{false, true};
        scene.camera = publishCamera(6, 1);
        CHECK(scene.camera.position.x == Catch::Approx(-1.0F / 3.0F));
        host.CreateViewport(scene, nullptr, nullptr, true);
        auto sampler = host.frontend->CreateSampler({});
        Check(sampler, "create native readback sampler");
        Check(host.frontend->ProcessResourceRequests(), "prepare native readback sampler");
        const auto before = PresentCameraColor(host, scene, 1, sampler.Value().handle);
        const auto initial = host.frontend->PresentationOutput();
        const auto output = ResizeNativePresentation(host, {512, 384});
        const auto retired = host.frontend->PollPresentationAvailability(host.extent);
        Check(retired, "reject the retired SDL extent");
        CHECK_FALSE(retired.Value());
        if (initial.HasValue()) {
            const auto replacement = host.frontend->PresentationOutput();
            Check(replacement, "observe the replacement presentation output");
            CHECK(replacement.Value().revision > initial.Value().revision);
        }
        scene.camera = publishCamera(8, 2);
        CHECK(scene.camera.position.x == 1.0F);
        const auto after = PresentCameraColor(host, scene, 2, sampler.Value().handle, output);
        CHECK(before.size() == after.size());
        CHECK(before != after);
        CHECK(CountCameraTint(before) > 16);
        CHECK(CountCameraTint(after) > 16);
        Check(host.frontend->ReleaseSampler(sampler.Value().handle), "release readback sampler");
        CHECK(host.Drain());
    }

    TEST_CASE("Native Vulkan keeps frame ownership and resumes presentation after an observed SDL pixel resize",
              "[native-smoke][vulkan][presentation]") {
        Host host;
        INFO("Vulkan / SDL window and swapchain / " << SDL_GetCurrentVideoDriver());
        host.RecordCapabilities();
        const auto device = host.bridge->Port()->QueryDevice();
        Check(device, "query realized native attachment");
        const auto initialOutput = host.frontend->PresentationOutput();
        if (device.Value().outputFormat == VK_FORMAT_B8G8R8A8_UNORM) {
            Check(initialOutput, "observe unmanaged SDR attachment");
            CHECK(initialOutput.Value().active == RenderOutputColor{});
            CHECK(initialOutput.Value().display.attached);
            CHECK(initialOutput.Value().display.hdr == RenderDisplayHdrSupport::Unknown);
        } else {
            REQUIRE(initialOutput.HasError());
            CHECK(initialOutput.ErrorValue().code.Value() == RenderPresentationErrors::OutputUnsupported.code.Value());
        }
        {
            auto initial = host.Frame(1);
            host.ExecuteGuiOnly(initial);
            Check(initial.Present(), "present initial SDR output");
        }
        constexpr FramebufferExtent logical{512, 384};
        {
            auto active = host.Frame(2);
            const auto rejected = host.frontend->Resize(logical);
            REQUIRE(rejected.HasError());
            CHECK(rejected.ErrorValue().code.Value() == "render.frontend.resize_during_frame");
            active.Cancel();
        }
        const auto suspended = host.frontend->PollPresentationAvailability({});
        Check(suspended, "poll without a drawable extent");
        CHECK_FALSE(suspended.Value());
        {
            const auto output = ResizeNativePresentation(host, logical);
            if (initialOutput.HasValue()) {
                const auto resizedOutput = host.frontend->PresentationOutput();
                Check(resizedOutput, "observe replacement SDR attachment");
                CHECK(resizedOutput.Value().revision > initialOutput.Value().revision);
                CHECK(resizedOutput.Value().active == initialOutput.Value().active);
            }
            const auto retiredExtent = host.frontend->PollPresentationAvailability(host.extent);
            Check(retiredExtent, "poll the superseded pixel extent");
            CHECK_FALSE(retiredExtent.Value());
            auto resumed = host.Frame(3, output);
            host.ExecuteGuiOnly(resumed);
            Check(resumed.Present(), "present the resized SDR output");
        }
        REQUIRE(host.Drain());
        CHECK(host.frontend->MemorySnapshot().allocationCount == 0);
        CHECK(host.port->GetInstanceProcAddr() == nullptr);
        CHECK(SDL_GetWindowID(host.window.get()) != 0);
    }

    TEST_CASE("Native Vulkan manual ColorOutput preserves graph acceptance and publishes the encoded GUI view",
              "[native-smoke][vulkan][gui][manual-color]") {
        const auto *root = std::getenv("HORO_TEST_VULKAN_COLOR_OUTPUT_PACKAGE");
        if (!root)
            SKIP("Supply HORO_TEST_VULKAN_COLOR_OUTPUT_PACKAGE with an actual Vulkan manual ColorOutput generation.");
        Host host;
        const auto presentation = host.frontend->PresentationOutput();
        if (presentation.HasError() && presentation.ErrorValue().code.Value() == RenderPresentationErrors::OutputUnsupported.code.Value())
            SKIP("Selected surface lacks the qualified BGRA8 unmanaged SDR attachment.");
        Check(presentation);

        auto package = Render::Internal::LoadColorOutputCookPackage(std::filesystem::path{root}, ShaderTargetBackend::Vulkan);
        Check(package, "load exact Vulkan manual color package");
        const EditorViewportColorInputs color{
            .package = std::make_shared<const Render::Internal::ColorOutputCookPackage>(std::move(package).Value())};
        host.CreateViewport({}, &color);
        CHECK(host.viewport->ColorDiagnostics().active);
        CHECK_FALSE(host.viewport->ColorDiagnostics().automatic);
        CHECK_FALSE(host.viewport->TextureView().IsValid());
        const auto rejectedSetting = host.viewport->RequestColorSettings({.mode = ExposureMode::AutomaticHistogram});
        REQUIRE(rejectedSetting.HasError());
        CHECK(rejectedSetting.ErrorValue().code.Value() == ExposureGpuErrors::Unsupported.code.Value());
        {
            host.PrepareScene();
            auto frame = host.Frame(1);
            host.ExecuteSceneAndGui(frame);
            CHECK(host.viewport->ColorDiagnostics().acceptedSubmission == 0);
            frame.Cancel();
            host.viewport->FinishColorFrame(false);
            CHECK_FALSE(host.viewport->TextureView().IsValid());
            CHECK(host.viewport->ColorDiagnostics().acceptedSubmission == 0);
        }
        {
            host.viewport->RequestGrid({.visible = true});
            host.PrepareScene();
            auto frame = host.Frame(2);
            host.ExecuteSceneAndGui(frame);
            Check(frame.Present(), "submit scene-linear output and GUI tail");
            host.viewport->FinishColorFrame(false);
            CHECK(host.viewport->ColorDiagnostics().acceptedSubmission == 1);
            CHECK(host.viewport->TextureView().IsValid());
        }
        {
            host.PrepareScene();
            auto frame = host.Frame(3);
            host.ExecuteSceneAndGui(frame);
            Check(frame.Present(), "resample the accepted analytic tables without another upload");
            host.viewport->FinishColorFrame(true);
            CHECK(host.viewport->ColorDiagnostics().acceptedSubmission == 2);
        }
        REQUIRE(host.Drain());
    }

    TEST_CASE("Native Vulkan viewport composes signed spatial and ColorOutput in its ordinary GUI frame",
              "[native-smoke][vulkan][gui][signed-spatial]") {
        const auto *productRoot = std::getenv("HORO_TEST_VULKAN_SPATIAL_PRODUCT_ROOT");
        const auto *trustedPublishers = std::getenv("HORO_TEST_VULKAN_SPATIAL_PUBLISHER_TRUST");
        const auto *colorRoot = std::getenv("HORO_TEST_VULKAN_COLOR_OUTPUT_PACKAGE");
        if (!productRoot || !trustedPublishers || !colorRoot)
            SKIP("Supply the explicit signed spatial product, independent trust document, and cooked Vulkan ColorOutput package.");
        Host host{true};
        if (!host.frontend->Capabilities().support.features.Supports(RenderCapability::NativeReconstruction))
            SKIP("Selected Vulkan device cannot realize a native reconstruction graph.");
        const auto output = host.frontend->PresentationOutput();
        if (output.HasError() && output.ErrorValue().code.Value() == RenderPresentationErrors::OutputUnsupported.code.Value())
            SKIP("Selected surface lacks the qualified BGRA8 unmanaged SDR attachment.");
        Check(output);

        using enum Release::DistributionPlatform;
#ifdef _WIN32
        constexpr auto platform = Windows;
#else
        constexpr auto platform = Linux;
#endif
#if defined(_M_ARM64) || defined(__aarch64__)
        constexpr auto architecture = Release::DistributionArchitecture::Arm64;
#else
        constexpr auto architecture = Release::DistributionArchitecture::X64;
#endif
        auto version = Release::ParseReleaseVersion(HORO_ENGINE_VERSION_STRING);
        Check(version);
        const EditorProviderProductIdentity product{Release::EngineProductVersion{std::move(version).Value()}, platform, architecture};
        const EditorImageProviderInspection request{ReconstructionKind::Spatial, productRoot, trustedPublishers, true};
        const NativeDurableFileSystem files;
        const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(SystemWallClock{}.UtcNow().time_since_epoch()).count();
        REQUIRE(now >= 0);
        const auto trust = ReadEditorImageProviderTrust(files, request.trustedPublishers, static_cast<std::uint64_t>(now));
        Check(trust);
        const auto installed = LoadEditorSignedSpatialProvider(*host.frontend, request, product, files, trust.Value());
        Check(installed, "verify signed spatial kernel and scene-linear material");
        auto package = Render::Internal::LoadColorOutputCookPackage(colorRoot, ShaderTargetBackend::Vulkan);
        Check(package, "load exact Vulkan manual ColorOutput package");
        const EditorViewportColorInputs inputs{
            .package = std::make_shared<const Render::Internal::ColorOutputCookPackage>(std::move(package).Value())};

        host.CreateViewport({}, &inputs, &installed.Value());
        host.viewport->RequestGrid({.visible = true});
        host.PrepareScene();
        auto frame = host.Frame(1);
        host.ExecuteSceneAndGui(frame);
        Check(frame.Present(), "submit signed spatial, analytic, overlay, encode and GUI");
        host.viewport->FinishColorFrame(false);
        CHECK(host.viewport->ColorDiagnostics().acceptedSubmission == 1);
        const auto original = host.viewport->TextureView();
        REQUIRE(original.IsValid());

        constexpr EditorViewportExtent resized{360, 256};
        host.viewport->RequestExtent(resized);
        host.SynchronizeViewport({}, resized);
        CHECK_FALSE(host.viewport->TextureView().IsValid());
        host.PrepareScene();
        auto replacement = host.Frame(2);
        host.ExecuteSceneAndGui(replacement);
        Check(replacement.Present(), "submit the resized spatial and display generation");
        host.viewport->FinishColorFrame(true);
        CHECK(host.viewport->ColorDiagnostics().acceptedSubmission == 2);
        CHECK(host.viewport->TextureView().sampledView != original.sampledView);
        REQUIRE(host.Drain());
    }

    /** @brief Observes the two guide draws and the scene draw, then settles the real frame attempt. */
    void VerifyTemporalGuideSubmission(Host &host, const RenderSceneView &scene, const std::uint64_t frameId, const bool submit) {
        host.PrepareScene(scene);
        constexpr std::array objectIds{RenderMotionObjectId{1}};
        Check(host.viewport->PrepareTemporalMotion({1, objectIds}));
        auto frame = host.Frame(frameId);
        host.viewportObservation->ResetDraws();
        const std::array passes{RenderPassDescriptor{.kind = RenderPassKind::Graphics, .staticMesh = host.ViewportPass(scene)},
                                RenderPassDescriptor{.kind = RenderPassKind::Graphics, .primaryOutput = PrimaryOutputAttachment{}}};
        Check(host.viewport->ExecutePreparedFrame(frame, passes));
        CHECK(host.viewportObservation->DrawCounts() == std::array<std::uint32_t, 3>{3, 3, 0});
        const auto texture = host.viewport->TextureView();
        REQUIRE(texture.IsValid());
        host.ComposeGui(frame, &texture);
        if (submit)
            Check(frame.Present(), "submit both guide draws and the ordinary viewport");
        else
            frame.Cancel();
        host.viewport->FinishColorFrame(submit);
    }

    /** @brief Decodes one compute stage from the actual release cook using its canonical shader contract. */
    CookedNativeReconstructionKernel ReadViewportTemporalKernel(const std::filesystem::path &stage, const NativeReconstructionEntry entry) {
        using namespace NativeReconstructionRelease;
        const NativeDurableFileSystem files;
        const auto prefix = Prefix(entry);
        auto payload = files.ReadFileBounded(stage / (prefix + ".horoshdr"), MaximumPayloadBytes);
        Check(payload, "read the cooked viewport compute payload");
        const auto reflection = files.ReadFileBounded(stage / (prefix + ".reflection.json"), MaximumReflectionBytes);
        Check(reflection, "read the cooked viewport compute reflection");
        const auto contract = DescribeNativeReconstructionShader(entry, Target);
        Check(contract);
        auto metadata = DecodeReflectedShaderArtifact(contract.Value().manifest, Target, payload.Value(), reflection.Value());
        Check(metadata, "decode cooked viewport compute metadata");
        CompiledShaderArtifact artifact{.backend = Target.backend,
                                        .payloadFormat = Target.payloadFormat,
                                        .artifactKey = metadata.Value().compiledArtifactKey,
                                        .payload = std::move(payload).Value(),
                                        .reflection = std::make_shared<const ReflectedShaderArtifact>(std::move(metadata).Value())};
        return {entry, Target, std::move(artifact)};
    }

    TEST_CASE("Native Vulkan admits the cooked temporal guide pair through the viewport owner",
              "[native-smoke][vulkan][gui][temporal-guides]") {
        const auto *assets = SDL_getenv("HORO_EDITOR_VULKAN_ASSET_ROOT");
        const auto *stage = SDL_getenv("HORO_TEST_EDITOR_TEMPORAL_STAGE");
        if (!assets || !stage)
            SKIP("Supply the cooked Vulkan viewport assets and the temporal reconstruction cook stage.");
        Host host{true, true};
        if (!host.frontend->Capabilities().support.features.Supports(RenderCapability::NativeReconstruction))
            SKIP("Selected Vulkan device cannot realize native reconstruction guides.");
        auto cooked = LoadPackagedVulkanViewportShader(std::filesystem::path{assets});
        Check(cooked, "load exact viewport shaders");
        host.viewport = std::make_unique<EditorViewportRendererVulkan>(*host.frontend, *host.bridge, std::move(cooked).Value());
        Check(host.viewport->Initialize(), "initialize viewport before guide preparation");
        const auto root = std::filesystem::path{stage} / "assets/shaders/editor";
        const NativeDurableFileSystem files;
        const auto loadGuidePart = [&](std::string_view name) {
            auto bytes = files.ReadFileBounded(root / name, 16U * 1024U * 1024U);
            Check(bytes, name);
            return std::move(bytes).Value();
        };
        const auto depth = loadGuidePart("viewport-linear-depth-vulkan.horoshdr");
        const auto depthMetadata = loadGuidePart("viewport-linear-depth-vulkan.reflection.json");
        const auto motion = loadGuidePart("viewport-motion-vulkan.horoshdr");
        const auto motionMetadata = loadGuidePart("viewport-motion-vulkan.reflection.json");
        const std::array kernels{ReadViewportTemporalKernel(stage, NativeReconstructionEntry::TemporalHistory),
                                 ReadViewportTemporalKernel(stage, NativeReconstructionEntry::TemporalSeed)};
        const auto incomplete =
            host.viewport->PrepareTemporalGuidePrograms({depth, depthMetadata}, {motion, motionMetadata}, std::span{kernels}.first(1));
        REQUIRE(incomplete.HasError());
        CHECK(incomplete.ErrorValue().code.Value() == NativeReconstructionErrors::InvalidArtifact.code.Value());
        REQUIRE(host.viewport->PrepareTemporalGuidePrograms({depth, depthMetadata}, {motion, depthMetadata}, kernels).HasError());
        Check(host.viewport->PrepareTemporalGuidePrograms({depth, depthMetadata}, {motion, motionMetadata}, kernels),
              "prepare the complete native temporal family after rejecting incomplete and mismatched candidates");
        const auto view = host.frontend->CreateRenderViewIdentity();
        Check(view);
        Check(host.viewport->BindLightingViewIdentity(view.Value()));
        const EditorViewportLightingProfile lighting;
        const auto selected = host.frontend->ResolveLightPreparation(lighting.requested, lighting.fallback, {});
        Check(selected);
        Check(host.viewport->RequestLightingProfile(lighting, selected.Value()));
        CameraViewportFixture fixture;
        const std::array<std::uint32_t, 6> indices{0, 1, 2, 0, 1, 2};
        fixture.meshes.front().indices = indices;
        auto scene = fixture.View(0);
        scene.lights = {};
        host.viewport->RequestExtent({320, 240});
        host.viewport->RequestGrid({.visible = false});
        host.SynchronizeViewport(scene);
        std::vector<RenderStaticMeshInstance> oversized(RenderMaterialPass::MaximumDraws + 1, scene.instances.front());
        auto excessive = scene;
        excessive.instances = oversized;
        const auto refused = host.viewport->PrepareResources(*host.frontend, excessive);
        REQUIRE(refused.HasError());
        CHECK(refused.ErrorValue().code.Value() == RendererErrors::ViewportInvalidScene.code.Value());
        Check(host.viewport->PrepareResources(*host.frontend, scene), "preserve the previously prepared guide draw slots");
        VerifyTemporalGuideSubmission(host, scene, 1, true);
        fixture.instances.front().localToWorld.values[12] = 1.0F;
        VerifyTemporalGuideSubmission(host, scene, 2, false);
        fixture.instances.front().localToWorld.values[12] = 2.0F;
        VerifyTemporalGuideSubmission(host, scene, 3, true);
        REQUIRE(host.Drain());
    }

    TEST_CASE("Native Vulkan viewport resolves temporal color before display encoding", "[native-smoke][vulkan][gui][temporal-color]") {
        const bool upscale = GENERATE(false, true);
        CAPTURE(upscale);
        const auto *assets = std::getenv("HORO_EDITOR_VULKAN_ASSET_ROOT");
        const auto *stage = std::getenv("HORO_TEST_EDITOR_TEMPORAL_STAGE");
        const auto *colorRoot = std::getenv("HORO_TEST_VULKAN_COLOR_OUTPUT_PACKAGE");
        if (!assets || !stage || !colorRoot)
            SKIP("Supply the viewport, temporal and Vulkan color packages.");
        Host host{true, true};
        if (!host.frontend->Capabilities().support.features.Supports(RenderCapability::NativeReconstruction))
            SKIP("The selected Vulkan device cannot realize native reconstruction.");
        auto cooked = LoadPackagedVulkanViewportShader(assets);
        Check(cooked);
        host.viewport = std::make_unique<EditorViewportRendererVulkan>(*host.frontend, *host.bridge, std::move(cooked).Value());
        Check(host.viewport->Initialize());
        const NativeDurableFileSystem files;
        const auto root = std::filesystem::path{stage} / "assets/shaders/editor";
        const auto read = [&](std::string_view name) {
            auto bytes = files.ReadFileBounded(root / name, NativeReconstructionRelease::MaximumPayloadBytes);
            Check(bytes, name);
            return std::move(bytes).Value();
        };
        const auto depth = read("viewport-linear-depth-vulkan.horoshdr");
        const auto depthLayout = read("viewport-linear-depth-vulkan.reflection.json");
        const auto motion = read("viewport-motion-vulkan.horoshdr");
        const auto motionLayout = read("viewport-motion-vulkan.reflection.json");
        auto sceneCook = LoadEditorVulkanSceneLinearCook(read("viewport-scene-linear-vulkan.horoshdr"),
                                                         read("viewport-scene-linear-vulkan.reflection.json"));
        Check(sceneCook);
        auto package = Render::Internal::LoadColorOutputCookPackage(colorRoot, ShaderTargetBackend::Vulkan);
        Check(package);
        const EditorViewportColorInputs inputs{
            .package = std::make_shared<const Render::Internal::ColorOutputCookPackage>(std::move(package).Value())};
        const std::array kernels{ReadViewportTemporalKernel(stage, NativeReconstructionEntry::TemporalSeed),
                                 ReadViewportTemporalKernel(stage, NativeReconstructionEntry::TemporalHistory)};
        const auto fallback = ReadViewportTemporalKernel(stage, NativeReconstructionEntry::Spatial);
        Check(host.viewport->PrepareTemporalColor(std::move(sceneCook).Value(), inputs, {}, {depth, depthLayout}, {motion, motionLayout},
                                                  kernels, upscale ? &fallback : nullptr));
        const auto view = host.frontend->CreateRenderViewIdentity();
        Check(view);
        Check(host.viewport->BindLightingViewIdentity(view.Value()));
        const EditorViewportLightingProfile lighting;
        const auto selected = host.frontend->ResolveLightPreparation(lighting.requested, lighting.fallback, {});
        Check(selected);
        Check(host.viewport->RequestLightingProfile(lighting, selected.Value()));
        CameraViewportFixture fixture;
        auto scene = fixture.View(0);
        scene.lights = {};
        constexpr auto size = Host::CameraQualificationExtent;
        if (upscale)
            scene.camera.position.y = 1.5F;
        host.viewport->RequestExtent(size);
        host.viewport->RequestGrid({.visible = upscale});
        host.SynchronizeViewport(scene, size);
        constexpr std::array objectIds{RenderMotionObjectId{1}};
        const auto sampler = host.frontend->CreateSampler({});
        Check(sampler);
        Check(host.frontend->ProcessResourceRequests());
        const auto waitIdle = host.viewportObservation->DeviceFunction<PFN_vkQueueWaitIdle>("vkQueueWaitIdle");
        for (std::uint64_t number = 1; number <= 5; ++number) {
            CAPTURE(number);
            constexpr std::uint64_t fallbackFrame = 3;
            const bool tracked = number != fallbackFrame;
            if (tracked)
                Check(host.viewport->PrepareTemporalMotion({1, objectIds}));
            Check(host.viewport->PrepareTemporalFrame(host.ViewportPass(scene), number));
            host.PrepareScene(scene);
            auto frame = host.Frame(number);
            host.viewportObservation->ResetDraws();
            host.ExecuteSceneAndGui(frame, scene);
            CHECK(host.viewportObservation->DispatchCount() == (tracked || upscale ? 1 : 0));
            Check(frame.Present(), "submit the viewport color and GUI");
            host.viewport->FinishColorFrame(true);
            CHECK(host.viewport->ColorDiagnostics().acceptedSubmission == number);
            REQUIRE(waitIdle(host.viewportObservation->Queue()) == VK_SUCCESS);
            Check(host.frontend->ProcessResourceRequests());
        }
        const auto pixels = ReadViewportColor(host, sampler.Value().handle);
        REQUIRE(pixels.size() == size.width * size.height * 4);
        const auto center = (size.width * (size.height / 2) + (size.width / 2)) * 4;
        CHECK(pixels[center + 3] == 255);
        CHECK_FALSE(std::ranges::equal(std::span{pixels}.first(3), std::span{pixels}.subspan(center, 3)));
        REQUIRE(host.Drain());
    }

    TEST_CASE("Native Vulkan replaces a font atlas after presentation and preserves the active atlas on rejection",
              "[native-smoke][vulkan][gui][font-atlas]") {
        Host host;
        auto &io = ImGui::GetIO();
        const auto originalIdentity = io.Fonts->TexID;
        {
            auto frame = host.Frame(1);
            host.ExecuteGuiOnly(frame);
            Check(frame.Present());
        }
        using Atlas = std::unique_ptr<ImFontAtlas, decltype(&IM_DELETE<ImFontAtlas>)>;
        Atlas replacement{IM_NEW(ImFontAtlas)(), &IM_DELETE<ImFontAtlas>};
        ImFontConfig config;
        config.RasterizerDensity = 1.5F;
        auto *font = replacement->AddFontDefault(&config);
        REQUIRE(font);
        CHECK(ReplaceEditorFontAtlas(*host.gui, *replacement).HasError());
        CHECK(io.Fonts->TexID == originalIdentity);
        REQUIRE(replacement->Build());
        Check(ReplaceEditorFontAtlas(*host.gui, *replacement), "upload replacement atlas");
        CHECK(replacement->TexID != originalIdentity);
        CHECK(replacement->TexID != 0);
        Atlas previous{std::exchange(io.Fonts, replacement.release()), &IM_DELETE<ImFontAtlas>};
        io.FontDefault = font;
        {
            auto frame = host.Frame(2);
            host.ExecuteGuiOnly(frame);
            previous.reset();
            CHECK(ImGui::GetIO().FontDefault == font);
            Check(frame.Present());
        }
        REQUIRE(host.Drain());
        host.gui.reset();
        CHECK(host.frontend->MemorySnapshot().allocationCount == 0);
    }

    TEST_CASE("Native Vulkan empty viewport GUI tail and cancellation share canonical retirement", "[native-smoke][vulkan][gui]") {
        Host host;
        host.RecordCapabilities();
        host.CreateViewport();
        {
            host.PrepareScene();
            auto frame = host.Frame(1);
            host.ExecuteSceneAndGui(frame);
            Check(frame.Present());
        }
        {
            host.PrepareScene();
            auto frame = host.Frame(2);
            host.ExecuteSceneAndGui(frame);
            frame.Cancel();
        }
        REQUIRE(host.Drain());
        CHECK(ImGui::GetIO().BackendRendererUserData == nullptr);
        CHECK_FALSE(host.viewport->IsReady());
        CHECK(host.frontend->MemorySnapshot().allocationCount == 0);
        host.viewport.reset();
        host.gui.reset();
        CHECK(ImGui::GetIO().BackendPlatformUserData == nullptr);
        CHECK(SDL_GetWindowID(host.window.get()) != 0);
    }

    TEST_CASE("Native Vulkan grid selected light and mixed lighting retire together across presented and cancelled frames",
              "[native-smoke][vulkan][gui][viewport]") {
        const auto curve = GENERATE(std::optional<Cinematic::CameraBlendCurve>{}, std::optional{Cinematic::CameraBlendCurve::Linear},
                                    std::optional{Cinematic::CameraBlendCurve::SmoothStep});
        std::optional<CameraCuts::NativeCameraPlayback> cinematic;
        if (curve)
            cinematic.emplace(*curve);
        const std::array vertices{MeshVertex{{-1, -1, 0}, {0, 0, 1}, {0, 0}}, MeshVertex{{1, -1, 0}, {0, 0, 1}, {1, 0}},
                                  MeshVertex{{0, 1, 0}, {0, 0, 1}, {0.5F, 1}}};
        const std::array<std::uint32_t, 3> indices{0, 1, 2};
        const RenderMeshSourceHandle source{{1}, 1};
        const std::array meshes{RenderMeshResourceView{source, vertices, indices, {{-1, -1, 0}, {1, 1, 0}}}};
        const std::array instances{RenderStaticMeshInstance{.mesh = source}};
        const std::array lights{RenderLight{.stableIdentity = 1},
                                RenderLight{.kind = RenderLightKind::Point, .position = {2, 3, 4}, .stableIdentity = 2},
                                RenderLight{.kind = RenderLightKind::Spot, .position = {0, 0, 3}, .stableIdentity = 3}};
        RenderSceneView scene{.camera = {.position = {0, 2, 5}, .target = {0, 0, 0}},
                              .meshResources = meshes,
                              .instances = instances,
                              .lights = lights,
                              .lightSceneDomain = 1,
                              .lightSnapshotRevision = 1};
        if (cinematic) {
            scene.camera = cinematic->Advance(0);
            const float expectedInitialZ = *curve == Cinematic::CameraBlendCurve::Linear ? 3.6666667F : 3.5185185F;
            CHECK(scene.camera.position.z == Catch::Approx(expectedInitialZ));
        }
        REQUIRE(scene.IsValid());
        Host host;
        host.RecordCapabilities();
        host.CreateViewport(scene);
        REQUIRE(host.frontend->Capabilities().supportsMaterialRasterGraph);
        CHECK(host.viewport->PrepareLightingFrame(host.ViewportPass(scene), {}).HasError());
        const auto firstTexture = host.viewport->TextureView().sampledView;
        for (std::uint64_t number = 1; number <= 6; ++number) {
            if (number == 2 && cinematic)
                scene.camera = cinematic->Advance(2);
            if (number == 3) {
                const auto previous = host.ViewportPass(scene);
                constexpr EditorViewportExtent resized{384, 256};
                host.viewport->RequestExtent(resized);
                host.SynchronizeViewport(scene, resized);
                CHECK(host.viewport->TextureView().sampledView != firstTexture);
                CHECK(host.viewport->PrepareLightingFrame(previous, host.viewport->LightingViewIdentity()).HasError());
            }
            const bool overlays = number % 2 != 0;
            host.viewport->RequestGrid({.visible = overlays});
            host.viewport->RequestLightVisualizer({overlays ? std::optional{lights[1]} : std::nullopt});
            host.PrepareScene(scene);
            auto frame = host.Frame(number);
            CHECK(host.viewport->PrepareLightingFrame(host.ViewportPass(scene), host.viewport->LightingViewIdentity()).HasError());
            host.ExecuteSceneAndGui(frame, scene);
            if (number % 2 == 0)
                frame.Cancel();
            else
                Check(frame.Present());
        }
        if (cinematic)
            CHECK(cinematic->Position() == 8);
        REQUIRE(host.Drain());
        CHECK_FALSE(host.viewport->IsReady());
        CHECK(host.frontend->MemorySnapshot().allocationCount == 0);
    }
}  // namespace Horo::Tests::VulkanEditor
